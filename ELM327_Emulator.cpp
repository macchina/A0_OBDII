/*
 *  ELM327_Emu.cpp
 *
 * Class emulates the serial comm of an ELM327 chip - Used to create an OBDII interface
 *
 * Created: 3/23/2017
 *  Author: Collin Kidder
 */

/*
 Copyright (c) 2017 Collin Kidder

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

#include "ELM327_Emulator.h"
#include <esp32_can.h>
#include "config.h"
#ifdef BLUETOOTH
#include "BluetoothSerial.h"
BluetoothSerial SerialBT;
#else
#include <WiFi.h>
#include <WiFiMulti.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
extern WiFiClient clientNodes[MAX_CLIENTS];
#endif

extern EEPROMSettings settings;

CAN_FRAME stmBuff[NUM_STM_BUFFER];
int stmWriteIdx = 0;
int stmReadIdx = 0;

uint32_t passFilter[NUM_PASS_FILTERS];
uint32_t passMask[NUM_PASS_FILTERS];

//192,168.0.10 - our IP address
//port 35000 - listen on this port

/*
 * Constructor. Assign serial interface to use for comm with bluetooth adapter we're emulating with
 */
ELM327Emu::ELM327Emu() {
}

/*
 * Initialization of hardware and parameters
 */
void ELM327Emu::setup() {
    tickCounter = 0;
    ibWritePtr = 0;
    stmActive = false;

    for (int i = 0; i < NUM_PASS_FILTERS; i++)
    {
        passFilter[i] = 0;
        passMask[i] = 0;
    }
#ifdef BLUETOOTH
    SerialBT.begin(settings.btName);
#endif
}

void ELM327Emu::processByte(int incoming) {
    if (incoming != -1) { //and there is no reason it should be -1
        if (incoming == 13 || ibWritePtr > 126) { // on CR or full buffer, process the line
            incomingBuffer[ibWritePtr] = 0; //null terminate the string
            ibWritePtr = 0; //reset the write pointer

            processCmd();

        } else { // add more characters
            if (incoming != 10 && incoming != ' ') // don't add a LF character or spaces. Strip them right out
                incomingBuffer[ibWritePtr++] = (char)tolower(incoming); //force lowercase to make processing easier
        }
    } else return;
}

void ELM327Emu::loop()
{
#ifdef BLUETOOTH
    int incoming;
    while (SerialBT.available()) {
        incoming = SerialBT.read();
        processByte(incoming);
    }
#endif
}

void ELM327Emu::processFrame(CAN_FRAME &frame)
{
#ifdef BLUETOOTH
    String retString = String();
    //Logger::debug("Id: %x", frame.id);
    //for (int i = 0; i < NUM_PASS_FILTERS; i++)
    //{
        //if (passFilter[i] > 0)
        //{
            //uint32_t maskedID = frame.id & passMask[i];
            //if (maskedID == passFilter[i]) 
            //{
                int newIdx = (stmWriteIdx + 1) % NUM_STM_BUFFER;
                if (newIdx != stmReadIdx) 
                {
                    stmBuff[stmWriteIdx] = frame;
                    stmWriteIdx = newIdx;
                }
            //}
        //}
    //}
    if (stmActive)
    {
        if (stmReadIdx != stmWriteIdx)
        {
            while (stmReadIdx != stmWriteIdx)
            {
                char buff[40];
                sprintf(buff, "%03X", stmBuff[stmReadIdx].id);
                retString.concat(buff);
                for (int d = 0; d < stmBuff[stmReadIdx].length; d++)
                {
                    sprintf(buff, "%02X", stmBuff[stmReadIdx].data.bytes[d]);
                    retString.concat(buff);
                }
                stmReadIdx = (stmReadIdx + 1) % NUM_STM_BUFFER;
                retString.concat("\r");                
            }
            /*
            if (Logger::isDebug()) {
                Logger::debug("In: %s", incomingBuffer);
                char buff[150];
                retString.toCharArray(buff, 150);
                Logger::debug("Out: %s", buff);
            } */
            SerialBT.print(retString);
            //stmActive = false;
        }
    }
#endif
}

/*
*   There is no need to pass the string in here because it is local to the class so this function can grab it by default
*   But, for reference, this cmd processes the command in incomingBuffer
*/
void ELM327Emu::processCmd() {
    String retString = processELMCmd(incomingBuffer);            
#ifdef BLUETOOTH
    SerialBT.print(retString);
    if (Logger::isDebug()) {
        Logger::debug("In: %s", incomingBuffer);
        char buff[150];
        retString.toCharArray(buff, 150);
        Logger::debug("Out: %s", buff);
    }
    
#else
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clientNodes[i] && clientNodes[i].connected())
        {
            clientNodes[i].print(retString);
        }
    }
#endif    

}

String ELM327Emu::processELMCmd(char *cmd) {
    String retString = String();
    String lineEnding;
    if (bLineFeed) lineEnding = String("\r\n");
    else lineEnding = String("\r");

    if (!strncmp(cmd, "at", 2)) {

        if (!strcmp(cmd, "atz") || !strcmp(cmd, "atws")) { //reset hardware
            retString.concat(lineEnding);
            retString.concat("ELM327 v1.3a");
        }
        else if (!strncmp(cmd, "atsh",4)) { //set header address
            //ignore this - just say OK
            retString.concat("OK");
        }
        else if (!strncmp(cmd, "ate",3)) { //turn echo on/off
            //could support echo but I don't see the need, just ignore this
            retString.concat("OK");
        }
        else if (!strncmp(cmd, "ath",3)) { //turn headers on/off
            if (cmd[3] == '1') bHeader = true;
            else bHeader = false;
            retString.concat("OK");
        }
        else if (!strncmp(cmd, "atl",3)) { //turn linefeeds on/off
            if (cmd[3] == '1') bLineFeed = true;
            else bLineFeed = false;
            retString.concat("OK");
        }
        else if (!strcmp(cmd, "at@1")) { //send device description
            retString.concat("ELM327 Emulator");
        }
        else if (!strcmp(cmd, "ati")) { //send chip ID
            retString.concat("ELM327 v1.3a");
        }
        else if (!strncmp(cmd, "atat",4)) { //set adaptive timing
            //don't intend to support adaptive timing at all
            retString.concat("OK");
        }
        else if (!strncmp(cmd, "atsp",4)) { //set protocol
            //theoretically we can ignore this
            retString.concat("OK");
        }
        else if (!strncmp(cmd, "atcaf", 5)) { //set can auto format
            //cmd[5] would be 0 for off, 1 for on
            retString.concat("OK");
        }
        else if (!strcmp(cmd, "atdp")) { //show description of protocol
            retString.concat("can11/500");
        }
        else if (!strcmp(cmd, "atdpn")) { //show protocol number (same as passed to sp)
            retString.concat("6");
        }
        else if (!strcmp(cmd, "atd")) { //set to defaults
            retString.concat("OK");
        }
        else if (!strncmp(cmd, "atm", 3)) { //turn memory on/off
            retString.concat("OK");
        }
        else if (!strcmp(cmd, "atrv")) { //show 12v rail voltage
            //TODO: the system should actually have this value so it wouldn't hurt to
            //look it up and report the real value.
            retString.concat("14.2V");
        }
        else { //by default respond to anything not specifically handled by just saying OK and pretending.
            retString.concat("OK");
        }
    }
    else if (!strncmp(cmd, "st", 2)) 
    {
        //When STM is encountered nothing is returned if there are no messages. If there is a message it goes
        //IIIdddddddddddddddd
        //where III are the three hex digits for the ID then dd are the hex digits for the data payload
        //Each frame returned (can return multiple frames) is separated by \r
        if (!strncmp(cmd, "stm", 3)) { //monitor CAN bus using current filters
            stmActive = true;
            if (stmReadIdx != stmWriteIdx)
            {
                while (stmReadIdx != stmWriteIdx)
                {
                    char buff[40];
                    sprintf(buff, "%03X", stmBuff[stmReadIdx].id);
                    retString.concat(buff);
                    for (int d = 0; d < stmBuff[stmReadIdx].length; d++)
                    {
                        sprintf(buff, "%02X", stmBuff[stmReadIdx].data.bytes[d]);
                        retString.concat(buff);
                    }
                    stmReadIdx = (stmReadIdx + 1) % NUM_STM_BUFFER;
                    retString.concat("\r");                
                }
            }
            else 
            {
                retString.concat("\r");                
            }
            return retString;
        } 
        if (!strncmp(cmd, "stdi", 4)) { //output device hardware string
            retString.concat("STM1110 v1.5");
        } 
        if (!strncmp(cmd, "stfcp", 4)) { //clear pass filters
            for (int i = 0; i < NUM_PASS_FILTERS; i++)
            {
                passFilter[i] = 0;
                passMask[i] = 0;
            }
            retString.concat("OK");
        } 
        if (!strncmp(cmd, "stfap", 4)) { //add a pass filter
            char id[10];
            strcpy(id, strtok((char *)(cmd + 5), ","));
            char mask[10];
            strcpy(mask, strtok(NULL, ","));
            //Logger::console("\n\nID %x", strtol(id, 0, 16));
            //Logger::console("Mask %x", strtol(mask, 0, 16));
            for (int i = 0; i < NUM_PASS_FILTERS; i++)
            {
                if (passFilter[i] == 0)
                {
                    passFilter[i] = strtol(id, 0, 16);
                    passMask[i] = strtol(mask, 0, 16);
                    Logger::debug("ID: %x Mask: %x", passFilter[i], passMask[i]);
                    break;
                }
            }
            retString.concat("OK");
        } 
    }
    else { //if no AT then assume it is a PID request. This takes the form of four bytes which form the alpha hex digit encoding for two bytes
        //there should be four or six characters here forming the ascii representation of the PID request. Easiest for now is to turn the ascii into
        //a 16 bit number and mask off to get the bytes
        if (strlen(cmd) == 4) {
            uint32_t valu = strtol((char *) cmd, NULL, 16); //the pid format is always in hex
            uint8_t pidnum = (uint8_t)(valu & 0xFF);
            uint8_t mode = (uint8_t)((valu >> 8) & 0xFF);
            Logger::debug("Mode: %i, PID: %i", mode, pidnum);

            CAN_FRAME frame;
            frame.id = 0x7E0;
            frame.length=8;
            frame.rtr = 0;
            frame.extended = 0;
            frame.data.byte[0] = 2;
            frame.data.byte[1] = mode;
            frame.data.byte[2] = pidnum;
            frame.data.byte[3] = 0xAA;
            frame.data.s2 = 0xAAAA;
            frame.data.s3 = 0xAAAA;

            CAN0.sendFrame(frame);
        }
    }

    retString.concat(lineEnding);
    retString.concat(">"); //prompt to show we're ready to receive again

    return retString;
}

void ELM327Emu::sendOBDReply(CAN_FRAME &frame)
{
    String retString = String();
    char buff[30];

    if (bHeader) { //ID and length only sent when other side has requested headers.
        sprintf(buff, "%03X", frame.id);
        retString.concat(buff);
        sprintf(buff, "%02X", frame.data.byte[0]);
        retString.concat(buff);
    }
    for (int i = 1; i < 8; i++) 
    {
        sprintf(buff, "%02X", frame.data.byte[i]);
        retString.concat(buff);
    }
#ifdef BLUETOOTH
    SerialBT.print(retString);
#else
    for(int i = 0; i < MAX_CLIENTS; i++)
    {
        if (clientNodes[i] && clientNodes[i].connected())
        {
            clientNodes[i].print(retString);
        }
    }
#endif
}
