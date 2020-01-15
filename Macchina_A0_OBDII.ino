/*
  Macchina_A0_OBDII.ino

  Sketch for the Macchina A0 that turns it into either a bluetooth or WiFi OBDII dongle

  Created: May 1, 2019
  Author: Collin Kidder

  Copyright (c) 2019 Collin Kidder

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
#include "config.h"
#include <esp32_can.h>
#include <SPI.h>
#include "EEPROM.h"
#include "Logger.h"
#include "SerialConsole.h"
#include "ELM327_Emulator.h"
#include <iso-tp.h>
#include "obd2_codes.h"

#ifndef BLUETOOTH
#include <WiFi.h>
#include <WiFiMulti.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#endif

byte i = 0;

byte serialBuffer[WIFI_BUFF_SIZE];
int serialBufferLength = 0; //not creating a ring buffer. The buffer should be large enough to never overflow
uint32_t lastFlushMicros = 0;
uint32_t lastBroadcast = 0;
EEPROMSettings settings;
SerialConsole console;
ELM327Emu elmEmulator;

#ifndef BLUETOOTH
WiFiClient clientNodes[MAX_CLIENTS];
WiFiClient savvyClient;
WiFiClient wifiClient;
WiFiServer wifiServer(35000);
WiFiServer savvyServer(23);
WiFiUDP wifiUDPServer;
IPAddress broadcastAddr(255, 255, 255, 255);
int OTAcount = 0;
#endif

bool test = false;
uint8_t testcount = 0;
bool isWifiConnected = false;

void execOTA();

template<class T> inline Print &operator <<(Print &obj, T arg) {
  obj.print(arg);
  return obj;
}

//initializes all the system EEPROM values. Chances are this should be broken out a bit but
//there is only one checksum check for all of them so it's simple to do it all here.
void loadSettings()
{
  Logger::console("Loading settings....");

  if (!EEPROM.begin(1024))
  {
    Serial.println("Could not initialize EEPROM!"); delay(1000000);
    return;
  }

  EEPROM.readBytes(0, &settings, sizeof(settings));

  if (settings.version != EEPROM_VER) { //if settings are not the current version then erase them and set defaults
    Logger::console("Resetting to factory defaults");
    settings.version = EEPROM_VER;
    settings.CAN0Speed = 500000;
    settings.CAN0_Enabled = true;
    settings.valid = 0; //not used right now
    settings.logLevel = 1; //info instead of debug
    strcpy(settings.softSSID, "MACCHINAOBD2");
    settings.softWPA2KEY[0] = 0; //no password by default
    settings.softAPMode = 1; //create an AP by default
    settings.wifiChannel = 13;
    settings.wifiTxPower = 78; //19.5dB tx power - the max. Values are in quarter dB
    strcpy(settings.clientSSID, "YOURAP");
    strcpy(settings.clientWPA2KEY, "Password");
    strcpy(settings.btName, "MACCHINAOBDII");
    EEPROM.writeBytes(0, &settings, sizeof(settings));
    EEPROM.commit();
  } else {
    Logger::console("Using stored values from EEPROM");
    if (settings.wifiChannel > 15 || settings.wifiChannel < 1) settings.wifiChannel = 13;
    if (settings.wifiTxPower < 8 || settings.wifiTxPower > 78) settings.wifiTxPower = 78;
  }

  Logger::setLoglevel((Logger::LogLevel)settings.logLevel);
}

void setPromiscuousMode()
{
  CAN0.watchFor();
}

void setup()
{
  //delay(5000); //just for testing. Don't use in production

  Serial.begin(115200);

  loadSettings();

  Serial.print("Build number: ");
  Serial.println(CFG_BUILD_NUM);

  if (settings.CAN0_Enabled) {
    CAN0.setCANPins(GPIO_NUM_4, GPIO_NUM_5); //rx, tx
    CAN0.enable();
    CAN0.begin(settings.CAN0Speed, 255);
    Serial.print("Enabled CAN0 with speed ");
    Serial.println(settings.CAN0Speed);
    CAN0.setListenOnlyMode(false);
    CAN0.watchFor();
  }
  else
  {
    CAN0.disable();
  }

  CAN1.disable();

  setPromiscuousMode();

#ifndef BLUETOOTH
  if (settings.softAPMode)
  {
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192, 168, 0, 10), IPAddress(192, 168, 0, 10), IPAddress(255, 255, 255, 0));
    WiFi.softAP((const char *)settings.softSSID, (const char *)settings.softWPA2KEY, settings.wifiChannel);
    WiFi.setTxPower((wifi_power_t)settings.wifiTxPower);
    //Logger::console("Tx Power: %i", WiFi.getTxPower());
  }
  else
  {
    WiFi.mode(WIFI_STA);
    WiFi.begin((const char *)settings.clientSSID, (const char *)settings.clientWPA2KEY);
    WiFi.setTxPower((wifi_power_t)settings.wifiTxPower);
  }
#endif

  elmEmulator.setup();

  Serial.print("Done with init\n");
}

//Get the value of XOR'ing all the bytes together. This creates a reasonable checksum that can be used
//to make sure nothing too stupid has happened on the comm.
uint8_t checksumCalc(uint8_t *buffer, int length)
{
  uint8_t valu = 0;
  for (int c = 0; c < length; c++) {
    valu ^= buffer[c];
  }
  return valu;
}

void sendFrameToWiFi(CAN_FRAME &frame, int whichBus)
{
  uint8_t buff[40];
  uint8_t writtenBytes;
  uint8_t temp;
  uint32_t now = micros();

  if (frame.extended) frame.id |= 1 << 31;
  serialBuffer[serialBufferLength++] = 0xF1;
  serialBuffer[serialBufferLength++] = 0; //0 = canbus frame sending
  serialBuffer[serialBufferLength++] = (uint8_t)(now & 0xFF);
  serialBuffer[serialBufferLength++] = (uint8_t)(now >> 8);
  serialBuffer[serialBufferLength++] = (uint8_t)(now >> 16);
  serialBuffer[serialBufferLength++] = (uint8_t)(now >> 24);
  serialBuffer[serialBufferLength++] = (uint8_t)(frame.id & 0xFF);
  serialBuffer[serialBufferLength++] = (uint8_t)(frame.id >> 8);
  serialBuffer[serialBufferLength++] = (uint8_t)(frame.id >> 16);
  serialBuffer[serialBufferLength++] = (uint8_t)(frame.id >> 24);
  serialBuffer[serialBufferLength++] = frame.length + (uint8_t)(whichBus << 4);
  for (int c = 0; c < frame.length; c++) {
    serialBuffer[serialBufferLength++] = frame.data.uint8[c];
  }
  //temp = checksumCalc(buff, 11 + frame.length);
  temp = 0;
  serialBuffer[serialBufferLength++] = temp;
  //Serial.write(buff, 12 + frame.length);
}

//very cut down version of the one from ESP32RET. Just the bare minumum support
void processIncomingByte(uint8_t in_byte)
{
  static CAN_FRAME build_out_frame;
  static int out_bus;
  static byte buff[20];
  static int step = 0;
  static STATE state = IDLE;
  static uint32_t build_int;
  uint32_t busSpeed = 0;
  uint32_t now = micros();

  uint8_t temp8;
  uint16_t temp16;

  switch (state) {
    case IDLE:
      if (in_byte == 0xF1)
      {
        state = GET_COMMAND;
      }
      else if (in_byte == 0xE7)
      {
        setPromiscuousMode(); //going into binary comm will set promisc. mode too.
      }
      else
      {
        console.rcvCharacter(in_byte);
      }
      break;
    case GET_COMMAND:
      switch (in_byte)
      {
        case PROTO_BUILD_CAN_FRAME:
          state = BUILD_CAN_FRAME;
          buff[0] = 0xF1;
          step = 0;
          break;
        case PROTO_TIME_SYNC:
          state = TIME_SYNC;
          step = 0;
          serialBuffer[serialBufferLength++] = 0xF1;
          serialBuffer[serialBufferLength++] = 1; //time sync
          serialBuffer[serialBufferLength++] = (uint8_t) (now & 0xFF);
          serialBuffer[serialBufferLength++] = (uint8_t) (now >> 8);
          serialBuffer[serialBufferLength++] = (uint8_t) (now >> 16);
          serialBuffer[serialBufferLength++] = (uint8_t) (now >> 24);
          break;
        case PROTO_SETUP_CANBUS:
          state = SETUP_CANBUS;
          step = 0;
          buff[0] = 0xF1;
          break;
        case PROTO_GET_CANBUS_PARAMS:
          //immediately return data on canbus params
          serialBuffer[serialBufferLength++] = 0xF1;
          serialBuffer[serialBufferLength++] = 6;
          serialBuffer[serialBufferLength++] = settings.CAN0_Enabled;// + ((unsigned char) settings.CAN0ListenOnly << 4);
          serialBuffer[serialBufferLength++] = settings.CAN0Speed;
          serialBuffer[serialBufferLength++] = settings.CAN0Speed >> 8;
          serialBuffer[serialBufferLength++] = settings.CAN0Speed >> 16;
          serialBuffer[serialBufferLength++] = settings.CAN0Speed >> 24;
          serialBuffer[serialBufferLength++] = 0; //settings.CAN1_Enabled + ((unsigned char) settings.CAN1ListenOnly << 4); //+ (unsigned char)settings.singleWireMode << 6;
          serialBuffer[serialBufferLength++] = 0;//settings.CAN1Speed;
          serialBuffer[serialBufferLength++] = 0;//settings.CAN1Speed >> 8;
          serialBuffer[serialBufferLength++] = 0;//settings.CAN1Speed >> 16;
          serialBuffer[serialBufferLength++] = 0;//settings.CAN1Speed >> 24;
          state = IDLE;
          break;
        case PROTO_GET_DEV_INFO:
          //immediately return device information
          serialBuffer[serialBufferLength++] = 0xF1;
          serialBuffer[serialBufferLength++] = 7;
          serialBuffer[serialBufferLength++] = CFG_BUILD_NUM & 0xFF;
          serialBuffer[serialBufferLength++] = (CFG_BUILD_NUM >> 8);
          serialBuffer[serialBufferLength++] = EEPROM_VER;
          serialBuffer[serialBufferLength++] = 0;
          serialBuffer[serialBufferLength++] = 0;
          serialBuffer[serialBufferLength++] = 0; //was single wire mode. Should be rethought for this board.
          state = IDLE;
          break;
        case PROTO_KEEPALIVE:
          serialBuffer[serialBufferLength++] = 0xF1;
          serialBuffer[serialBufferLength++] = 0x09;
          serialBuffer[serialBufferLength++] = 0xDE;
          serialBuffer[serialBufferLength++] = 0xAD;
          state = IDLE;
          break;
        case PROTO_GET_NUMBUSES:
          serialBuffer[serialBufferLength++] = 0xF1;
          serialBuffer[serialBufferLength++] = 12;
          serialBuffer[serialBufferLength++] = 2; //just CAN0 and CAN1 on this hardware
          state = IDLE;
          break;
        case PROTO_GET_EXT_BUSES:
          serialBuffer[serialBufferLength++]  = 0xF1;
          serialBuffer[serialBufferLength++]  = 13;
          for (int u = 2; u < 17; u++) serialBuffer[serialBufferLength++] = 0;
          step = 0;
          state = IDLE;
          break;
        case PROTO_SET_EXT_BUSES:
          state = SETUP_EXT_BUSES;
          step = 0;
          buff[0] = 0xF1;
          break;
      }
      break;
    case BUILD_CAN_FRAME:
      buff[1 + step] = in_byte;
      switch (step)
      {
        case 0:
          build_out_frame.id = in_byte;
          break;
        case 1:
          build_out_frame.id |= in_byte << 8;
          break;
        case 2:
          build_out_frame.id |= in_byte << 16;
          break;
        case 3:
          build_out_frame.id |= in_byte << 24;
          if (build_out_frame.id & 1 << 31)
          {
            build_out_frame.id &= 0x7FFFFFFF;
            build_out_frame.extended = true;
          } else build_out_frame.extended = false;
          break;
        case 4:
          out_bus = in_byte & 3;
          break;
        case 5:
          build_out_frame.length = in_byte & 0xF;
          if (build_out_frame.length > 8) build_out_frame.length = 8;
          break;
        default:
          if (step < build_out_frame.length + 6)
          {
            build_out_frame.data.bytes[step - 6] = in_byte;
          }
          else
          {
            state = IDLE;
            //this would be the checksum byte. Compute and compare.
            temp8 = checksumCalc(buff, step);
            build_out_frame.rtr = 0;
            if (out_bus == 0)
            {
              CAN0.sendFrame(build_out_frame);
            }
            if (out_bus == 1)
            {
              //CAN1.sendFrame(build_out_frame);
            }
          }
          break;
      }
      step++;
      break;
    case TIME_SYNC:
      state = IDLE;
      break;
    case SETUP_CANBUS: //todo: validate checksum
      switch (step)
      {
        case 0:
          build_int = in_byte;
          break;
        case 1:
          build_int |= in_byte << 8;
          break;
        case 2:
          build_int |= in_byte << 16;
          break;
        case 3:
          build_int |= in_byte << 24;
          busSpeed = build_int & 0xFFFFF;
          if (busSpeed > 1000000) busSpeed = 1000000;

          if (build_int > 0)
          {
            if (build_int & 0x80000000ul) //signals that enabled and listen only status are also being passed
            {
              if (build_int & 0x40000000ul)
              {
                settings.CAN0_Enabled = true;
              } else
              {
                settings.CAN0_Enabled = false;
              }
              if (build_int & 0x20000000ul)
              {
                //settings.CAN0ListenOnly = true;
              } else
              {
                //settings.CAN0ListenOnly = false;
              }
            } else
            {
              //if not using extended status mode then just default to enabling - this was old behavior
              settings.CAN0_Enabled = true;
            }
            //CAN0.set_baudrate(build_int);
            settings.CAN0Speed = busSpeed;
          } else { //disable first canbus
            settings.CAN0_Enabled = false;
          }

          if (settings.CAN0_Enabled)
          {
            CAN0.begin(settings.CAN0Speed, 255);
            CAN0.setListenOnlyMode(false);
            CAN0.watchFor();
          }
          else CAN0.disable();
          break;
        case 4:
          build_int = in_byte;
          break;
        case 5:
          build_int |= in_byte << 8;
          break;
        case 6:
          build_int |= in_byte << 16;
          break;
        case 7:
          build_int |= in_byte << 24;
          busSpeed = build_int & 0xFFFFF;
          if (busSpeed > 1000000) busSpeed = 1000000;

          if (build_int > 0) {
            if (build_int & 0x80000000) { //signals that enabled and listen only status are also being passed
              if (build_int & 0x40000000) {
                //settings.CAN1_Enabled = true;
              } else {
                //settings.CAN1_Enabled = false;
              }
              if (build_int & 0x20000000) {
                //settings.CAN1ListenOnly = true;
              } else {
                //settings.CAN1ListenOnly = false;
              }
            } else {
              //if not using extended status mode then just default to enabling - this was old behavior
              //settings.CAN1_Enabled = true;
            }
            //CAN1.set_baudrate(build_int);
            //settings.CAN1Speed = busSpeed;
          } else { //disable second canbus
            //settings.CAN1_Enabled = false;
          }

          CAN1.disable();

          state = IDLE;
          //now, write out the new canbus settings to EEPROM
          EEPROM.writeBytes(0, &settings, sizeof(settings));
          EEPROM.commit();
          //setPromiscuousMode();
          break;
      }
      step++;
      break;
    case GET_CANBUS_PARAMS:
      // nothing to do
      break;
    case GET_DEVICE_INFO:
      // nothing to do
      break;
    case SETUP_EXT_BUSES: //setup enable/listenonly/speed for SWCAN, Enable/Speed for LIN1, LIN2
      state = IDLE;
      break;
  }
}

void loop()
{
  CAN_FRAME incoming;
  int in_byte;

  if (CAN0.available() > 0) {
    CAN0.read(incoming);
    elmEmulator.processFrame(incoming);
#ifndef BLUETOOTH
    sendFrameToWiFi(incoming, 0);
#endif
  }

  while (Serial.available() > 0) {
    in_byte = Serial.read();
    console.rcvCharacter((uint8_t) in_byte);
  }

#ifndef BLUETOOTH
  bool needServerInit = false;

  if (!isWifiConnected)
  {
    if (WiFi.isConnected())
    {
      Serial.print("Wifi now connected to SSID ");
      Serial.println((const char *)settings.clientSSID);
      Serial.print("IP address: ");
      Serial.println(WiFi.localIP());
      Serial.print("RSSI: ");
      Serial.println(WiFi.RSSI());
      needServerInit = true;
    }
    if (settings.softAPMode)
    {
      Serial.print("Wifi setup as SSID ");
      Serial.println((const char *)settings.softSSID);
      Serial.print("IP address: ");
      Serial.println(WiFi.softAPIP());
      if (WiFi.softAPIP().toString() == String("192.168.4.1"))
      {
        Serial.println("Got wrong address. Forcing a reset to fix it.");
        WiFi.softAPdisconnect();
        ESP.restart();
      }
      needServerInit = true;
    }
    if (needServerInit)
    {
      isWifiConnected = true;
      wifiServer.begin();
      wifiServer.setNoDelay(true);

      savvyServer.begin();
      savvyServer.setNoDelay(true);

      ArduinoOTA.setPort(3232);
      ArduinoOTA.setHostname("MACCHINA_A0");
      // No authentication by default
      //ArduinoOTA.setPassword("admin");

      ArduinoOTA
      .onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH)
          type = "sketch";
        else // U_SPIFFS
          type = "filesystem";

        // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
        Serial.println("Start updating " + type);
      })
      .onEnd([]() {
        Serial.println("\nEnd");
      })
      .onProgress([](unsigned int progress, unsigned int total) {
        Serial.printf("Progress: %u%%\r\n", (progress / (total / 100)));
      })
      .onError([](ota_error_t error) {
        Serial.printf("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
        else if (error == OTA_END_ERROR) Serial.println("End Failed");
      });

      ArduinoOTA.begin();
      Serial.println("OBDII Server Started, OTA Server Started");
    }
  }
  else
  {
    if (wifiServer.hasClient())
    {
      for (i = 0; i < MAX_CLIENTS; i++)
      {
        if (!clientNodes[i])
        {
          clientNodes[i] = wifiServer.available();
          if (!clientNodes[i]) Serial.println("Couldn't accept client connection!");
          else
          {
            Serial.print("New client: ");
            Serial.print(i); Serial.print(' ');
            Serial.println(clientNodes[i].remoteIP());
            break;
          }
        }
      }
    }

    if (savvyServer.hasClient())
    {
      if (!savvyClient)
      {
        savvyClient = savvyServer.available();
        if (!savvyClient) Serial.println("Couldn't accept client savvycan connection!");
        else
        {
          Serial.print("New savvycan client: ");
          Serial.print(i); Serial.print(' ');
          Serial.println(savvyClient.remoteIP());
        }
      }
    }


    //check clients for data
    for (i = 0; i < MAX_CLIENTS; i++)
    {
      if (clientNodes[i])
      {
        if (clientNodes[i].connected())
        {
          //get data from the telnet client and push it to input processing
          while (clientNodes[i].available())
          {
            uint8_t inByt;
            inByt = clientNodes[i].read();
            //isWifiActive = true;
            //Serial.write(inByt); //echo to serial - just for debugging. Don't leave this on!
            //processIncomingByte(inByt);
            elmEmulator.processByte(inByt);
          }
        }
        else
        {
          Serial.print("Client ");
          Serial.print(i);
          Serial.println(" dropped.");
          clientNodes[i].stop();
          clientNodes[i] = 0;
        }
      }
    }

    if (savvyClient)
    {
      if (savvyClient.connected())
      {
        //get data from the telnet client and push it to input processing
        while (savvyClient.available())
        {
          uint8_t inByt;
          inByt = savvyClient.read();
          //isWifiActive = true;
          //Serial.write(inByt); //echo to serial - just for debugging. Don't leave this on!
          processIncomingByte(inByt);
          //elmEmulator.processByte(inByt);
        }
      }
      else
      {
        Serial.print("Savvy Client ");
        Serial.println(" dropped.");
        savvyClient.stop();
        savvyClient = 0;
      }
    }
  }

  if (isWifiConnected && ((micros() - lastBroadcast) > 1000000ul)) //every second send out a broadcast ping
  {
    uint8_t buff[4] = {0x1C, 0xEF, 0xAC, 0xED};
    lastBroadcast = micros();
    wifiUDPServer.beginPacket(broadcastAddr, 17222);
    wifiUDPServer.write(buff, 4);
    wifiUDPServer.endPacket();
  }

  //If the max time has passed or the buffer is almost filled then send buffered data out
  if ((micros() - lastFlushMicros > SER_BUFF_FLUSH_INTERVAL) || (serialBufferLength > (WIFI_BUFF_SIZE - 40)) ) {
    if (serialBufferLength > 0) {
      if (isWifiConnected)
      {
        if (savvyClient && savvyClient.connected())
        {
          savvyClient.write(serialBuffer, serialBufferLength);
        }
      }
      serialBufferLength = 0;
      lastFlushMicros = micros();
    }
  }

#endif
#ifndef BLUETOOTH
  ArduinoOTA.handle();
#else
  elmEmulator.loop();
#endif
}

#ifndef BLUETOOTH
// Utility to extract header value from headers
String getHeaderValue(String header, String headerName)
{
  return header.substring(strlen(headerName.c_str()));
}

void onOTAProgress(uint32_t progress, size_t fullSize)
{
  //esp_task_wdt_reset();
  // printf("%u of %u bytes written to memory...\n", progress, fullSize);
  if (OTAcount++ == 10)
  {
    printf("..%u\n", progress);
    OTAcount = 0;
  }
  else printf("..%u", progress);
}

void execOTA()
{
  //can't do this over softAP mode so try to connect to the set up AP if possible.
  if (settings.softAPMode)
  {
    Logger::console("Attempting to connect to an AP to get an internet connection.");
    WiFi.softAPdisconnect();
    WiFi.mode(WIFI_STA);
    WiFi.begin((const char *)settings.clientSSID, (const char *)settings.clientWPA2KEY);
    delay(10000);
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial << "\n\n\n******* WIFI STATUS..connected to local Access Point:" << WiFi.SSID() << " as IP:" << WiFi.localIP() << " signal strength:" << WiFi.RSSI() << "dBm *******\n\n";
    Serial << "We have a wireless connection...Initiating Wireless Over-the-Air Firmware Update...\n\n";
  }
  else
  {
    Serial << "\n\nYou must have a wireless connection to do OTA firmware update...\n\n";
    delay(4000);
    return;
  }

  int contentLength = 0;
  bool isValidContentType = false;
  String host = "www.macchina.cc"; // Host => bucket-name.s3.region.amazonaws.com
  int port = 80; // Non https. For HTTPS 443. As of today, HTTPS doesn't work.
  String bin = "/Macchina_A0_OBDII.ino.esp32.bin"; // bin file name with a slash in front.

  //esp_task_wdt_reset();        //Feed our watchdog
  Update.onProgress(onOTAProgress);

  Serial.println("Connecting to AWS server: " + String(host)); // Connect to S3

  if (wifiClient.connect(host.c_str(), port))
  {
    //Serial.println("Searching for: " + String(bin)); //Connection succeeded -fetching the binary
    wifiClient.print(String("GET ") + bin + " HTTP/1.1\r\n" +
                     "Host: " + host + "\r\n" +
                     "Cache-Control: no-cache\r\n" +
                     "Connection: close\r\n\r\n");  // Get the contents of the bin file

    unsigned long timeout = millis();
    while (wifiClient.available() == 0)
    {
      if (millis() - timeout > 5000)
      {
        Serial.println("Client Timeout !");
        wifiClient.stop();
        return;
      }
    }
    // Once the response is available,
    // check stuff

    /*
      Response Structure
      HTTP/1.1 200 OK
      x-amz-id-2: NVKxnU1aIQMmpGKhSwpCBh8y2JPbak18QLIfE+OiUDOos+7UftZKjtCFqrwsGOZRN5Zee0jpTd0=
      x-amz-request-id: 2D56B47560B764EC
      Date: Wed, 14 Jun 2017 03:33:59 GMT
      Last-Modified: Fri, 02 Jun 2017 14:50:11 GMT
      ETag: "d2afebbaaebc38cd669ce36727152af9"
      Accept-Ranges: bytes
      Content-Type: application/octet-stream
      Content-Length: 357280
      Server: AmazonS3

      {{BIN FILE CONTENTS}}

    */
    while (wifiClient.available())
    {
      String line = wifiClient.readStringUntil('\n');// read line till /n
      line.trim();// remove space, to check if the line is end of headers

      // if the the line is empty,this is end of headers break the while and feed the
      // remaining `client` to the Update.writeStream();

      if (!line.length()) {
        break;
      }


      // Check if the HTTP Response is 200 else break and Exit Update

      if (line.startsWith("HTTP/1.1"))
      {
        if (line.indexOf("200") < 0)
        {
          Serial.println("FAIL...Got a non 200 status code from server. Exiting OTA Update.");
          break;
        }
      }

      // extract headers here
      // Start with content length

      if (line.startsWith("Content-Length: "))
      {
        contentLength = atoi((getHeaderValue(line, "Content-Length: ")).c_str());
        Serial.println("              ...Server indicates " + String(contentLength) + " byte file size\n");
      }

      if (line.startsWith("Content-Type: "))
      {
        String contentType = getHeaderValue(line, "Content-Type: ");
        Serial.println("\n              ...Server indicates correct " + contentType + " payload.\n");
        if (contentType == "application/octet-stream")
        {
          isValidContentType = true;
        }
      }
    } //end while client available
  }
  else
  {
    // Connect to S3 failed
    Serial.println("Connection to " + String(host) + " failed. Please check your setup");
  }

  // Check what is the contentLength and if content type is `application/octet-stream`
  //Serial.println("File length: " + String(contentLength) + ", Valid Content Type flag:" + String(isValidContentType));

  // check contentLength and content type
  if (contentLength && isValidContentType) // Check if there is enough to OTA Update
  {
    bool canBegin = Update.begin(contentLength);
    if (canBegin)
    {
      Serial << "We have space for the update...starting transfer... \n\n";
      // delay(1100);
      size_t written = Update.writeStream(wifiClient);

      if (written == contentLength)
      {
        Serial.println("\nWrote " + String(written) + " bytes to memory...");
      }
      else
      {
        Serial.println("\n********** FAILED - Wrote:" + String(written) + " of " + String(contentLength) + ". We'll try again...********\n\n" );
        execOTA();
      }

      if (Update.end())
      {
        //  Serial.println("OTA file transfer completed!");
        if (Update.isFinished())
        {
          Serial.println("Rebooting new firmware...\n");
          WiFi.disconnect();
          delay(1000);
          ESP.restart();
        }
        else Serial.println("FAILED...update not finished? Something went wrong!");

      }
      else
      {
        Serial.println("Error Occurred. Error #: " + String(Update.getError()));
        execOTA();
      }
    } //end if can begin
    else {
      // not enough space to begin OTA
      // Understand the partitions and space availability

      Serial.println("Not enough space to begin OTA");
      wifiClient.flush();
    }
  } //End contentLength && isValidContentType
  else
  {
    Serial.println("There was no content in the response");
    //wifiClient.flush();
  }
  Serial.println("End of OTA firmware update routine. Apparently it failed. Check above for reasons.");
  //if we were in softAP mode then re-enter it again before leaving
  if (settings.softAPMode)
  {
    Logger::console("Returing to softAP mode");
    WiFi.disconnect();
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192, 168, 0, 10), IPAddress(192, 168, 0, 10), IPAddress(255, 255, 255, 0));
    WiFi.softAP((const char *)settings.softSSID, (const char *)settings.softWPA2KEY);
    isWifiConnected = false; //probably have to do this to make it re-setup the sockets
  }
} //End execOTA()
#endif
