/*
____  ____  _  _  ____  __  __  ___
(  _ \(_  _)( \/ )(  _ \(  \/  )/ __)
)(_) )_)(_  \  /  ) _ < )    ( \__ \
(____/(____) (__) (____/(_/\/\_)(___/

DIYBMS V4.0
CELL MODULE FOR ATTINY841

(c)2019/2020 Stuart Pittaway

COMPILE THIS CODE USING PLATFORM.IO

LICENSE
Attribution-NonCommercial-ShareAlike 2.0 UK: England & Wales (CC BY-NC-SA 2.0 UK)
https://creativecommons.org/licenses/by-nc-sa/2.0/uk/

* Non-Commercial — You may not use the material for commercial purposes.
* Attribution — You must give appropriate credit, provide a link to the license, and indicate if changes were made.
  You may do so in any reasonable manner, but not in any way that suggests the licensor endorses you or your use.
* ShareAlike — If you remix, transform, or build upon the material, you must distribute your
  contributions under the same license as the original.
* No additional restrictions — You may not apply legal terms or technological measures
  that legally restrict others from doing anything the license permits.
*/

#include "packet_processor.h"

//Returns TRUE if the internal thermistor is hotter than the required setting (or over max limit)
bool PacketProcessor::BypassOverheatCheck()
{
  int16_t temp = InternalTemperature();
  return (temp > _config->BypassTemperatureSetPoint || temp > DIYBMS_MODULE_SafetyTemperatureCutoff);
}

// Returns an integer byte indicating the internal thermistor temperature in degrees C
// uses basic B Coefficient Steinhart calculaton to give rough approximation in temperature
int16_t PacketProcessor::InternalTemperature()
{
  return Steinhart::ThermistorToCelcius(INT_BCOEFFICIENT, onboard_temperature);
}

//Returns TRUE if the cell voltage is greater than the required setting
bool PacketProcessor::BypassCheck()
{
  return (CellVoltage() > _config->BypassThresholdmV);
}

//Records an ADC reading after the interrupt has finished
void PacketProcessor::ADCReading(uint16_t value)
{
  switch (adcmode)
  {
  case ADC_CELL_VOLTAGE:
  {
    //UpdateRingBuffer(value);
    raw_adc_voltage = value;
    break;
  }
  case ADC_INTERNAL_TEMP:
  {
#if (defined(DIYBMSMODULEVERSION) && (DIYBMSMODULEVERSION == 420 && defined(SWAPR19R20)))
    //R19 and R20 swapped on V4.2 board, invert the thermistor reading
    onboard_temperature = 1225 - value;
#elif (defined(DIYBMSMODULEVERSION) && (DIYBMSMODULEVERSION == 430 && defined(SWAPR19R20)))
    //R19 and R20 swapped on V4.3 board (never publically released), invert the thermistor reading
    onboard_temperature = 1000 - value;
#else
    onboard_temperature = value;
#endif
    break;
  }
  case ADC_EXTERNAL_TEMP:
  {
    external_temperature = value;
    break;
  }
  }
}

//Start an ADC reading via Interrupt
void PacketProcessor::TakeAnAnalogueReading(uint8_t mode)
{
  adcmode = mode;

  switch (adcmode)
  {
  case ADC_CELL_VOLTAGE:
  {
    _hardware->SelectCellVoltageChannel();
    break;
  }
  case ADC_INTERNAL_TEMP:
  {
    _hardware->SelectInternalTemperatureChannel();
    break;
  }
  case ADC_EXTERNAL_TEMP:
  {
    _hardware->SelectExternalTemperatureChannel();
    break;
  }
  default:
    //Avoid taking a reading if we get to here
    return;
  }

  _hardware->BeginADCReading();
}

//Run when a new packet is received over serial
bool PacketProcessor::onPacketReceived(PacketStruct *receivebuffer)
{
  // Process your decoded incoming packet here.
  //if (len == sizeof(buffer))
  //{

  //Copy to our buffer (probably a better way to share memory than this)
  //memcpy(&buffer, re1ceivebuffer, sizeof(PacketStruct));

  //Calculate the CRC and compare to received
  uint16_t validateCRC = CRC16::CalculateArray((unsigned char *)receivebuffer, sizeof(PacketStruct) - 2);

  if (validateCRC == receivebuffer->crc)
  {
    //TODO: We can probably get rid of mymoduleaddress
    mymoduleaddress = receivebuffer->hops;

    bool isPacketForMe = receivebuffer->start_address <= mymoduleaddress && receivebuffer->end_address >= mymoduleaddress;

    //Increment the hops no matter what (on valid CRC)
    receivebuffer->hops++;

    bool commandProcessed = false;
    //It's a good packet
    if (isPacketForMe)
    {
      commandProcessed = processPacket(receivebuffer);

      if (commandProcessed)
      {
        //Set flag to indicate we processed packet (other modules may also do this)
        receivebuffer->command = receivebuffer->command | B10000000;
      }
    }

    //Calculate new checksum over whole buffer (as hops as increased)
    receivebuffer->crc = CRC16::CalculateArray((unsigned char *)receivebuffer, sizeof(PacketStruct) - 2);

    //Return false the packet was not for me (but still a valid packet)...
    return commandProcessed;
    //}
  }

  //Clear the packet buffer on an invalid packet so the previous packet
  //is not re-transmitted issue #22
  //memset(&buffer, 0, sizeof(buffer));
  receivebuffer->crc =0;

  //We need to do something here, the packet received was not correct
  badpackets++;
  return false;
}

//Read cell voltage and return millivolt reading (16 bit unsigned)
uint16_t PacketProcessor::CellVoltage()
{
  //TODO: Get rid of the need for float variables?
  float v = ((float)raw_adc_voltage * (float)MV_PER_ADC) * _config->Calibration;

  return (uint16_t)v;
}

//Returns the last RAW ADC value 0-1023
uint16_t PacketProcessor::RawADCValue()
{
  return raw_adc_voltage;
}

// Process the request in the received packet
//command byte
// RRRR CCCC
// X    = 1 bit indicate if packet processed
// R    = 3 bits reserved not used
// C    = 4 bits command (16 possible commands)
bool PacketProcessor::processPacket(PacketStruct *buffer)
{
  switch (buffer->command & 0x0F)
  {
  case COMMAND::ResetBadPacketCounter:
    badpackets = 0;
    return true;

  case COMMAND::ReadVoltageAndStatus:
  {
    //Read voltage of VCC
    //Maximum voltage 8191mV
    buffer->moduledata[mymoduleaddress] = CellVoltage() & 0x1FFF;

    //3 top bits
    //X = In bypass
    //Y = Bypass over temperature
    //Z = Not used

    if (BypassOverheatCheck())
    {
      //Set bit
      buffer->moduledata[mymoduleaddress] = buffer->moduledata[mymoduleaddress] | 0x4000;
    }

    if (IsBypassActive())
    {
      //Set bit
      buffer->moduledata[mymoduleaddress] = buffer->moduledata[mymoduleaddress] | 0x8000;
    }

    return true;
  }

  case COMMAND::Identify:
  {
    //identify module
    //For the next 10 received packets - keep the LEDs lit up
    identifyModule = 10;
    return true;
  }

  case COMMAND::ReadTemperature:
  {
    //Return the last known temperature values recorded by the ADC (both internal and external)
    buffer->moduledata[mymoduleaddress] = TemperatureMeasurement();
    return true;
  }

  case COMMAND::ReadBalancePowerPWM:
  {
    //Read the last PWM value
    //Use WeAreInBypass instead of IsByPassActive() as the later also includes the "settle" time
    buffer->moduledata[mymoduleaddress] = WeAreInBypass ? (PWMValue & 0xFF) : 0;
    return true;
  }

  case COMMAND::ReadBadPacketCounter:
  {
    //Report number of bad packets
    buffer->moduledata[mymoduleaddress] = badpackets;
    return true;
  }

  case COMMAND::ReadSettings:
  {
    //Report settings/configuration

    FLOATUNION_t myFloat;
    myFloat.number = (float)LOAD_RESISTANCE;
    buffer->moduledata[0] = myFloat.word[0];
    buffer->moduledata[1] = myFloat.word[1];

    myFloat.number = _config->Calibration;
    buffer->moduledata[2] = myFloat.word[0];
    buffer->moduledata[3] = myFloat.word[1];

    myFloat.number = (float)MV_PER_ADC;
    buffer->moduledata[4] = myFloat.word[0];
    buffer->moduledata[5] = myFloat.word[1];

    buffer->moduledata[6] = _config->BypassTemperatureSetPoint;
    buffer->moduledata[7] = _config->BypassThresholdmV;
    buffer->moduledata[8] = INT_BCOEFFICIENT;
    buffer->moduledata[9] = EXT_BCOEFFICIENT;
    buffer->moduledata[10] = DIYBMSMODULEVERSION;

    //Version of firmware.
    buffer->moduledata[15] = MODULE_FIRMWARE_VERSION;
    return true;
  }

  case COMMAND::WriteSettings:
  {
    FLOATUNION_t myFloat;

    myFloat.word[0] = buffer->moduledata[0];
    myFloat.word[1] = buffer->moduledata[1];
    //if (myFloat.number < 0xFFFF)
    //{
    //      _config->LoadResistance = myFloat.number;
    //}

    myFloat.word[0] = buffer->moduledata[2];
    myFloat.word[1] = buffer->moduledata[3];
    if (myFloat.number < 0xFFFF)
    {
      _config->Calibration = myFloat.number;
    }

    //myFloat.word[0] = buffer.moduledata[4];
    //myFloat.word[1] = buffer.moduledata[5];
    //if (myFloat.number < 0xFFFF)
    //{
    //  _config->mVPerADC = (float)MV_PER_ADC;
    //}

    if (buffer->moduledata[6] != 0xFF)
    {
      _config->BypassTemperatureSetPoint = buffer->moduledata[6];

#if defined(DIYBMSMODULEVERSION) && (DIYBMSMODULEVERSION == 420 && !defined(SWAPR19R20))
      //Keep temperature low for modules with R19 and R20 not swapped
      if (_config->BypassTemperatureSetPoint > 45)
      {
        _config->BypassTemperatureSetPoint = 45;
      }
#endif
    }

    if (buffer->moduledata[7] != 0xFFFF)
    {
      _config->BypassThresholdmV = buffer->moduledata[7];
    }
    //if (buffer.moduledata[8] != 0xFFFF)
    //{
    //      _config->Internal_BCoefficient = buffer.moduledata[8];
    //}

    //if (buffer.moduledata[9] != 0xFFFF)
    //{
    //      _config->External_BCoefficient = buffer.moduledata[9];
    //}

    //Save settings
    Settings::WriteConfigToEEPROM((uint8_t *)_config, sizeof(CellModuleConfig), EEPROM_CONFIG_ADDRESS);

    SettingsHaveChanged = true;

    return true;
  }
  }

  return false;
}

uint16_t PacketProcessor::TemperatureMeasurement()
{
  return (Steinhart::TemperatureToByte(Steinhart::ThermistorToCelcius(INT_BCOEFFICIENT, onboard_temperature)) << 8) +
         Steinhart::TemperatureToByte(Steinhart::ThermistorToCelcius(EXT_BCOEFFICIENT, external_temperature));
}
