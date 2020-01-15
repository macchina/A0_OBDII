/*
 * SerialConsole.cpp
 *
 Copyright (c) 2014-2018 Collin Kidder

 Shamelessly copied from the version in GEVCU

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

 */

#include "SerialConsole.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <esp32_can.h>
#include "config.h"
#ifndef BLUETOOTH
#include <WiFi.h>
#endif
#include "EEPROM.h"
#include "Logger.h"

extern void CANHandler();
extern void execOTA();

SerialConsole::SerialConsole()
{
    init();
}

void SerialConsole::init()
{
    //State variables for serial console
    ptrBuffer = 0;
    state = STATE_ROOT_MENU;
}

void SerialConsole::printMenu()
{
    char buff[80];
    //Show build # here as well in case people are using the native port and don't get to see the start up messages
    Serial.print("Build number: ");
    Serial.println(CFG_BUILD_NUM);
    CAN1.printDebug();
    Serial.println("System Menu:");
    Serial.println();
    Serial.println("Enable line endings of some sort (LF, CR, CRLF)");
    Serial.println();
    Serial.println("Short Commands:");
    Serial.println("h = help (displays this message)");
    Serial.println("R = reset to factory defaults");
    Serial.println();
    Serial.println("Config Commands (enter command=newvalue). Current values shown in parenthesis:");
    Serial.println();

    Logger::console("LOGLEVEL=%i - set log level (0=debug, 1=info, 2=warn, 3=error, 4=off)", settings.logLevel);
    Serial.println();

    Logger::console("CAN0EN=%i - Enable/Disable CAN0 (0 = Disable, 1 = Enable)", settings.CAN0_Enabled);
    Logger::console("CAN0SPEED=%i - Set speed of CAN0 in baud (125000, 250000, etc)", settings.CAN0Speed);
    Serial.println();

#ifndef BLUETOOTH
    Logger::console("SSID=%s - SSID for creating a soft AP", settings.softSSID);
    Logger::console("WPA2KEY=%s - WPA2 key to use for softAP", settings.softWPA2KEY);
    Logger::console("WIFIMODE=%i - 0 = Connect to an AP as client, 1 = Make a softAP", settings.softAPMode);
    Logger::console("CLIENTSSID=%s - SSID to connect to as a client", settings.clientSSID);
    Logger::console("CLIENTWPA2KEY=%s - WPA2 key to use when connecting to AP", settings.clientWPA2KEY);
#else
    Logger::console("BTNAME=%s - Name for bluetooth", settings.btName);
#endif
    Serial.println();

#ifndef BLUETOOTH
    Logger::console("UPDATE - Get an update from S3 server (Requires you can connect to an AP)");
#endif    
    Serial.println();
}

/*	There is a help menu (press H or h or ?)
 This is no longer going to be a simple single character console.
 Now the system can handle up to 80 input characters. Commands are submitted
 by sending line ending (LF, CR, or both)
 */
void SerialConsole::rcvCharacter(uint8_t chr)
{
    if (chr == 10 || chr == 13) { //command done. Parse it.
        handleConsoleCmd();
        ptrBuffer = 0; //reset line counter once the line has been processed
    } else {
        cmdBuffer[ptrBuffer++] = (unsigned char) chr;
        if (ptrBuffer > 79)
            ptrBuffer = 79;
    }
}

void SerialConsole::handleConsoleCmd()
{
    if (state == STATE_ROOT_MENU) {
        if (ptrBuffer == 1) {
            //command is a single ascii character
            handleShortCmd();
        } else { //at least two bytes
#ifndef BLUETOOTH
            if (!strncmp(cmdBuffer, "UPDATE", 6)) execOTA();
            if (!strncmp(cmdBuffer, "update", 6)) execOTA();
#endif
            boolean equalSign = false;
            for (int i = 0; i < ptrBuffer; i++) if (cmdBuffer[i] == '=') equalSign = true;
            if (equalSign) handleConfigCmd();
        }
        ptrBuffer = 0; //reset line counter once the line has been processed
    }
}

void SerialConsole::handleShortCmd()
{
    uint8_t val;

    switch (cmdBuffer[0]) {
    //non-lawicel commands
    case 'h':
    case '?':
    case 'H':
        printMenu();
        break;
    case 'R': //reset to factory defaults.
        settings.version = 0xFF;
        EEPROM.writeBytes(0, &settings, sizeof(settings));
        Logger::console("Power cycle to reset to factory defaults");
        break;        
    }
}

void SerialConsole::handleConfigCmd()
{
    int i;
    int newValue;
    char *newString;
    bool writeEEPROM = false;
    bool needReboot = false;
    char *dataTok;

    //Logger::debug("Cmd size: %i", ptrBuffer);
    if (ptrBuffer < 6)
        return; //4 digit command, =, value is at least 6 characters
    cmdBuffer[ptrBuffer] = 0; //make sure to null terminate
    String cmdString = String();
    unsigned char whichEntry = '0';
    i = 0;

    while (cmdBuffer[i] != '=' && i < ptrBuffer) {
        cmdString.concat(String(cmdBuffer[i++]));
    }
    i++; //skip the =
    if (i >= ptrBuffer) {
        //Logger::console("Command needs a value..ie TORQ=3000");
        //Logger::console("");
        //return; //or, we could use this to display the parameter instead of setting
    }

    // strtol() is able to parse also hex values (e.g. a string "0xCAFE"), useful for enable/disable by device id
    newValue = strtol((char *) (cmdBuffer + i), NULL, 0); //try to turn the string into a number
    newString = (char *)(cmdBuffer + i); //leave it as a string

    cmdString.toUpperCase();

    if (cmdString == String("CAN0EN")) {
        if (newValue < 0) newValue = 0;
        if (newValue > 1) newValue = 1;
        Logger::console("Setting CAN0 Enabled to %i", newValue);
        settings.CAN0_Enabled = newValue;
        if (newValue == 1) 
        {
            //CAN0.enable();
            CAN0.begin(settings.CAN0Speed, 255);
            CAN0.watchFor();
        }
        else CAN0.disable();
        writeEEPROM = true;
    } else if (cmdString == String("CAN0SPEED")) {
        if (newValue > 0 && newValue <= 1000000) {
            Logger::console("Setting CAN0 Baud Rate to %i", newValue);
            settings.CAN0Speed = newValue;
            if (settings.CAN0_Enabled) CAN0.begin(settings.CAN0Speed, 255);
            writeEEPROM = true;
        } else Logger::console("Invalid baud rate! Enter a value 1 - 1000000");
    } else if (cmdString == String("WIFIMODE")) {
        if (newValue >= 0 && newValue <= 1) {
            Logger::console("Setting WifiMode to %i", newValue);
            settings.softAPMode = newValue;
            writeEEPROM = true;
            needReboot = true;
        } else Logger::console("Invalid setting! Enter a value 0 - 1");
    } else if (cmdString == String("WIFICHAN")) {
        if (newValue >= 1 && newValue <= 15) {
            Logger::console("Setting WiFi softAP channel to %i", newValue);
            settings.wifiChannel = newValue;
            writeEEPROM = true;
            needReboot = true;
        } else Logger::console("Invalid setting! Enter a value 1 - 15");
    } else if (cmdString == String("WIFIPOWER")) {
        if (newValue >= 8 && newValue <= 78) {
            Logger::console("Setting wifi transmit power to %i", newValue);
            settings.wifiTxPower = newValue;
            writeEEPROM = true;
            needReboot = true;
        } else Logger::console("Invalid setting! Enter one of 8,20,28,34,44,52,60,68,74,76,78");        
    } else if (cmdString == String("SSID")) {
        Logger::console("Setting softAP SSID to %s", newString);
        strncpy(settings.softSSID, newString, 32);
        writeEEPROM = true;
    } else if (cmdString == String("WPA2KEY")) {
        Logger::console("Setting softAP WPA2 Key to %s", newString);
        strncpy(settings.softWPA2KEY, newString, 32);
        writeEEPROM = true;
    } else if (cmdString == String("CLIENTSSID")) {
        Logger::console("Setting client SSID to %s", newString);
        strncpy(settings.clientSSID, newString, 32);
        writeEEPROM = true;
    } else if (cmdString == String("CLIENTWPA2KEY")) {
        Logger::console("Setting client WPA2 Key to %s", newString);
        strncpy(settings.clientWPA2KEY, newString, 32);
        writeEEPROM = true;
    } else if (cmdString == String("BTNAME")) {
        Logger::console("Setting bluetooth name to %s", newString);
        strncpy(settings.btName, newString, 32);
        writeEEPROM = true;
    } else if (cmdString == String("LOGLEVEL")) {
        switch (newValue) {
        case 0:
            Logger::setLoglevel(Logger::Debug);
            settings.logLevel = 0;
            Logger::console("setting loglevel to 'debug'");
            writeEEPROM = true;
            break;
        case 1:
            Logger::setLoglevel(Logger::Info);
            settings.logLevel = 1;
            Logger::console("setting loglevel to 'info'");
            writeEEPROM = true;
            break;
        case 2:
            Logger::console("setting loglevel to 'warning'");
            settings.logLevel = 2;
            Logger::setLoglevel(Logger::Warn);
            writeEEPROM = true;
            break;
        case 3:
            Logger::console("setting loglevel to 'error'");
            settings.logLevel = 3;
            Logger::setLoglevel(Logger::Error);
            writeEEPROM = true;
            break;
        case 4:
            Logger::console("setting loglevel to 'off'");
            settings.logLevel = 4;
            Logger::setLoglevel(Logger::Off);
            writeEEPROM = true;
            break;
        }
    } else {
        Logger::console("Unknown command");
    }
    if (writeEEPROM) {
        EEPROM.writeBytes(0, &settings, sizeof(settings));
        EEPROM.commit();
    }
    if (needReboot)
    {
        delay(1000); //let EEPROM write settle
#ifndef BLUETOOTH
        if (settings.softAPMode) WiFi.softAPdisconnect();
        else WiFi.disconnect();
#endif
        ESP.restart(); //and force a reboot.
    }
}

unsigned int SerialConsole::parseHexCharacter(char chr)
{
    unsigned int result = 0;
    if (chr >= '0' && chr <= '9') result = chr - '0';
    else if (chr >= 'A' && chr <= 'F') result = 10 + chr - 'A';
    else if (chr >= 'a' && chr <= 'f') result = 10 + chr - 'a';

    return result;
}

unsigned int SerialConsole::parseHexString(char *str, int length)
{
    unsigned int result = 0;
    for (int i = 0; i < length; i++) result += parseHexCharacter(str[i]) << (4 * (length - i - 1));
    return result;
}

//Tokenize cmdBuffer on space boundaries - up to 10 tokens supported
void SerialConsole::tokenizeCmdString() {
   int idx = 0;
   char *tok;
   
   for (int i = 0; i < 13; i++) tokens[i][0] = 0;
   
   tok = strtok(cmdBuffer, " ");
   if (tok != nullptr) strcpy(tokens[idx], tok);
       else tokens[idx][0] = 0;
   while (tokens[idx] != nullptr && idx < 13) {
       idx++;
       tok = strtok(nullptr, " ");
       if (tok != nullptr) strcpy(tokens[idx], tok);
            else tokens[idx][0] = 0;
   }
}

void SerialConsole::uppercaseToken(char *token) {
    int idx = 0;
    while (token[idx] != 0 && idx < 9) {
        token[idx] = toupper(token[idx]);
        idx++;
    }
    token[idx] = 0;
}

void SerialConsole::printBusName(int bus) {
    switch (bus) {
    case 0:
        Serial.print("CAN0");
        break;
    case 1:
        Serial.print("CAN1");
        break;
    default:
        Serial.print("UNKNOWN");
        break;
    }
}
