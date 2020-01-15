/*
 * config.h
 *
 * allows the user to configure static parameters.
 *
 * Note: Make sure with all pin defintions of your hardware that each pin number is
 *       only defined once.

 Copyright (c) 2019 - Collin Kidder

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *      Author: Michael Neuweiler
 */

#ifndef CONFIG_H_
#define CONFIG_H_

#include "esp32_can.h"

//if BLUETOOTH is defined we'll use bluetooth instead of wifi. It changes things alot so be warned.
//#define BLUETOOTH

#define CFG_BUILD_NUM   112
#define CFG_VERSION "Macchina OBDII May 1 2019"
#define EEPROM_VER      0x24
//How many devices to allow to connect to our WiFi port?
#define MAX_CLIENTS 1

#define WIFI_BUFF_SIZE      8192

#define NUM_PASS_FILTERS    32
#define NUM_STM_BUFFER      32

//How frequently to flush the serial buffer to wifi or bluetooth
#define SER_BUFF_FLUSH_INTERVAL 50000

struct EEPROMSettings {
    uint8_t version;

    uint32_t CAN0Speed;
    boolean CAN0_Enabled;

    uint8_t logLevel; //Level of logging to output on serial line

    uint16_t valid; //stores a validity token to make sure EEPROM is not corrupt

    char softSSID[33];
    char softWPA2KEY[33];
    bool softAPMode;
    char clientSSID[33];
    char clientWPA2KEY[33];
    char btName[32];
    uint8_t wifiChannel;
    uint8_t wifiTxPower;
};

enum STATE {
    IDLE,
    GET_COMMAND,
    BUILD_CAN_FRAME,
    TIME_SYNC,
    GET_DIG_INPUTS,
    GET_ANALOG_INPUTS,
    SET_DIG_OUTPUTS,
    SETUP_CANBUS,
    GET_CANBUS_PARAMS,
    GET_DEVICE_INFO,
    SET_SINGLEWIRE_MODE,
    SET_SYSTYPE,
    ECHO_CAN_FRAME,
    SETUP_EXT_BUSES
};

enum GVRET_PROTOCOL
{
    PROTO_BUILD_CAN_FRAME = 0,
    PROTO_TIME_SYNC = 1,
    PROTO_DIG_INPUTS = 2,
    PROTO_ANA_INPUTS = 3,
    PROTO_SET_DIG_OUT = 4,
    PROTO_SETUP_CANBUS = 5,
    PROTO_GET_CANBUS_PARAMS = 6,
    PROTO_GET_DEV_INFO = 7,
    PROTO_SET_SW_MODE = 8,
    PROTO_KEEPALIVE = 9,
    PROTO_SET_SYSTYPE = 10,
    PROTO_ECHO_CAN_FRAME = 11,
    PROTO_GET_NUMBUSES = 12,
    PROTO_GET_EXT_BUSES = 13,
    PROTO_SET_EXT_BUSES = 14
};

extern EEPROMSettings settings;

#endif /* CONFIG_H_ */
