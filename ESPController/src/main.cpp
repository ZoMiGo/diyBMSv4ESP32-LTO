/*

 ____  ____  _  _  ____  __  __  ___    _  _  __
(  _ \(_  _)( \/ )(  _ \(  \/  )/ __)  ( \/ )/. |
 )(_) )_)(_  \  /  ) _ < )    ( \__ \   \  /(_  _)
(____/(____) (__) (____/(_/\/\_)(___/    \/   (_)

  (c) 2017/18/19/20 Stuart Pittaway

  This is the code for the controller - it talks to the V4.X cell modules over isolated serial bus

  This code runs on ESP-8266 WEMOS D1 PRO and compiles with VS CODE and PLATFORM IO environment
*/
/*
*** NOTE IF YOU GET ISSUES WHEN COMPILING IN PLATFORM.IO ***
ERROR: "ESP Async WebServer\src\WebHandlers.cpp:67:64: error: 'strftime' was not declared in this scope"
Delete the file <project folder>\diyBMSv4\ESPController\.pio\libdeps\esp8266_d1minipro\Time\Time.h
The Time.h file in this library conflicts with the time.h file in the ESP core platform code

See reasons why here https://github.com/me-no-dev/ESPAsyncWebServer/issues/60
*/
/*
   DIAGRAM
   https://www.hackster.io/Aritro/getting-started-with-esp-nodemcu-using-arduinoide-aa7267
*/

// PacketSerial library takes 1691ms round trip with 8 modules, 212ms per module @ 2400baud

#include <Arduino.h>

//#define PACKET_LOGGING_RECEIVE
//#define PACKET_LOGGING_SEND
//#define RULES_LOGGING
//#define MQTT_LOGGING

#include "FS.h"

//Libraries just for ESP8266
#if defined(ESP8266)
#include <TimeLib.h>
#include <ESP8266WiFi.h>
#include <NtpClientLib.h>
#include <LittleFS.h>
#endif

//Libraries just for ESP32
#if defined(ESP32)
#include <SPIFFS.h>
#include <WiFi.h>
#include <SPI.h>
#include "time.h"
#include <esp_wifi.h>
#include "tft_splash_image.h"
#endif

#if defined(ESP32)
/*
#define USER_SETUP_LOADED

#define USE_DMA_TO_TFT
// Color depth has to be 16 bits if DMA is used to render image
#define COLOR_DEPTH 16
#define ILI9341_DRIVER
//#define SPI_FREQUENCY 40000000
//#define SPI_READ_FREQUENCY 20000000
//#define SPI_TOUCH_FREQUENCY 2500000

#define LOAD_GLCD  // Font 1. Original Adafruit 8 pixel font needs ~1820 bytes in FLASH
#define LOAD_FONT2 // Font 2. Small 16 pixel high font, needs ~3534 bytes in FLASH, 96 characters
#define LOAD_FONT4 // Font 4. Medium 26 pixel high font, needs ~5848 bytes in FLASH, 96 characters
#define LOAD_FONT6 // Font 6. Large 48 pixel font, needs ~2666 bytes in FLASH, only characters 1234567890:-.apm
#define LOAD_FONT7 // Font 7. 7 segment 48 pixel font, needs ~2438 bytes in FLASH, only characters 1234567890:.
#define LOAD_FONT8 // Font 8. Large 75 pixel font needs ~3256 bytes in FLASH, only characters 1234567890:-.
#define LOAD_GFXFF // FreeFonts. Include access to the 48 Adafruit_GFX free fonts FF1 to FF48 and custom fonts

//#define SMOOTH_FONT

//#define TFT_MISO 12
//#define TFT_MOSI 13
//#define TFT_SCLK 14
#define TFT_DC 15  // Data Command control pin
#define TFT_RST -1 // Reset pin (could connect to RST pin)

#define USE_HSPI_PORT
#define SUPPORT_TRANSACTIONS

//Our CS pin is directly connected to ground as the TFT display is the only item on the HSPI bus
#undef TFT_CS
*/
#include "TFT_eSPI.h"
TFT_eSPI tft = TFT_eSPI();
#endif

//Shared libraries across processors
#include <Ticker.h>
#include <ESPAsyncWebServer.h>
#include <AsyncMqttClient.h>
#include <SerialEncoder.h>
#include <cppQueue.h>

#include "defines.h"

#include <ArduinoOTA.h>

#if defined(ESP8266)
#include "HAL_ESP8266.h"
HAL_ESP8266 hal;
#endif

#if defined(ESP32)
#include "HAL_ESP32.h"
HAL_ESP32 hal;
#endif

#if defined(ESP32)
#include <XPT2046_Touchscreen.h>
XPT2046_Touchscreen touchscreen(TOUCH_CHIPSELECT, TOUCH_IRQ); // Param 2 - Touch IRQ Pin - interrupt enabled polling
#endif


#include "Rules.h"

volatile bool emergencyStop = false;
volatile bool WifiDisconnected = true;

Rules rules;

diybms_eeprom_settings mysettings;
uint16_t ConfigHasChanged = 0;

uint16_t TotalNumberOfCells() { return mysettings.totalNumberOfBanks * mysettings.totalNumberOfSeriesModules; }

bool server_running = false;
RelayState previousRelayState[RELAY_TOTAL];
bool previousRelayPulse[RELAY_TOTAL];

volatile enumInputState InputState[INPUTS_TOTAL];

#if defined(ESP8266)
bool NTPsyncEventTriggered = false; // True if a time even has been triggered
NTPSyncEvent_t ntpEvent;            // Last triggered event
#endif

AsyncWebServer server(80);

#if defined(ESP32)
static TaskHandle_t i2c_task_handle = NULL;
static TaskHandle_t ledoff_task_handle = NULL;
static TaskHandle_t wifiresetdisable_task_handle = NULL;
static QueueHandle_t queue_i2c = NULL;
#endif

//This large array holds all the information about the modules
//up to 4x16
CellModuleInfo cmi[maximum_controller_cell_modules];

#include "crc16.h"

#include "settings.h"
#include "SoftAP.h"
#include "DIYBMSServer.h"
#include "PacketRequestGenerator.h"
#include "PacketReceiveProcessor.h"

// Instantiate queue to hold packets ready for transmission
cppQueue requestQueue(sizeof(PacketStruct), 16, FIFO);

cppQueue replyQueue(sizeof(PacketStruct), 8, FIFO);

PacketRequestGenerator prg = PacketRequestGenerator(&requestQueue);

PacketReceiveProcessor receiveProc = PacketReceiveProcessor();

// Memory to hold in and out serial buffer
uint8_t SerialPacketReceiveBuffer[2 * sizeof(PacketStruct)];

SerialEncoder myPacketSerial;

#if defined(ESP8266)
WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;
#endif

Ticker myTimerRelay;
Ticker myTimer;
Ticker myTransmitTimer;
Ticker myReplyTimer;
Ticker myLazyTimer;
Ticker wifiReconnectTimer;
Ticker mqttReconnectTimer;
Ticker myTimerSendMqttPacket;
Ticker myTimerSendMqttStatus;
Ticker myTimerSendInfluxdbPacket;
Ticker myTimerSwitchPulsedRelay;

uint16_t sequence = 0;

ControllerState ControlState = ControllerState::Unknown;

//These need to be removed/replaced/fixed
bool OutputsEnabled;
//These need to be removed/replaced/fixed
bool InputsEnabled;

AsyncMqttClient mqttClient;

#if defined(ESP32)

void QueueLED(uint8_t bits)
{
  i2cQueueMessage m;
  //3 = LED
  m.command = 0x03;
  //Lowest 3 bits are RGB led GREEN/RED/BLUE
  m.data = bits & B00000111;
  xQueueSendToBack(queue_i2c, &m, 10 / portTICK_PERIOD_MS);
}

//Disable the BOOT button from acting as a WIFI RESET
//button which clears the EEPROM settings for WIFI connection
void wifiresetdisable_task(void *param)
{
  for (;;)
  {
    //Wait until this task is triggered https://www.freertos.org/ulTaskNotifyTake.html
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    //Wait for 20 seconds before disabling button/pin
    for (size_t i = 0; i < 20; i++)
    {
      //Wait 1 second
      vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    hal.SwapGPIO0ToOutput();
  }

  //vTaskDelete( NULL );
}

void ledoff_task(void *param)
{
  for (;;)
  {
    //Wait until this task is triggered https://www.freertos.org/ulTaskNotifyTake.html
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    //Wait 100ms
    vTaskDelay(100 / portTICK_PERIOD_MS);
    //LED OFF
    QueueLED(RGBLED::OFF);
  }
}

// Handle all calls to i2c devices in this single task
// Provides thread safe mechanism to talk to i2c
void i2c_task(void *param)
{
  for (;;)
  {
    i2cQueueMessage m;

    if (xQueueReceive(queue_i2c, &m, portMAX_DELAY) == pdPASS)
    {
      // do some i2c task
      if (m.command == 0x01)
      {
        // Read ports A/B/C/D inputs (on TCA6408)
        uint8_t v = hal.ReadTCA6408InputRegisters();
        //P0=A
        InputState[0] = (v & B00000001) == 0 ? enumInputState::INPUT_LOW : enumInputState::INPUT_HIGH;
        //P1=B
        InputState[1] = (v & B00000010) == 0 ? enumInputState::INPUT_LOW : enumInputState::INPUT_HIGH;
        //P2=C
        InputState[2] = (v & B00000100) == 0 ? enumInputState::INPUT_LOW : enumInputState::INPUT_HIGH;
        //P3=D
        InputState[3] = (v & B00001000) == 0 ? enumInputState::INPUT_LOW : enumInputState::INPUT_HIGH;
        //P7=E
        InputState[4] = (v & B10000000) == 0 ? enumInputState::INPUT_LOW : enumInputState::INPUT_HIGH;
      }

      if (m.command == 0x02)
      {
        //Read ports
        //The 9534 deals with internal LED outputs and spare IO on J10
        uint8_t v = hal.ReadTCA9534InputRegisters();
        //P6 = spare I/O (on PCB pin)
        InputState[5] = (v & B01000000) == 0 ? enumInputState::INPUT_LOW : enumInputState::INPUT_HIGH;
        //P7 = Emergency Stop
        InputState[6] = (v & B10000000) == 0 ? enumInputState::INPUT_LOW : enumInputState::INPUT_HIGH;

        //Emergency Stop (J1) has triggered
        if (InputState[6] == enumInputState::INPUT_LOW)
        {
          emergencyStop = true;
        }
      }

      if (m.command == 0x03)
      {
        hal.Led(m.data);
      }

      if (m.command == 0x04)
      {
        hal.TFTScreenBacklight(m.data);
      }

      if (m.command >= 0xE0 && m.command <= 0xE0 + RELAY_TOTAL)
      {
        //Set state of relays
        hal.SetOutputState(m.command - 0xe0, (RelayState)m.data);
      }
    }
  }
}

volatile uint32_t WifiPasswordClearTime;
volatile bool ResetWifi = false;

// Check if BOOT button is pressed, if held down for more than 4 seconds
// trigger a wifi password reset/clear from EEPROM.
void IRAM_ATTR WifiPasswordClear()
{
  if (digitalRead(GPIO_NUM_0) == LOW)
  {
    //Button pressed, store time
    WifiPasswordClearTime = millis() + 4000;
    ResetWifi = false;
  }
  else
  {
    //Button released
    //Did user press button for longer than 4 seconds?
    if (millis() > WifiPasswordClearTime)
    {
      ResetWifi = true;
    }
  }
}

void IRAM_ATTR TCA6408Interrupt()
{
  if (queue_i2c==NULL) return;
  i2cQueueMessage m;
  m.command = 0x01;
  m.data = 0;
  xQueueSendToBackFromISR(queue_i2c, &m, NULL);
}

void IRAM_ATTR TCA9534AInterrupt()
{
  if (queue_i2c==NULL) return;
  i2cQueueMessage m;
  m.command = 0x02;
  m.data = 0;
  xQueueSendToBackFromISR(queue_i2c, &m, NULL);
}
#endif

#if defined(ESP8266)
void IRAM_ATTR ExternalInputInterrupt()
{
  if ((hal.ReadInputRegisters() & B00010000) == 0)
  {
    //Emergency Stop (J1) has triggered
    emergencyStop = true;
  }
}
#endif

void dumpByte(uint8_t data)
{
  if (data <= 0x0F)
  {
    SERIAL_DEBUG.print('0');
  }
  SERIAL_DEBUG.print(data, HEX);
}

void dumpPacketToDebug(char indicator, PacketStruct *buffer)
{
  //Filter on some commands
  //if ((buffer->command & 0x0F) != COMMAND::Timing)    return;

  SERIAL_DEBUG.print(millis());
  SERIAL_DEBUG.print(':');

  SERIAL_DEBUG.print(indicator);

  SERIAL_DEBUG.print(':');
  dumpByte(buffer->start_address);
  SERIAL_DEBUG.print('-');
  dumpByte(buffer->end_address);
  SERIAL_DEBUG.print('/');
  dumpByte(buffer->hops);
  SERIAL_DEBUG.print('/');
  dumpByte(buffer->command);

  switch (buffer->command & 0x0F)
  {

  case COMMAND::ResetBadPacketCounter:
    SERIAL_DEBUG.print(F(" ResetC   "));
    break;
  case COMMAND::ReadVoltageAndStatus:
    SERIAL_DEBUG.print(F(" RdVolt   "));
    break;
  case COMMAND::Identify:
    SERIAL_DEBUG.print(F(" Ident    "));
    break;
  case COMMAND::ReadTemperature:
    SERIAL_DEBUG.print(F(" RdTemp   "));
    break;
  case COMMAND::ReadBadPacketCounter:
    SERIAL_DEBUG.print(F(" RdBadPkC "));
    break;
  case COMMAND::ReadSettings:
    SERIAL_DEBUG.print(F(" RdSettin "));
    break;
  case COMMAND::WriteSettings:
    SERIAL_DEBUG.print(F(" WriteSet "));
    break;
  case COMMAND::ReadBalancePowerPWM:
    SERIAL_DEBUG.print(F(" RdBalanc "));
    break;
  case COMMAND::Timing:
    SERIAL_DEBUG.print(F(" Timing   "));
    break;
  case COMMAND::ReadBalanceCurrentCounter:
    SERIAL_DEBUG.print(F(" Current  "));
    break;
  case COMMAND::ReadPacketReceivedCounter:
    SERIAL_DEBUG.print(F(" PktRvd   "));
    break;
  default:
    SERIAL_DEBUG.print(F(" ??????   "));
    break;
  }

  SERIAL_DEBUG.printf("%.4X", buffer->sequence);
  //SERIAL_DEBUG.print(buffer->sequence, HEX);
  SERIAL_DEBUG.print('=');
  for (size_t i = 0; i < maximum_cell_modules_per_packet; i++)
  {
    //SERIAL_DEBUG.print(buffer->moduledata[i], HEX);
    SERIAL_DEBUG.printf("%.4X", buffer->moduledata[i]);
    SERIAL_DEBUG.print(" ");
  }
  SERIAL_DEBUG.print("=");
  //SERIAL_DEBUG.print(buffer->crc, HEX);
  SERIAL_DEBUG.printf("%.4X", buffer->crc);

  SERIAL_DEBUG.println();
}

String ControllerStateString(ControllerState value)
{
  switch (value)
  {
  case ControllerState::PowerUp:
    return String(F("PowerUp"));
  case ControllerState::ConfigurationSoftAP:
    return String(F("ConfigurationSoftAP"));
  case ControllerState::Stabilizing:
    return String(F("Stabilizing"));
  case ControllerState::Running:
    return String(F("Running"));
  case ControllerState::Unknown:
    return String(F("Unknown"));
  }

  return String("?");
}

void SetControllerState(ControllerState newState)
{
  if (ControlState != newState)
  {
    SERIAL_DEBUG.println();
    SERIAL_DEBUG.print(F("** Controller changed state from "));
    SERIAL_DEBUG.print(ControllerStateString(ControlState));
    SERIAL_DEBUG.print(F(" to "));
    SERIAL_DEBUG.println(ControllerStateString(newState));

    ControlState = newState;

#if defined(ESP32)
    switch (ControlState)
    {
    case ControllerState::PowerUp:
      //Purple during start up, don't use the QueueLED as thats not setup at this state
      hal.Led(RGBLED::Purple);
      break;
    case ControllerState::ConfigurationSoftAP:
      //Don't use the QueueLED as thats not setup at this state
      hal.Led(RGBLED::White);
      break;
    case ControllerState::Stabilizing:
      QueueLED(RGBLED::Yellow);
      break;
    case ControllerState::Running:
      QueueLED(RGBLED::Green);
      //Fire task to switch off BOOT button after 30 seconds
      xTaskNotify(wifiresetdisable_task_handle, 0x00, eNotifyAction::eNoAction);
      break;
    case ControllerState::Unknown:
      //Do nothing
      break;
    }
#endif
  }
}

uint16_t minutesSinceMidnight()
{

#if defined(ESP8266)
  return (hour() * 60) + minute();
#endif

#if defined(ESP32)
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
  {
    return 0;
  }
  else
  {
    return (timeinfo.tm_hour * 60) + timeinfo.tm_min;
  }
#endif
}

#if defined(ESP8266)
void processSyncEvent(NTPSyncEvent_t ntpEvent)
{
  if (ntpEvent < 0)
  {
    SERIAL_DEBUG.printf("Time Sync error: %d\n", ntpEvent);
    if (ntpEvent == noResponse)
      SERIAL_DEBUG.println(F("NTP server not reachable"));
    else if (ntpEvent == invalidAddress)
      SERIAL_DEBUG.println(F("Invalid NTP server address"));
    else if (ntpEvent == errorSending)
      SERIAL_DEBUG.println(F("Error sending request"));
    else if (ntpEvent == responseError)
      SERIAL_DEBUG.println(F("NTP response error"));
  }
  else
  {
    if (ntpEvent == timeSyncd)
    {
      SERIAL_DEBUG.print(F("Got NTP time"));
      time_t lastTime = NTP.getLastNTPSync();
      SERIAL_DEBUG.println(NTP.getTimeDateString(lastTime));
      setTime(lastTime);
    }
  }
}
#endif

void serviceReplyQueue()
{
  //if (replyQueue.isEmpty()) return;

  while (!replyQueue.isEmpty())
  {
    PacketStruct ps;
    replyQueue.pop(&ps);

#if defined(PACKET_LOGGING_RECEIVE)
    // Process decoded incoming packet
    dumpPacketToDebug('R', &ps);
#else
    //SERIAL_DEBUG.print('R');
#endif

    if (receiveProc.ProcessReply(&ps))
    {
      //Success, do nothing
    }
    else
    {
#if defined(ESP32)
      //Error blue
      QueueLED(RGBLED::Blue);
#endif
      SERIAL_DEBUG.print(F("*FAIL*"));
      dumpPacketToDebug('F', &ps);
    }
  }
}

void onPacketReceived()
{
#if defined(ESP8266)
  hal.GreenLedOn();
#endif

  PacketStruct ps;
  memcpy(&ps, SerialPacketReceiveBuffer, sizeof(PacketStruct));

  if ((ps.command & 0x0F) == COMMAND::Timing)
  {
    //Timestamp at the earliest possible moment
    uint32_t t = millis();
    ps.moduledata[2] = (t & 0xFFFF0000) >> 16;
    ps.moduledata[3] = t & 0x0000FFFF;
    //Ensure CRC is correct
    ps.crc = CRC16::CalculateArray((uint8_t *)&ps, sizeof(PacketStruct) - 2);
  }

  if (!replyQueue.push(&ps))
  {
    SERIAL_DEBUG.println(F("*Failed to queue reply*"));
  }

  //#if defined(PACKET_LOGGING_RECEIVE)
  // Process decoded incoming packet
  //dumpPacketToDebug('Q', &ps);
  //#endif

#if defined(ESP8266)
  hal.GreenLedOff();
#endif
}

void timerTransmitCallback()
{
  if (requestQueue.isEmpty())
    return;

  // Called to transmit the next packet in the queue need to ensure this procedure is called more frequently than
  // items are added into the queue

  PacketStruct transmitBuffer;

  requestQueue.pop(&transmitBuffer);
  sequence++;
  transmitBuffer.sequence = sequence;

  if (transmitBuffer.command == COMMAND::Timing)
  {
    //Timestamp at the last possible moment
    uint32_t t = millis();
    transmitBuffer.moduledata[0] = (t & 0xFFFF0000) >> 16;
    transmitBuffer.moduledata[1] = t & 0x0000FFFF;
  }

  transmitBuffer.crc = CRC16::CalculateArray((uint8_t *)&transmitBuffer, sizeof(PacketStruct) - 2);
  myPacketSerial.sendBuffer((byte *)&transmitBuffer);

  // Output the packet we just transmitted to debug console
#if defined(PACKET_LOGGING_SEND)
  dumpPacketToDebug('S', &transmitBuffer);
#else
  //SERIAL_DEBUG.print('S');
#endif
}

//Runs the rules and populates rule_outcome array with true/false for each rule
//Rules based on module parameters/readings like voltage and temperature
//are only processed once every module has returned at least 1 reading/communication
void ProcessRules()
{
  rules.ClearValues();
  rules.ClearWarnings();
  rules.ClearErrors();

  rules.rule_outcome[Rule::BMSError] = false;

  uint16_t totalConfiguredModules = TotalNumberOfCells();
  if (totalConfiguredModules > maximum_controller_cell_modules)
  {
    //System is configured with more than maximum modules - abort!
    rules.SetError(InternalErrorCode::TooManyModules);
  }

  if (receiveProc.totalModulesFound > 0 && receiveProc.totalModulesFound != totalConfiguredModules)
  {
    //Found more or less modules than configured for
    rules.SetError(InternalErrorCode::ModuleCountMismatch);
  }

  //Communications error...
  if (receiveProc.HasCommsTimedOut())
  {
    rules.SetError(InternalErrorCode::CommunicationsError);
  }

  uint8_t cellid = 0;
  for (int8_t bank = 0; bank < mysettings.totalNumberOfBanks; bank++)
  {
    for (int8_t i = 0; i < mysettings.totalNumberOfSeriesModules; i++)
    {
      rules.ProcessCell(bank, &cmi[cellid]);

      if (cmi[cellid].valid && cmi[cellid].settingsCached)
      {
        if (cmi[cellid].BypassThresholdmV != mysettings.BypassThresholdmV)
        {
          rules.SetWarning(InternalWarningCode::ModuleInconsistantBypassVoltage);
        }

        if (cmi[cellid].BypassOverTempShutdown != mysettings.BypassOverTempShutdown)
        {
          rules.SetWarning(InternalWarningCode::ModuleInconsistantBypassTemperature);
        }

        if (cmi[cellid].CodeVersionNumber != cmi[0].CodeVersionNumber)
        {
          //Do all the modules have the same version of code as module zero?
          rules.SetWarning(InternalWarningCode::ModuleInconsistantCodeVersion);
        }

        if (cmi[cellid].BoardVersionNumber != cmi[0].BoardVersionNumber)
        {
          //Do all the modules have the same hardware revision?
          rules.SetWarning(InternalWarningCode::ModuleInconsistantBoardRevision);
        }
      }

      cellid++;
    }
    rules.ProcessBank(bank);
  }

  if (rules.invalidModuleCount > 0)
  {
    //Some modules are not yet valid
    rules.SetError(InternalErrorCode::WaitingForModulesToReply);
  }

  if (ControlState == ControllerState::Running && rules.zeroVoltageModuleCount > 0)
  {
    rules.SetError(InternalErrorCode::ZeroVoltModule);
  }

  rules.RunRules(
      mysettings.rulevalue,
      mysettings.rulehysteresis,
      emergencyStop,
      minutesSinceMidnight());

  if (ControlState == ControllerState::Stabilizing)
  {
    //Check for zero volt modules - not a problem whilst we are in stabilizing start up mode
    if (rules.zeroVoltageModuleCount == 0 && rules.invalidModuleCount == 0)
    {
      //Every module has been read and they all returned a voltage move to running state
      SetControllerState(ControllerState::Running);
    }
  }

#if defined(ESP32)
  if (emergencyStop)
  {
    //Lowest 3 bits are RGB led GREEN/RED/BLUE
    QueueLED(RGBLED::Red);
  }
#endif
}

void timerSwitchPulsedRelay()
{
  //Set defaults based on configuration
  for (int8_t y = 0; y < RELAY_TOTAL; y++)
  {
    if (previousRelayPulse[y])
    {
//We now need to rapidly turn off the relay after a fixed period of time (pulse mode)
//However we leave the relay and previousRelayState looking like the relay has triggered (it has!)
//to prevent multiple pulses being sent on each rule refresh
#if defined(ESP8266)
      hal.SetOutputState(y, previousRelayState[y] == RelayState::RELAY_ON ? RelayState::RELAY_OFF : RelayState::RELAY_ON);
#endif

#if defined(ESP32)
      i2cQueueMessage m;
      //Different command for each relay
      m.command = 0xE0 + y;
      m.data = previousRelayState[y] == RelayState::RELAY_ON ? RelayState::RELAY_OFF : RelayState::RELAY_ON;
      xQueueSendToBack(queue_i2c, &m, 10 / portTICK_PERIOD_MS);
#endif

      previousRelayPulse[y] = false;
    }
  }

  //This only fires once
  myTimerSwitchPulsedRelay.detach();
}

void timerProcessRules()
{

  //Run the rules
  ProcessRules();

#if defined(RULES_LOGGING)
  SERIAL_DEBUG.print(F("Rules:"));
  for (int8_t r = 0; r < RELAY_RULES; r++)
  {
    SERIAL_DEBUG.print(rules.rule_outcome[r]);
  }
  SERIAL_DEBUG.print("=");
#endif

  RelayState relay[RELAY_TOTAL];

  //Set defaults based on configuration
  for (int8_t y = 0; y < RELAY_TOTAL; y++)
  {
    relay[y] = mysettings.rulerelaydefault[y] == RELAY_ON ? RELAY_ON : RELAY_OFF;
  }

  //Test the rules (in reverse order)
  for (int8_t n = RELAY_RULES - 1; n >= 0; n--)
  {
    if (rules.rule_outcome[n] == true)
    {

      for (int8_t y = 0; y < RELAY_TOTAL; y++)
      {
        //Dont change relay if its set to ignore/X
        if (mysettings.rulerelaystate[n][y] != RELAY_X)
        {
          if (mysettings.rulerelaystate[n][y] == RELAY_ON)
          {
            relay[y] = RELAY_ON;
          }
          else
          {
            relay[y] = RELAY_OFF;
          }
        }
      }
    }
  }

  for (int8_t n = 0; n < RELAY_TOTAL; n++)
  {
    if (previousRelayState[n] != relay[n])
    {
      //Would be better here to use the WRITE8 to lower i2c traffic
#if defined(RULES_LOGGING)
      SERIAL_DEBUG.print(F("Relay:"));
      SERIAL_DEBUG.print(n);
      SERIAL_DEBUG.print("=");
      SERIAL_DEBUG.print(relay[n]);
#endif
      //hal.SetOutputState(n, relay[n]);

      //This would be better if we worked out the bit pattern first and then just
      //submitted that as a single i2c read/write transaction

#if defined(ESP8266)
      hal.SetOutputState(n, relay[n]);
#endif

#if defined(ESP32)
      i2cQueueMessage m;
      //Different command for each relay
      m.command = 0xE0 + n;
      m.data = relay[n];
      xQueueSendToBack(queue_i2c, &m, 10 / portTICK_PERIOD_MS);
#endif

      previousRelayState[n] = relay[n];

      if (mysettings.relaytype[n] == RELAY_PULSE)
      {
        //If its a pulsed relay, invert the output quickly via a single shot timer
        previousRelayPulse[n] = true;
        myTimerSwitchPulsedRelay.attach(0.1, timerSwitchPulsedRelay);
#if defined(RULES_LOGGING)
        SERIAL_DEBUG.print("P");
#endif
      }
    }
  }
#if defined(RULES_LOGGING)
  SERIAL_DEBUG.println("");
#endif
}

void timerEnqueueCallback()
{
  QueueLED(RGBLED::Green);
  //Fire task to switch off LED in a few ms
  xTaskNotify(ledoff_task_handle, 0x00, eNotifyAction::eNoAction);

  //this is called regularly on a timer, it determines what request to make to the modules (via the request queue)
  uint16_t i = 0;
  uint16_t max = TotalNumberOfCells();

  uint8_t startmodule = 0;

  while (i < max)
  {
    uint16_t endmodule = (startmodule + maximum_cell_modules_per_packet) - 1;

    //Limit to number of modules we have configured
    if (endmodule > max)
    {
      endmodule = max - 1;
    }

    //Need to watch overflow of the uint8 here...
    prg.sendCellVoltageRequest(startmodule, endmodule);
    prg.sendCellTemperatureRequest(startmodule, endmodule);

    //If any module is in bypass then request PWM reading for whole bank
    for (uint8_t m = startmodule; m <= endmodule; m++)
    {
      if (cmi[m].inBypass)
      {
        prg.sendReadBalancePowerRequest(startmodule, endmodule);
        //We only need 1 reading for whole bank
        break;
      }
    }

    //Move to the next bank
    startmodule = endmodule + 1;
    i += maximum_cell_modules_per_packet;
  }
}

void connectToWifi()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    //SERIAL_DEBUG.println(F("Configuring Wi-Fi STA..."));
    WiFi.mode(WIFI_STA);

    char hostname[40];

#if defined(ESP8266)
    sprintf(hostname, "DIYBMS-%08X", ESP.getChipId());
    wifi_station_set_hostname(hostname);
    WiFi.hostname(hostname);
#endif
#if defined(ESP32)
    uint32_t chipId = 0;
    for (int i = 0; i < 17; i = i + 8)
    {
      chipId |= ((ESP.getEfuseMac() >> (40 - i)) & 0xff) << i;
    }
    sprintf(hostname, "DIYBMS-%08X", chipId);
    WiFi.setHostname(hostname);
#endif
    SERIAL_DEBUG.print(F("Hostname: "));
    SERIAL_DEBUG.print(hostname);
    SERIAL_DEBUG.println(F("  Connecting to Wi-Fi..."));
    WiFi.begin(DIYBMSSoftAP::WifiSSID(), DIYBMSSoftAP::WifiPassword());
  }

  WifiDisconnected = false;
}

void connectToMqtt()
{
  SERIAL_DEBUG.println(F("Connecting to MQTT..."));
  mqttClient.connect();
}

static AsyncClient *aClient = NULL;

void setupInfluxClient()
{

  if (aClient) //client already exists
    return;

  aClient = new AsyncClient();
  if (!aClient) //could not allocate client
    return;

  aClient->onError([](void *arg, AsyncClient *client, err_t error) {
    SERIAL_DEBUG.println(F("Connect Error"));
    aClient = NULL;
    delete client;
  },
                   NULL);

  aClient->onConnect([](void *arg, AsyncClient *client) {
    SERIAL_DEBUG.println(F("Connected"));

    //Send the packet here

    aClient->onError(NULL, NULL);

    client->onDisconnect([](void *arg, AsyncClient *c) {
      SERIAL_DEBUG.println(F("Disconnected"));
      aClient = NULL;
      delete c;
    },
                         NULL);

    client->onData([](void *arg, AsyncClient *c, void *data, size_t len) {
      //Data received
      SERIAL_DEBUG.print(F("\r\nData: "));
      SERIAL_DEBUG.println(len);
      //uint8_t* d = (uint8_t*)data;
      //for (size_t i = 0; i < len; i++) {SERIAL_DEBUG.write(d[i]);}
    },
                   NULL);

    //send the request

    //Construct URL for the influxdb
    //See API at https://docs.influxdata.com/influxdb/v1.7/tools/api/#write-http-endpoint

    String poststring;

    for (uint8_t bank = 0; bank < mysettings.totalNumberOfBanks; bank++)
    {
      //TODO: We should send a request per bank not just a single POST as we are likely to exceed capabilities of ESP
      for (uint8_t i = 0; i < mysettings.totalNumberOfSeriesModules; i++)
      {
        //Data in LINE PROTOCOL format https://docs.influxdata.com/influxdb/v1.7/write_protocols/line_protocol_tutorial/
        poststring = poststring + "cells," + "cell=" + String(bank) + "_" + String(i) + " v=" + String((float)cmi[i].voltagemV / 1000.0, 3) + ",i=" + String(cmi[i].internalTemp) + "i" + ",e=" + String(cmi[i].externalTemp) + "i" + ",b=" + (cmi[i].inBypass ? String("true") : String("false")) + "\n";
      }
    }

    //TODO: Need to URLEncode these values
    String url = "/write?db=" + String(mysettings.influxdb_database) + "&u=" + String(mysettings.influxdb_user) + "&p=" + String(mysettings.influxdb_password);
    String header = "POST " + url + " HTTP/1.1\r\n" + "Host: " + String(mysettings.influxdb_host) + "\r\n" + "Connection: close\r\n" + "Content-Length: " + poststring.length() + "\r\n" + "Content-Type: text/plain\r\n" + "\r\n";

    //SERIAL_DEBUG.println(header.c_str());
    //SERIAL_DEBUG.println(poststring.c_str());

    client->write(header.c_str());
    client->write(poststring.c_str());
  },
                     NULL);
}

void SendInfluxdbPacket()
{
  if (!mysettings.influxdb_enabled)
    return;

  SERIAL_DEBUG.println(F("SendInfluxdbPacket"));

  setupInfluxClient();

  if (!aClient->connect(mysettings.influxdb_host, mysettings.influxdb_httpPort))
  {
    SERIAL_DEBUG.println(F("Influxdb connect fail"));
    AsyncClient *client = aClient;
    aClient = NULL;
    delete client;
  }
}

void startTimerToInfluxdb()
{
  myTimerSendInfluxdbPacket.attach(30, SendInfluxdbPacket);
}

void SetupOTA()
{

  ArduinoOTA.setPort(3232);

  ArduinoOTA.setPassword("1jiOOx12AQgEco4e");

  ArduinoOTA
      .onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
          type = "sketch";
        else // U_SPIFFS
          type = "filesystem";

        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        SERIAL_DEBUG.println("Start updating " + type);
      });
  ArduinoOTA.onEnd([]() {
    SERIAL_DEBUG.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    SERIAL_DEBUG.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    SERIAL_DEBUG.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)
      SERIAL_DEBUG.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR)
      SERIAL_DEBUG.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR)
      SERIAL_DEBUG.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR)
      SERIAL_DEBUG.println("Receive Failed");
    else if (error == OTA_END_ERROR)
      SERIAL_DEBUG.println("End Failed");
  });

  ArduinoOTA.begin();
}

#if defined(ESP8266)
void onWifiConnect(const WiFiEventStationModeGotIP &event)
{
#else
void onWifiConnect(WiFiEvent_t event, WiFiEventInfo_t info)
{
#endif

  SERIAL_DEBUG.print(F("Wi-Fi status="));
  SERIAL_DEBUG.print(WiFi.status());
  SERIAL_DEBUG.print(F(". Connected IP:"));
  SERIAL_DEBUG.println(WiFi.localIP());

  SERIAL_DEBUG.print(F("Request NTP from "));
  SERIAL_DEBUG.println(mysettings.ntpServer);

#if defined(ESP8266)
  //Update time every 10 minutes
  NTP.setInterval(600);
  NTP.setNTPTimeout(NTP_TIMEOUT);
  // String ntpServerName, int8_t timeZone, bool daylight, int8_t minutes, AsyncUDP* udp_conn
  NTP.begin(mysettings.ntpServer, mysettings.timeZone, mysettings.daylight, mysettings.minutesTimeZone);
#endif

#if defined(ESP32)
  //Use native ESP32 code
  configTime(mysettings.minutesTimeZone * 60, mysettings.daylight * 60, mysettings.ntpServer);
#endif

  /*
  TODO: CHECK ERROR CODES BETTER!
  0 : WL_IDLE_STATUS when Wi-Fi is in process of changing between statuses
  1 : WL_NO_SSID_AVAIL in case configured SSID cannot be reached
  3 : WL_CONNECTED after successful connection is established
  4 : WL_CONNECT_FAILED if password is incorrect
  6 : WL_DISCONNECTED if module is not configured in station mode
  */
  if (!server_running)
  {
    DIYBMSServer::StartServer(&server);
    server_running = true;
  }

  if (mysettings.mqtt_enabled)
  {
    connectToMqtt();
  }

  if (mysettings.influxdb_enabled)
  {
    startTimerToInfluxdb();
  }

  SetupOTA();
}

#if defined(ESP8266)
void onWifiDisconnect(const WiFiEventStationModeDisconnected &event)
{
#else
void onWifiDisconnect(WiFiEvent_t event, WiFiEventInfo_t info)
{
#endif
  SERIAL_DEBUG.println(F("Disconnected from Wi-Fi."));

  //Indicate to loop() to reconnect, seems to be
  //ESP issues using Wifi from timers - https://github.com/espressif/arduino-esp32/issues/2686
  WifiDisconnected = true;

  // ensure we don't reconnect to MQTT while reconnecting to Wi-Fi
  mqttReconnectTimer.detach();
  myTimerSendMqttPacket.detach();
  myTimerSendMqttStatus.detach();
  myTimerSendInfluxdbPacket.detach();

  //wifiReconnectTimer.once(2, connectToWifi);
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
{
  SERIAL_DEBUG.println(F("Disconnected from MQTT."));

  myTimerSendMqttPacket.detach();
  myTimerSendMqttStatus.detach();

  if (WiFi.isConnected())
  {
    mqttReconnectTimer.once(2, connectToMqtt);
  }
}

void sendMqttStatus()
{
  if (!mysettings.mqtt_enabled && !mqttClient.connected())
    return;

  char topic[80];
  char jsonbuffer[220];
  DynamicJsonDocument doc(220);
  JsonObject root = doc.to<JsonObject>();

  root["banks"] = mysettings.totalNumberOfBanks;
  root["cells"] = mysettings.totalNumberOfSeriesModules;
  root["uptime"] = millis() / 1000; // I want to know the uptime of the device.

  JsonArray bankVoltage = root.createNestedArray("bankVoltage");
  for (int8_t bank = 0; bank < mysettings.totalNumberOfBanks; bank++)
  {
    bankVoltage.add((float)rules.packvoltage[bank] / (float)1000.0);
  }

  JsonObject monitor = root.createNestedObject("monitor");

  // Set error flag if we have attempted to send 2*number of banks without a reply
  monitor["commserr"] = receiveProc.HasCommsTimedOut() ? 1 : 0;
  monitor["sent"] = prg.packetsGenerated;
  monitor["received"] = receiveProc.packetsReceived;
  monitor["badcrc"] = receiveProc.totalCRCErrors;
  monitor["ignored"] = receiveProc.totalNotProcessedErrors;
  monitor["oos"] = receiveProc.totalOutofSequenceErrors;
  monitor["roundtrip"] = receiveProc.packetTimerMillisecond;

  serializeJson(doc, jsonbuffer, sizeof(jsonbuffer));
  sprintf(topic, "%s/status", mysettings.mqtt_topic);
  mqttClient.publish(topic, 0, false, jsonbuffer);
#if defined(MQTT_LOGGING)
  SERIAL_DEBUG.print("MQTT - ");
  SERIAL_DEBUG.print(topic);
  SERIAL_DEBUG.print('=');
  SERIAL_DEBUG.println(jsonbuffer);
#endif

  //Using Json for below reduced MQTT messages from 14 to 2. Could be combined into same json object too. But even better is status + event driven.
  doc.clear(); // Need to clear the json object for next message
  sprintf(topic, "%s/rule", mysettings.mqtt_topic);
  for (uint8_t i = 0; i < RELAY_RULES; i++)
  {
    doc[(String)i] = rules.rule_outcome[i] ? 1 : 0; // String conversion should be removed but just quick to get json format nice
  }
  serializeJson(doc, jsonbuffer, sizeof(jsonbuffer));
#if defined(MQTT_LOGGING)
  SERIAL_DEBUG.print("MQTT - ");
  SERIAL_DEBUG.print(topic);
  SERIAL_DEBUG.print('=');
  SERIAL_DEBUG.println(jsonbuffer);
#endif
  mqttClient.publish(topic, 0, false, jsonbuffer);

  doc.clear(); // Need to clear the json object for next message
  sprintf(topic, "%s/output", mysettings.mqtt_topic);
  for (uint8_t i = 0; i < RELAY_TOTAL; i++)
  {
    doc[(String)i] = (previousRelayState[i] == RelayState::RELAY_ON) ? 1 : 0;
  }

  serializeJson(doc, jsonbuffer, sizeof(jsonbuffer));
#if defined(MQTT_LOGGING)
  SERIAL_DEBUG.print("MQTT - ");
  SERIAL_DEBUG.print(topic);
  SERIAL_DEBUG.print('=');
  SERIAL_DEBUG.println(jsonbuffer);
#endif
  mqttClient.publish(topic, 0, false, jsonbuffer);
}

//Send a few MQTT packets and keep track so we send the next batch on following calls
uint8_t mqttStartModule = 0;

void sendMqttPacket()
{
#if defined(MQTT_LOGGING)
  SERIAL_DEBUG.println("sendMqttPacket");
#endif

  if (!mysettings.mqtt_enabled && !mqttClient.connected())
    return;

  char topic[80];
  char jsonbuffer[200];
  StaticJsonDocument<200> doc;

  //If the BMS is in error, stop sending MQTT packets for the data
  if (!rules.rule_outcome[Rule::BMSError])
  {
    uint8_t counter = 0;
    for (uint8_t i = mqttStartModule; i < TotalNumberOfCells(); i++)
    {
      //Only send valid module data
      if (cmi[i].valid)
      {
        uint8_t bank = i / mysettings.totalNumberOfSeriesModules;
        uint8_t module = i - (bank * mysettings.totalNumberOfSeriesModules);

        doc.clear();
        doc["voltage"] = (float)cmi[i].voltagemV / (float)1000.0;
        doc["vMax"] = (float)cmi[i].voltagemVMax / (float)1000.0;
        doc["vMin"] = (float)cmi[i].voltagemVMin / (float)1000.0;
        doc["inttemp"] = cmi[i].internalTemp;
        doc["exttemp"] = cmi[i].externalTemp;
        doc["bypass"] = cmi[i].inBypass ? 1 : 0;
        doc["PWM"] = (int)((float)cmi[i].PWMValue / (float)255.0 * 100);
        doc["bypassT"] = cmi[i].bypassOverTemp ? 1 : 0;
        doc["bpc"] = cmi[i].badPacketCount;
        doc["mAh"] = cmi[i].BalanceCurrentCount;
        serializeJson(doc, jsonbuffer, sizeof(jsonbuffer));

        sprintf(topic, "%s/%d/%d", mysettings.mqtt_topic, bank, module);

        mqttClient.publish(topic, 0, false, jsonbuffer);

#if defined(MQTT_LOGGING)
        SERIAL_DEBUG.print("MQTT - ");
        SERIAL_DEBUG.print(topic);
        SERIAL_DEBUG.print('=');
        SERIAL_DEBUG.println(jsonbuffer);
#endif
      }

      counter++;

      //After transmitting this many packets over MQTT, store our current state and exit the function.
      //this prevents flooding the ESP controllers wifi stack and potentially causing reboots/fatal exceptions
      if (counter == 6)
      {
        mqttStartModule = i + 1;

        if (mqttStartModule > TotalNumberOfCells())
        {
          mqttStartModule = 0;
        }

        return;
      }
    }

    //Completed the loop, start at zero
    mqttStartModule = 0;
  }
}

void onMqttConnect(bool sessionPresent)
{
  SERIAL_DEBUG.println(F("Connected to MQTT."));
  myTimerSendMqttPacket.attach(5, sendMqttPacket);
  myTimerSendMqttStatus.attach(25, sendMqttStatus);
}

void LoadConfiguration()
{
  if (Settings::ReadConfigFromEEPROM((char *)&mysettings, sizeof(mysettings), EEPROM_SETTINGS_START_ADDRESS))
    return;

  SERIAL_DEBUG.println(F("Apply default config"));

  //Zero all the bytes
  memset(&mysettings, 0, sizeof(mysettings));

  //Default to a single module
  mysettings.totalNumberOfBanks = 1;
  mysettings.totalNumberOfSeriesModules = 1;
  mysettings.BypassOverTempShutdown = 65;
  //4.10V bypass
  mysettings.BypassThresholdmV = 4100;
  mysettings.graph_voltagehigh = 4.5;
  mysettings.graph_voltagelow = 2.75;

  //EEPROM settings are invalid so default configuration
  mysettings.mqtt_enabled = false;
  mysettings.mqtt_port = 1883;

  //Default to EMONPI default MQTT settings
  strcpy(mysettings.mqtt_topic, "diybms");
  strcpy(mysettings.mqtt_server, "192.168.0.26");
  strcpy(mysettings.mqtt_username, "emonpi");
  strcpy(mysettings.mqtt_password, "emonpimqtt2016");

  mysettings.influxdb_enabled = false;
  strcpy(mysettings.influxdb_host, "myinfluxserver");
  strcpy(mysettings.influxdb_database, "database");
  strcpy(mysettings.influxdb_user, "user");
  strcpy(mysettings.influxdb_password, "");

  mysettings.timeZone = 0;
  mysettings.minutesTimeZone = 0;
  mysettings.daylight = false;
  strcpy(mysettings.ntpServer, "time.google.com");

  for (size_t x = 0; x < RELAY_TOTAL; x++)
  {
    mysettings.rulerelaydefault[x] = RELAY_OFF;
  }

  //1. Emergency stop
  mysettings.rulevalue[Rule::EmergencyStop] = 0;
  //2. Internal BMS error (communication issues, fault readings from modules etc)
  mysettings.rulevalue[Rule::BMSError] = 0;
  //3. Individual cell over voltage
  mysettings.rulevalue[Rule::Individualcellovervoltage] = 4150;
  //4. Individual cell under voltage
  mysettings.rulevalue[Rule::Individualcellundervoltage] = 3000;
  //5. Individual cell over temperature (external probe)
  mysettings.rulevalue[Rule::IndividualcellovertemperatureExternal] = 55;
  //6. Pack over voltage (mV)
  mysettings.rulevalue[Rule::IndividualcellundertemperatureExternal] = 5;
  //7. Pack under voltage (mV)
  mysettings.rulevalue[Rule::PackOverVoltage] = 4200 * 8;
  //8. RULE_PackUnderVoltage
  mysettings.rulevalue[Rule::PackUnderVoltage] = 3000 * 8;
  mysettings.rulevalue[Rule::Timer1] = 60 * 8;  //8am
  mysettings.rulevalue[Rule::Timer2] = 60 * 17; //5pm

  for (size_t i = 0; i < RELAY_RULES; i++)
  {
    mysettings.rulehysteresis[i] = mysettings.rulevalue[i];

    //Set all relays to don't care
    for (size_t x = 0; x < RELAY_TOTAL; x++)
    {
      mysettings.rulerelaystate[i][x] = RELAY_X;
    }
  }

  for (size_t x = 0; x < RELAY_TOTAL; x++)
  {
    mysettings.relaytype[x] = RELAY_STANDARD;
  }
}

uint8_t lazyTimerMode = 0;
//Do activities which are not critical to the system like background loading of config, or updating timing results etc.
void timerLazyCallback()
{
  if (requestQueue.getRemainingCount() < 6)
  {
    //Exit here to avoid overflowing the queue
    return;
  }

  lazyTimerMode++;

  if (lazyTimerMode == 1)
  {
    uint8_t counter = 0;
    //Find modules that don't have settings cached and request them
    for (uint8_t module = 0; module < (TotalNumberOfCells()); module++)
    {
      if (cmi[module].valid && !cmi[module].settingsCached)
      {
        if (requestQueue.getRemainingCount() < 6)
        {
          //Exit here to avoid flooding the queue
          return;
        }

        prg.sendGetSettingsRequest(module);
        counter++;
      }
    }

    return;
  }

  if (lazyTimerMode == 2)
  {
    //Send a "ping" message through the cells to get a round trip time
    prg.sendTimingRequest();
    return;
  }

  //Send these requests to all banks of modules
  uint16_t i = 0;
  uint16_t max = TotalNumberOfCells();

  uint8_t startmodule = 0;

  while (i < max)
  {
    uint16_t endmodule = (startmodule + maximum_cell_modules_per_packet) - 1;

    //Limit to number of modules we have configured
    if (endmodule > max)
    {
      endmodule = max - 1;
    }

    //Need to watch overflow of the uint8 here...
    prg.sendCellVoltageRequest(startmodule, endmodule);

    if (lazyTimerMode == 3)
    {
      prg.sendReadBalanceCurrentCountRequest(startmodule, endmodule);
      return;
    }

    if (lazyTimerMode == 4)
    {
      //Just for debug, only do the first 16 modules
      prg.sendReadPacketsReceivedRequest(startmodule, endmodule);
      return;
    }

    //Ask for bad packet count (saves battery power if we dont ask for this all the time)
    if (lazyTimerMode == 5)
    {
      prg.sendReadBadPacketCounter(startmodule, endmodule);
      return;
    }

    //Move to the next bank
    startmodule = endmodule + 1;
    i += maximum_cell_modules_per_packet;
  }

  lazyTimerMode = 0;
}

void resetAllRules()
{
  //Clear all rules
  for (int8_t r = 0; r < RELAY_RULES; r++)
  {
    rules.rule_outcome[r] = false;
  }
}

bool CaptureSerialInput(HardwareSerial stream, char *buffer, int buffersize, bool OnlyDigits, bool ShowPasswordChar)
{
  int length = 0;
  unsigned long timer = millis() + 30000;

  while (true)
  {

    //Abort after 30 seconds of inactivity
    if (millis() > timer)
      return false;

    //We should add a timeout in here, and return FALSE when we abort....
    while (stream.available())
    {
      //Reset timer on serial input
      timer = millis() + 30000;

      int data = stream.read();
      if (data == '\b' || data == '\177')
      { // BS and DEL
        if (length)
        {
          length--;
          stream.write("\b \b");
        }
      }
      else if (data == '\n')
      {
        //Ignore
      }
      else if (data == '\r')
      {
        if (length > 0)
        {
          stream.write("\r\n"); // output CRLF
          buffer[length] = '\0';

          //Soak up any other characters on the buffer and throw away
          while (stream.available())
          {
            stream.read();
          }

          //Return to caller
          return true;
        }

        length = 0;
      }
      else if (length < buffersize - 1)
      {
        if (OnlyDigits && (data < '0' || data > '9'))
        {
          //We need to filter out non-digit characters
        }
        else
        {
          buffer[length++] = data;
          if (ShowPasswordChar)
          {
            //Hide real character
            stream.write('*');
          }
          else
          {
            stream.write(data);
          }
        }
      }
    }
  }
}

void TerminalBasedWifiSetup()
{
  SERIAL_DEBUG.println(F("\r\n\r\nDIYBMS CONTROLLER - Scanning Wifi"));

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  int n = WiFi.scanNetworks();

  if (n == 0)
    SERIAL_DEBUG.println(F("no networks found"));
  else
  {
    for (int i = 0; i < n; ++i)
    {
      if (i < 10)
      {
        SERIAL_DEBUG.print(' ');
      }
      SERIAL_DEBUG.print(i);
      SERIAL_DEBUG.print(':');
      SERIAL_DEBUG.print(WiFi.SSID(i));

      //Pad out the wifi names into 2 columns
      for (size_t spaces = WiFi.SSID(i).length(); spaces < 36; spaces++)
      {
        SERIAL_DEBUG.print(' ');
      }

      if ((i + 1) % 2 == 0)
      {
        SERIAL_DEBUG.println();
      }
      delay(5);
    }
    SERIAL_DEBUG.println();
  }

  WiFi.mode(WIFI_OFF);

  SERIAL_DEBUG.print(F("Enter the NUMBER of the Wifi network to connect to:"));

  bool result;
  char buffer[10];
  result = CaptureSerialInput(SERIAL_DEBUG, buffer, 10, true, false);
  if (result)
  {
    int index = String(buffer).toInt();
    SERIAL_DEBUG.print(F("Enter the password to use when connecting to '"));
    SERIAL_DEBUG.print(WiFi.SSID(index));
    SERIAL_DEBUG.print("':");

    char passwordbuffer[80];
    result = CaptureSerialInput(SERIAL_DEBUG, passwordbuffer, 80, false, true);

    if (result)
    {
      wifi_eeprom_settings config;
      memset(&config, 0, sizeof(config));
      WiFi.SSID(index).toCharArray(config.wifi_ssid, sizeof(config.wifi_ssid));
      strcpy(config.wifi_passphrase, passwordbuffer);
      Settings::WriteConfigToEEPROM((char *)&config, sizeof(config), EEPROM_WIFI_START_ADDRESS);
    }
  }

  SERIAL_DEBUG.println(F("REBOOTING IN 5..."));
  delay(5000);
  ESP.restart();
}

void tft_display_off()
{
  tft.fillScreen(TFT_BLACK);
  //hal.TFTScreenBacklight(true);

  //Queue up i2c message
  i2cQueueMessage m;
  //4 = TFT backlight LED
  m.command = 0x04;
  //Lowest 3 bits are RGB led GREEN/RED/BLUE
  m.data = false;
  xQueueSendToBack(queue_i2c, &m, 10 / portTICK_PERIOD_MS);
}



void init_tft_display()
{
  tft.init();
  tft.initDMA(); // Initialise the DMA engine (tested with STM32F446 and STM32F767)
  tft.getSPIinstance().setHwCs(false);
  tft.setRotation(3);
  tft.fillScreen(SplashLogoPalette[0]);

  //SplashLogoGraphic_Height
  tft.pushImage((int32_t)TFT_HEIGHT / 2 - SplashLogoGraphic_Width / 2, (int32_t)4, (int32_t)152, (int32_t)48, SplashLogoGraphic, false, SplashLogoPalette);

  tft.setTextColor(TFT_WHITE, SplashLogoPalette[0]);

  tft.setCursor(0, 48 + 16, 4);
  tft.print(F("Ver:"));
  tft.println(GIT_VERSION_SHORT);
  tft.println(F("Build Date:"));
  tft.println(COMPILE_DATE_TIME_SHORT);

  hal.TFTScreenBacklight(true);
}

bool rst_active_high=false;

void avr_reset_target(bool reset) {
  digitalWrite(GPIO_NUM_0, ((reset && rst_active_high) || (!reset && !rst_active_high)) ? HIGH : LOW);
}


void setup()
{
  WiFi.mode(WIFI_OFF);
#if defined(ESP32)
  btStop();
  esp_log_level_set("*", ESP_LOG_WARN); // set all components to WARN level
  //esp_log_level_set("wifi", ESP_LOG_WARN);      // enable WARN logs from WiFi stack
  //esp_log_level_set("dhcpc", ESP_LOG_WARN);     // enable INFO logs from DHCP client
#endif

  //file_diybms_module_blinky_firmware_avrbin
  //size_file_diybms_module_blinky_firmware_avrbin

  //Debug serial output
#if defined(ESP8266)
  //ESP8266 uses dedicated 2nd serial port, but transmit only
  SERIAL_DEBUG.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  SERIAL_DEBUG.setDebugOutput(true);
#endif
#if defined(ESP32)
  //ESP32 we use the USB serial interface for console/debug messages
  SERIAL_DEBUG.begin(115200, SERIAL_8N1);
  SERIAL_DEBUG.setDebugOutput(true);
  SERIAL_DEBUG.print(F("DIYBMS CONTROLLER - version:"));
  SERIAL_DEBUG.print(GIT_VERSION);
  SERIAL_DEBUG.print(F(" compiled:"));
  SERIAL_DEBUG.println(COMPILE_DATE_TIME);
/*
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);

  SERIAL_DEBUG.print(F("ESP32 Chip model = "));
  SERIAL_DEBUG.print(chip_info.model);
  SERIAL_DEBUG.print(", Rev ");
  SERIAL_DEBUG.print(chip_info.revision);
  SERIAL_DEBUG.print(", Cores ");
  SERIAL_DEBUG.print(chip_info.cores);
  SERIAL_DEBUG.print(", Features=0x");
  SERIAL_DEBUG.println(chip_info.features, HEX);
*/
#endif

  //We generate a unique number which is used in all following JSON requests
  //we use this as a simple method to avoid cross site scripting attacks
  DIYBMSServer::generateUUID();

#if defined(ESP32)
  hal.ConfigurePins(WifiPasswordClear);
  hal.ConfigureI2C(TCA6408Interrupt, TCA9534AInterrupt);
  hal.ConfigureVSPI();
#endif

#if defined(ESP8266)
  hal.ConfigurePins();
  hal.ConfigureI2C(ExternalInputInterrupt);
#endif

  SetControllerState(ControllerState::PowerUp);

  hal.Led(0);

  SERIAL_DEBUG.println("Start ATMEL ISP programming...");

  avr_reset_target(true);
  //Disable BOOT button interrupt
  hal.SwapGPIO0ToOutput();
  hal.ConfigureVSPIForAVRISP();


  digitalWrite(VSPI_SCK, LOW);
  delay(20); // discharge PIN_SCK, value arbitrarily chosen
  avr_reset_target(false);
  // Pulse must be minimum 2 target CPU clock cycles so 100 usec is ok for CPU speeds above 20 KHz
  delayMicroseconds(100);
  avr_reset_target(true);

  // Send the enable programming command:
  delay(30); // datasheet: must be > 20 msec
  uint8_t reply = hal.VSPI_Transaction(0xAC, 0x53, 0x00, 0x00);

  SERIAL_DEBUG.println(reply);

  SERIAL_DEBUG.print("Device Signature=");
  uint8_t high = hal.VSPI_Transaction(0x30, 0x00, 0x00, 0x00);
  SERIAL_DEBUG.print(high,HEX);
  uint8_t middle = hal.VSPI_Transaction(0x30, 0x00, 0x01, 0x00);
  SERIAL_DEBUG.print(middle,HEX);
  uint8_t low = hal.VSPI_Transaction(0x30, 0x00, 0x02, 0x00);
  SERIAL_DEBUG.println(low,HEX);

  SERIAL_DEBUG.print("Delay 1");
  delay(1000);
  SERIAL_DEBUG.print("2");
  delay(1000);
  SERIAL_DEBUG.print("3");
  delay(1000);
  SERIAL_DEBUG.print("4");
  delay(1000);
  SERIAL_DEBUG.println("5");
  delay(1000);
  
  hal.VSPI_EndTransaction();
  //Exit programming mode
  hal.ConfigureVSPI();
  avr_reset_target(false);
  SERIAL_DEBUG.println("FINISH");
  SERIAL_DEBUG.flush();

  while (true)
  {
    delay(100);
  }


#if defined(ESP32)
  hal.ConfigureVSPI();
  init_tft_display();

  //Init the touch screen
  touchscreen.begin(hal.vspi);  
  touchscreen.setRotation(3);

  //All comms to i2c needs to go through this single task
  //to prevent issues with thread safety on the i2c hardware/libraries
  queue_i2c = xQueueCreate(10, sizeof(i2cQueueMessage));

  //Create i2c task on CPU 0 (normal code runs on CPU 1)
  xTaskCreatePinnedToCore(i2c_task, "i2c", 2048, nullptr, 2, &i2c_task_handle, 0);
  xTaskCreatePinnedToCore(ledoff_task, "ledoff", 1048, nullptr, 1, &ledoff_task_handle, 0);
  xTaskCreate(wifiresetdisable_task, "wifidbl", 1048, nullptr, 1, &wifiresetdisable_task_handle);
#endif

#if defined(ESP8266)
  //Pretend the button is not pressed
  uint8_t clearAPSettings = 0xFF;
  //Fix for issue 5, delay for 3 seconds on power up with green LED lit so
  //people get chance to jump WIFI reset pin (d3)
  hal.GreenLedOn();
  delay(3000);
  //This is normally pulled high, D3 is used to reset WIFI details
  clearAPSettings = digitalRead(RESET_WIFI_PIN);
  hal.GreenLedOff();
#endif

  //Pre configure the array
  memset(&cmi, 0, sizeof(cmi));
  for (size_t i = 0; i < maximum_controller_cell_modules; i++)
  {
    DIYBMSServer::clearModuleValues(i);
  }

  resetAllRules();

#if defined(ESP32)
  //Receive is IO2 which means the RX1 plug must be disconnected for programming to work!
  SERIAL_DATA.begin(COMMS_BAUD_RATE, SERIAL_8N1, 2, 32); // Serial for comms to modules
#endif

#if defined(ESP8266)
  SERIAL_DATA.begin(COMMS_BAUD_RATE, SERIAL_8N1); // Serial for comms to modules
  //Use alternative GPIO pins of D7/D8
  //D7 = GPIO13 = RECEIVE SERIAL
  //D8 = GPIO15 = TRANSMIT SERIAL
  SERIAL_DATA.swap();
#endif

  myPacketSerial.begin(&SERIAL_DATA, &onPacketReceived, sizeof(PacketStruct), SerialPacketReceiveBuffer, sizeof(SerialPacketReceiveBuffer));

#if defined(ESP8266)
  // initialize LittleFS
  if (!LittleFS.begin())
#endif
#if defined(ESP32)
    // initialize LittleFS
    if (!SPIFFS.begin())
#endif
    {
      SERIAL_DEBUG.println(F("An Error has occurred while mounting LittleFS"));
    }

  LoadConfiguration();

  //These need to be removed/replaced/fixed
  InputsEnabled = hal.InputsEnabled;
  OutputsEnabled = hal.OutputsEnabled;

  //Set relay defaults
  for (int8_t y = 0; y < RELAY_TOTAL; y++)
  {
    previousRelayState[y] = mysettings.rulerelaydefault[y];
    //Set relay defaults
    hal.SetOutputState(y, mysettings.rulerelaydefault[y]);
  }

#if defined(ESP32)
  //Allow user to press SPACE BAR key on serial terminal
  //to enter text based WIFI setup
  SERIAL_DEBUG.print(F("Press SPACE BAR to enter terminal based configuration...."));
  for (size_t i = 0; i < (3000 / 250); i++)
  {
    SERIAL_DEBUG.print('.');
    while (SERIAL_DEBUG.available())
    {
      int x = SERIAL_DEBUG.read();
      //SPACE BAR
      if (x == 32)
      {
        TerminalBasedWifiSetup();
      }
    }
    delay(250);
  }
  SERIAL_DEBUG.println(F("skipped"));
#endif

  //Temporarly force WIFI settings
  //wifi_eeprom_settings xxxx;
  //strcpy(xxxx.wifi_ssid,"XXXXXX");
  //strcpy(xxxx.wifi_passphrase,"XXXXXX");
  //Settings::WriteConfigToEEPROM((char*)&xxxx, sizeof(xxxx), EEPROM_WIFI_START_ADDRESS);
  //clearAPSettings = 0;

  if (!DIYBMSSoftAP::LoadConfigFromEEPROM()
#if defined(ESP8266)
      || clearAPSettings == 0
#endif
  )
  {
    //We have just started up and the EEPROM is empty of configuration
    SetControllerState(ControllerState::ConfigurationSoftAP);

    //SERIAL_DEBUG.print(F("Clear AP settings"));
    //SERIAL_DEBUG.println(clearAPSettings);
    SERIAL_DEBUG.println(F("Setup Access Point"));
    //We are in initial power on mode (factory reset)
    DIYBMSSoftAP::SetupAccessPoint(&server);
  }
  else
  {

#if defined(ESP8266)
    //Config NTP
    NTP.onNTPSyncEvent([](NTPSyncEvent_t event) {
      ntpEvent = event;
      NTPsyncEventTriggered = true;
    });
#endif

    SERIAL_DEBUG.println(F("Connecting to WIFI"));

    /* Explicitly set the ESP8266 to be a WiFi-client, otherwise by default,
      would try to act as both a client and an access-point */

#if defined(ESP8266)
    wifiConnectHandler = WiFi.onStationModeGotIP(onWifiConnect);
    wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWifiDisconnect);
#endif

#if defined(ESP32)
    WiFi.onEvent(onWifiConnect, WiFiEvent_t::SYSTEM_EVENT_STA_GOT_IP);
    WiFi.onEvent(onWifiDisconnect, WiFiEvent_t::SYSTEM_EVENT_STA_DISCONNECTED);
#endif

    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);

    if (mysettings.mqtt_enabled)
    {
      SERIAL_DEBUG.println("MQTT Enabled");
      mqttClient.setServer(mysettings.mqtt_server, mysettings.mqtt_port);
      mqttClient.setCredentials(mysettings.mqtt_username, mysettings.mqtt_password);
    }

    //Ensure we service the cell modules every 6 seconds
    myTimer.attach(6, timerEnqueueCallback);

    //Process rules every 5 seconds
    myTimerRelay.attach(5, timerProcessRules);

    //We process the transmit queue every 1 second (this needs to be lower delay than the queue fills)
    //and slower than it takes a single module to process a command (about 300ms)
    myTransmitTimer.attach(1, timerTransmitCallback);

    //Service reply queue
    myReplyTimer.attach(1, serviceReplyQueue);

    //This is a lazy timer for low priority tasks
    myLazyTimer.attach(8, timerLazyCallback);

    //We have just started...
    SetControllerState(ControllerState::Stabilizing);

    tft_display_off();
  }
}

void loop()
{
  //Allow CPU to sleep some
  //delay(10);
  //ESP_LOGW("LOOP","LOOP");

  if (touchscreen.tirqTouched())
  {
    if (touchscreen.touched())
    {
      /*
      TS_Point p = touchscreen.getPoint();
      SERIAL_DEBUG.print("Pressure = ");
      SERIAL_DEBUG.print(p.z);
      SERIAL_DEBUG.print(", x = ");
      SERIAL_DEBUG.print(p.x);
      SERIAL_DEBUG.print(", y = ");
      SERIAL_DEBUG.print(p.y);
      SERIAL_DEBUG.println();
      */
    }
  }

  if (WifiDisconnected && ControlState != ControllerState::ConfigurationSoftAP)
  {
    connectToWifi();
  }

  if (ResetWifi)
  {
    //Password reset, turn LED CYAN
    QueueLED(RGBLED::Cyan);

    //Wipe EEPROM WIFI setting
    DIYBMSSoftAP::FactoryReset();
  }

  ArduinoOTA.handle();

  // Call update to receive, decode and process incoming packets.
  myPacketSerial.checkInputStream();

  if (ConfigHasChanged > 0)
  {
    //Auto reboot if needed (after changing MQTT or INFLUX settings)
    //Ideally we wouldn't need to reboot if the code could sort itself out!
    ConfigHasChanged--;
    if (ConfigHasChanged == 0)
    {
      SERIAL_DEBUG.println(F("RESTART AFTER CONFIG CHANGE"));
      //Stop networking
      if (mqttClient.connected())
      {
        mqttClient.disconnect(true);
      }
      WiFi.disconnect();
      ESP.restart();
    }
  }

#if defined(ESP8266)
  if (NTPsyncEventTriggered)
  {
    processSyncEvent(ntpEvent);
    NTPsyncEventTriggered = false;
  }
#endif
}
