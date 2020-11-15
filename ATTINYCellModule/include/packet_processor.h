#ifndef DIYBMS_PACKETPROCESSOR_H // include guard
#define DIYBMS_PACKETPROCESSOR_H

#include <Arduino.h>

#include "diybms_attiny841.h"
#include "Steinhart.h"
#include "defines.h"
#include "settings.h"
#include "crc16.h"

#define ADC_CELL_VOLTAGE 0
#define ADC_INTERNAL_TEMP 1
#define ADC_EXTERNAL_TEMP 2

//Define maximum allowed temperature as safety cut off
#define DIYBMS_MODULE_SafetyTemperatureCutoff 90

#define maximum_cell_modules 16

//NOTE THIS MUST BE EVEN IN SIZE (BYTES) ESP8266 IS 32 BIT AND WILL ALIGN AS SUCH!
struct PacketStruct
{
  uint8_t start_address;
  uint8_t end_address;
  uint8_t command;
  uint8_t hops;
  uint16_t sequence;
  uint16_t moduledata[maximum_cell_modules];
  uint16_t crc;
} __attribute__((packed));


typedef union
{
  float number;
  uint8_t bytes[4];
  uint16_t word[2];
} FLOATUNION_t;

class PacketProcessor
{
public:
  PacketProcessor(DiyBMSATTiny841 *hardware, CellModuleConfig *config)
  {
    _hardware = hardware;
    _config = config;
    SettingsHaveChanged = false;
    WeAreInBypass = false;
    bypassCountDown = 0;
    bypassHasJustFinished = 0;
    pwmrunning = false;
  }
  ~PacketProcessor() {}

  bool onPacketReceived(PacketStruct *receivebuffer);

  void ADCReading(uint16_t value);
  void TakeAnAnalogueReading(uint8_t mode);
  uint16_t CellVoltage();
  uint16_t IncrementWatchdogCounter()
  {
    watchdog_counter++;
    return watchdog_counter;
  }
  bool BypassCheck();
  uint16_t TemperatureMeasurement();
  byte identifyModule;
  bool BypassOverheatCheck();

  //Raw value returned from ADC (10bit)
  uint16_t RawADCValue();
  int16_t InternalTemperature();

  //Returns TRUE if the module is bypassing current
  bool WeAreInBypass;

  //Value of PWM 0-100
  uint16_t PWMValue;
  volatile bool SettingsHaveChanged;

  uint16_t bypassCountDown;
  uint8_t bypassHasJustFinished;
  bool pwmrunning;

  bool IsBypassActive()
  {
    return WeAreInBypass || bypassHasJustFinished > 0 || pwmrunning;
  }

private:
  DiyBMSATTiny841 *_hardware;
  CellModuleConfig *_config;

  //PacketStruct buffer;

  bool processPacket(PacketStruct *buffer);

  volatile bool ModuleAddressAssignedFlag = false;
  volatile uint8_t adcmode = 0;
  volatile uint16_t raw_adc_voltage;
  volatile uint16_t onboard_temperature;
  volatile uint16_t external_temperature;
  volatile uint8_t mymoduleaddress = 0;
  volatile uint16_t badpackets = 0;
  volatile uint16_t watchdog_counter = 0;
};

#endif
