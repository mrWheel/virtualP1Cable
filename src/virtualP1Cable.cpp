/*
* virual P1 Cable
* copyright 2023 by Willem Aandewiel
* 
* Library: TMRh20/RF24, https://github.com/tmrh20/RF24/
* --> by Dejan Nedelkovski, www.HowToMechatronics.com
*/
/*----------------------------------------------------------------------------
*  Chip: "AVR128DB32" / "AVR128DB28"
*  Clock Speed: "24 MHz internal"
*  millis()/micros() timer: |TCB2 (recommended)"
*  Reset pin function: "Hardware Reset (recommended)"
*  Startup Time: "8ms"
*  MultiVoltage I/O (MVIO): "Disabled"
*  --
*  Programmer: "jtag2updi" 
*-----------------------------------------------------------------------------
* >>>>>>>>>>>>>>> Don't forget to set fuses in "Project Tasks" <<<<<<<<<<<<<<<
* (will execute "/<home>/.platformio/platforms/atmelmegaavr/builder/fuses.py")
*---------------------------------------------------------------------------*/

#define _FW_VERSION "1.1 (14-08-2023)"

/*
 * PINS NAME     AVR128DB32        AVR128DB28
 * TXD0           PIN_PA0           PIN_PA0
 * RXD0           PIN_PA1           PIN_PA1
 * MOSI           PIN_PA4           PIN_PA4
 * MISO           PIN_PA5           PIN_PA5
 * SCK            PIN_PA6           PIN_PA6
 * PIN_LED        PIN_PA7           PIN_PA7
 * TXD1           PIN_PC0           PIN_PC0
 * RXD1           PIN_PC1           PIN_PC1
 * DSMR_VRS       -                 PIN_PC3
 * PIN_MODE       PIN_PF0           PIN_PF0
 * RESET PIN      PF6               PF6      (set fuses!)
 * PIN_CE         PIN_PF1           PIN_PD1
 * PIN_CSN        PIN_PF2           PIN_PD2
 * SWITCH4        -                 PIN_PD3
 * SWITCH1        -                 PIN_PD4
 * SWITCH2        -                 PIN_PD5
 * SWITCH3        -                 PIN_PD6
*/
#if defined( __AVR_AVR128DB32__ )
  #define PIN_CE      PIN_PF1
  #define PIN_CSN     PIN_PF2
#elif defined( __AVR_AVR128DB28__ )
  #define PIN_CE      PIN_PD1
  #define PIN_CSN     PIN_PD2
#else
  #error No Processor selected!
#endif
#define PIN_LED     PIN_PA7
#define DSMR_VRS    PIN_PC3
#define PIN_MODE    PIN_PF0
#define SWITCH1     PIN_PD4
#define SWITCH2     PIN_PD5
#define SWITCH3     PIN_PD6
#define SWITCH4     PIN_PD3

#define SET_BIT(x, pos) (x |= (1U << pos))
#define CLR_BIT(x, pos) (x &= (~(1U<< pos)))

#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>
#include "CRC16.h"

/*** for testing ***
// "/KFM5KAIFA-METER\r\n\r\n1-0:1.8.1(000671.578*kWh)\r\n1-0:1.7.0(00.318*kW)\r\n!1E1D\r\n\r\n";
char minTelegram[] =
  "/KFM5KAIFA-METER\r\n"              //-- 18
  "\r\n"                              //--  2
  "1-0:1.8.1(000671.578*kWh)\r\n"     //-- 27
  "1-0:1.7.0(00.318*kW)\r\n"          //-- 22
  //----------------------------------//---> 69
  "!"                                 //---> 70
  "1E1D\r\n\r\n";
***/

#define _MAX_P1BUFFER       10000
#define _DATA_SIZE             29
#define _MAX_TRANSMIT_ERRORS   50
#define _SEND_INTERVAL        500 //-- milli seconds
#define _MAX_LAST_TIME      30000 //-- milli seconds
#define _MAX_TIMEOUT         2000 //-- milli secondes
#define _REBOOT_TIME      3000000 //-- every fifty minutes

struct payLoadStruct
{
   uint8_t sequenceNr;
   char    data[_DATA_SIZE];
} payLoad;

RF24 radio(PIN_CE, PIN_CSN); // CE, CSN

byte      pipeName[6] = {};

bool      isReceiver;
bool      isDSMR_4Plus;
byte      channelNr = 0x61;
int       powerMode = RF24_PA_MIN;
char      p1Buffer[_MAX_P1BUFFER+15];  // room for "!1234\0\r\n\r\n"
int16_t   p1BuffLen;
int16_t   p1BuffPos = 0;
bool      startTelegram;
int16_t   startTelegramPos;
int16_t   avrgTelegramlen = 500;
int16_t   payLoadSize = sizeof(payLoad);
uint32_t  sendTimer, startTime, waitTimer, rebootTimer;
uint32_t  errCount;
char      orgCrcChar[10];
int16_t   orgCrcLen;


//--------------------------------------------------------------
void blinkLed(int nrTimes, int waitMs)
{
  for (int i=0; i<nrTimes; i++)
  {
    digitalWrite(PIN_LED, CHANGE);
    delay(waitMs);
  }
  digitalWrite(PIN_LED, LOW);

} //  blinkLed()

//--------------------------------------------------------------
void resetViaSWR() 
{
  Serial.println("\r\nSoftware reset ...\r\n");
  Serial.flush();
  blinkLed(10, 30);
  delay(500);
  _PROTECTED_WRITE(RSTCTRL.SWRR,1);

} //  resetViaSWR()


//--------------------------------------------------------------
bool readSettings() 
{
  Serial.println("Read the Switches ...");
  //---- read PIN_MODE, DSMR_VRS, SWITCH4 and SWITCH1/2/3
  //---- en doe daar wat mee
  isReceiver = digitalRead(PIN_MODE);

  isDSMR_4Plus = digitalRead(DSMR_VRS);

  if (!digitalRead(SWITCH4))
  {
    powerMode = RF24_PA_MAX;
    Serial.println("  * RF24 Power set to MAX");
  }
  else  
  {
    powerMode = RF24_PA_LOW;
    Serial.println("  * RF24 Power set to LOW");
  }
  //x[0] = digitalRead(DSMR_VRS);
  /*
  **             |     SWITCH      |               ||
  **  switch |   | [1] | [2] | [3] |               || 
  **     bit | 7 |  6  |  5  |  4  | 3 | 2 | 1 | 0 || CHANNEL
  **         +---+-----+-----+-----+---+---+---+---++--------  
  **         | 0 | Off | Off | Off | 0 | 1 | 1 | 1 ||   12
  **         | 0 | Off | Off | On  | 0 | 1 | 1 | 1 ||   28
  **         | 0 | Off | On  | Off | 0 | 1 | 1 | 1 ||   44
  **         | 0 | Off | On  | On  | 0 | 1 | 1 | 1 ||   60
  **         | 0 | On  | Off | Off | 0 | 1 | 1 | 1 ||   76
  **         | 0 | On  | Off | On  | 0 | 1 | 1 | 1 ||   92
  **         | 0 | On  | On  | Off | 0 | 1 | 1 | 1 ||  108
  **         | 0 | On  | On  | On  | 0 | 1 | 1 | 1 ||  124
  **
  */
  SET_BIT(channelNr,0);
  SET_BIT(channelNr,1);
  SET_BIT(channelNr,2);
  CLR_BIT(channelNr,3);
  if (!digitalRead(SWITCH1))
        SET_BIT(channelNr,4);
  else  CLR_BIT(channelNr,4);
  if (!digitalRead(SWITCH2))
        SET_BIT(channelNr,5);
  else  CLR_BIT(channelNr,5);
  if (!digitalRead(SWITCH3))
        SET_BIT(channelNr,6);
  else  CLR_BIT(channelNr,6);
  CLR_BIT(channelNr,7);
  channelNr += 0x05;

} //  readSettings()


//--------------------------------------------------------------
void transmitTelegram(char *telegram, int telegramLen) 
{
  bool transmitOk = false;

  int16_t sequenceNr = 0;
  errCount         = 0;
  startTelegramPos = 0;

  sequenceNr = telegramLen / sizeof(payLoad.data);
  Serial.printf("\r\nTransmit Telegram ..(%d bytes) in [%d+1] packages\r\n", telegramLen, sequenceNr);
  sequenceNr = 0;
  digitalWrite(PIN_LED, HIGH);

  uint16_t calcCrc=CRC16(0x0000, (unsigned char*)telegram, telegramLen);

  startTime = millis();

  while(startTelegramPos<telegramLen)
  {
    memset(&payLoad, 0, payLoadSize);
    payLoad.sequenceNr = sequenceNr++;
    for(int i=0; i<_DATA_SIZE; i++)
    {
      payLoad.data[i] = telegram[i+startTelegramPos];
    }
    //---- ending "0" activates autoACK & Retry
    errCount = 0;
    do
    {
      Serial.printf("%d ", payLoad.sequenceNr);
      transmitOk = radio.write(&payLoad, payLoadSize, 0);
      if (!transmitOk) { delay(10); errCount++; }
      else 
      { 
        if (startTelegramPos==0) 
        { delay(50); }  //-- give receiver time to check start
        else          
        { delay(5); }
      }
    }
    while (!transmitOk && (errCount < _MAX_TRANSMIT_ERRORS));

    if (!transmitOk) 
    {
      Serial.println("\r\nMax Transmit errors.. Bailout!");
      blinkLed(15, 70);
      return;
    }
    startTelegramPos += _DATA_SIZE;
    delay(10);
  }

  //-- send End Of Telegram token + CRC16 checksum
  memset(&payLoad, 0, payLoadSize);
  payLoad.sequenceNr = sequenceNr++;
  snprintf(payLoad.data, _DATA_SIZE, "*EOT* %04x", calcCrc);
  //Serial.printf("EOT Send [%s]\r\n", &payLoadBuff);

  //---- ending "0" activates autoACK & Retry
  errCount = 0;
  do
  {
    transmitOk = radio.write(&payLoad, payLoadSize, 0);
    Serial.printf("\r\n[%2d] EOT Send [%s]\r\n", payLoad.sequenceNr, &payLoad.data);
    if (!transmitOk)  { delay(10); errCount++; }
    else              { delay(5); }
  }
  while (!transmitOk && (errCount < _MAX_TRANSMIT_ERRORS));

  Serial.printf("Telegram transmitted in %ld milliseconds!\r\n", (millis()-startTime));

} //  transmitTelegram()


//--------------------------------------------------------------
void setupReceiver()
{
  Serial.printf("I'm a RECEIVER using pipe [%s]...\r\n\n", pipeName);

  radio.openReadingPipe(0, pipeName);
  Serial.println("startListening ...");
  radio.startListening();

  rebootTimer = millis() + _REBOOT_TIME;

  Serial.println("Done with setupReceiver()");
  Serial.flush();
  delay(1000);

} //  setupReceiver


//--------------------------------------------------------------
void loopReceiver()
{
  uint8_t sequenceNr   = 0;
  uint8_t dataRead    = 0;
  bool foundSlash     = false;
  bool foundEOT       = false;
  char calcCrcChar[5] = {};
  p1BuffLen           = 0;
  p1BuffPos           = 0;

  memset(p1Buffer, 0, _MAX_P1BUFFER);

  if (radio.available()) 
  {
    memset(&payLoad, 0, payLoadSize);
    uint8_t bytesRead = radio.getDynamicPayloadSize();
    //Serial.printf("read [%d]bytes -> ", bytesRead);
    radio.read(&payLoad, bytesRead);
    dataRead  = bytesRead - sizeof(uint8_t);
    foundSlash = false;
    //-- sequenceNr '0' is start Telegram
    if (payLoad.sequenceNr == 0)
    {
      Serial.println("\r\nFound sequenceNr [0] (start new Telegram)");
      //Serial.printf("dataLen [%d]bytes ", dataRead);
      sequenceNr   = 0;
      foundSlash  = true;
      digitalWrite(PIN_LED, HIGH); 
      memset(p1Buffer, 0, _MAX_P1BUFFER+10);
      p1BuffLen = 0;
      p1BuffPos = 0;
      memcpy(&p1Buffer[p1BuffPos], payLoad.data, dataRead);
      p1BuffPos += dataRead;
      startTelegramPos = 0;
      startTime = millis();
    }
    
    if (!foundSlash) 
    {
      Serial.printf("%d ", payLoad.sequenceNr);
      blinkLed(1, 10);
      return;
    }
    foundEOT = false;
    while(!foundEOT && (p1BuffPos < (_MAX_P1BUFFER -15)) && (millis() < rebootTimer) )
    {
      memset(&payLoad, 0, payLoadSize);
      if (radio.available())
      {
        uint8_t bytesRead = radio.getDynamicPayloadSize();
        radio.read(&payLoad, bytesRead);
        dataRead = bytesRead - sizeof(uint8_t);
        if ( ((payLoad.sequenceNr % 10) == 0) || (payLoad.sequenceNr == 1) )
              Serial.print(payLoad.sequenceNr);
        else  Serial.print('.');
        //-- look for *EOT* in payLoad.data ..
        if (strncmp(payLoad.data, "*EOT*", 5) == 0)
        {
          Serial.printf("\r\nFound '*EOT*' token [%s] in sequenceNr[%d]\r\n", payLoad.data, payLoad.sequenceNr);
          //-- copy CRC16 checksum ..
          for (int i=0; i<5; i++) 
          {
            orgCrcChar[i] = payLoad.data[6+i];
          }
          //-- now calculate CRC16 checksum for complete Telegram
          uint16_t calcCrc=CRC16(0x0000, (unsigned char*)p1Buffer, strlen(p1Buffer));
          snprintf(calcCrcChar, 5, "%04x", calcCrc);
          Serial.printf("  Received CRC[%s]\r\n", orgCrcChar);
          Serial.printf("Calculated CRC[%s] ", calcCrcChar);
          //-- are the CRC16 the same ??
          if (strncmp(calcCrcChar, orgCrcChar, 4 ) == 0)
          {
            Serial.println(" -> Match!");
          }
          else
          {
            //-- No! exit
            Serial.println(" -> CRC error! Bailout!");
            blinkLed(5, 50);
            return;
          }
          foundEOT = true;
          digitalWrite(PIN_LED, LOW); 
        }
        else
        {
          //-- is this the next payLoad in sequence?
          if (payLoad.sequenceNr == (sequenceNr+1))
          {
            //-- Yes! Copy data to p1Buffer
            memcpy(&p1Buffer[p1BuffPos], payLoad.data, dataRead);
            p1BuffPos += dataRead;
          }
          else
          {
            //-- if greater or smaller that's an error
            if ( (payLoad.sequenceNr > sequenceNr) || (payLoad.sequenceNr < sequenceNr) )
            {
              Serial.println(" -> sequence error!");
              blinkLed(5, 60);
              return;
            }
          }
          sequenceNr = payLoad.sequenceNr;
        }
      }
    
    } //  while !*EOT*

    //-- received all data packages, so now process Telegram
    Serial.println();
    int telegramLen = strlen(p1Buffer)-startTelegramPos; 
    int telegramEnd = 0;
    Serial.printf("+++++Telegram is [%d]bytes++++++++++++++++++++\r\n", telegramLen);

    uint32_t receiveTime = (millis() - startTime);
    Serial.print(&p1Buffer[startTelegramPos]);
    Serial.printf("Telegram received in %ld milliseconds!\r\n",receiveTime);
    Serial.print("Send Telegram to P1-OUT .. >");
    //-- send it 3 times, just to be sure!
    for(int r=0; r<3; r++)
    {
      Serial.printf("%d>", r);
      Serial1.print(&p1Buffer[startTelegramPos]);
      delay((r*30));
    }
    Serial.println();
    //-- reset rebootTimer --
    rebootTimer = millis() + _REBOOT_TIME;
  }

  //-- if it takes too long to receive a Telegram, reboot Receiver 
  if ( millis() >  rebootTimer)  { resetViaSWR(); }


} //  loopReceiver


//--------------------------------------------------------------
void setupTransmitter()
{
  memset(p1Buffer, 0, sizeof(p1Buffer));

  Serial.printf("I'm a TRANSMITTER using pipe [%s]...\r\n\n", pipeName);
  Serial1.setTimeout(10);

  radio.openWritingPipe(pipeName);
  Serial.println("stopListening ...");
  radio.stopListening();

} //  setupTransmitter


//--------------------------------------------------------------
void loopTransmitter()
{
  bool transmitOk = false;

  if (!Serial1.available()) { return; }

  memset(orgCrcChar,   0, sizeof(orgCrcChar));

  memset(p1Buffer, 0, _MAX_P1BUFFER+10);
  p1BuffLen = 0;
  p1BuffPos = 0;

  //-- set timeout to 15 seconds ....
  waitTimer = millis();
  //-- read until "!" ("!" is not in p1Buffer!!!) --
  int len = Serial1.readBytesUntil('!', p1Buffer, (_MAX_P1BUFFER -15));

  //-- average length of Telegram does not change to mutch, so if
  //-- not in range it's a partial Telegram that we cannot process
  if (len < (avrgTelegramlen/2) ) { return; }

  Serial.printf("\r\nRead [%d]bytes\r\n", len);

  if (len >= (_MAX_P1BUFFER - 15))
  {
    Serial.println("Something went wrong .. (no '!' found in input) .. Bailout!");
    Serial.flush();
    blinkLed(10, 100);
    waitTimer = millis();
    return;
  }

  //-- if timer has expired ..
  if ((millis() - waitTimer) >= _MAX_TIMEOUT)  
  {
    Serial.println("Max waittime expired! BialOut");
    Serial.flush();
    blinkLed(20, 50);
    waitTimer = millis();
    return;
  }
  //          1       2       3                                        len
  //  1...5...0...5...0...5...0                       ..................| 
  //                                                                    v
  // "/KFM5KAIFA-METER\r\n\r\n1-0:1.8.1(0006 . . . 0:1.7.0(00.318*kW)\r\n!1E1D\r\n\r\n";
  //
  //-- track back to first non-control char
  for (int p=0; p>-10; p--)
  {
    //-- test if not a control char
    if ((p1Buffer[len-p] >= ' ') && (p1Buffer[len-p] <= '~'))
    {
      len-=p;
      Serial.println();
      break;
    }
  }
  Serial.println();
  //                                                               len
  //  1...5...1...5...2...5...3                       ..............| 
  //                                                                v
  // "/KFM5KAIFA-METER\r\n\r\n1-0:1.8.1(0006 . . . 0:1.7.0(00.318*kW)\r\n!1E1D\r\n\r\n";
  //
  p1Buffer[len++] = '!';

  int orgCrcPos = 0;
  memset(orgCrcChar, 0, sizeof(orgCrcChar));

  Serial1.setTimeout(500);
  int orgCrcLen = Serial1.readBytesUntil('\r', orgCrcChar, sizeof(orgCrcChar));
  Serial.printf("(a) p1Buffer [%d]bytes, orgCrcLen is [%d]\r\n", len, orgCrcLen);
  for(int i=0; i<strlen(orgCrcChar); i++)
  {
    if ((orgCrcChar[i] >= ' ') && (orgCrcChar[i] <= '~'))
    {
      p1Buffer[len++] = orgCrcChar[i];
    }
  }
  Serial.printf("(b) p1Buffer [%d]bytes\r\n", len);

  p1Buffer[len++] = '\r';
  p1Buffer[len++] = '\n';
  p1Buffer[len++] = '\r';
  p1Buffer[len++] = '\n';
  p1Buffer[len++] = '\0';

  //-- Telegram length does not vary much so it is
  //-- a good idea to test to see if it's in range ...
  if (len < avrgTelegramlen) 
  {
    Serial.printf("Telegram too short ([%d]bytes); must be at least [%d]bytes... Bailout\r\n", len, avrgTelegramlen);
    if (avrgTelegramlen > 60) avrgTelegramlen -= 10;
    blinkLed(5, 100);
    return;
  }

  startTelegram = false;
  startTelegramPos = -1;
  p1BuffLen = strlen(p1Buffer);
  avrgTelegramlen = p1BuffLen * 0.8;
  Serial.printf("Find last [/] .. in [%d]bytes\r\n", p1BuffLen);
  //-- loopback over all chars in Telegram and try to find a '/' character
  for(int i=p1BuffLen; (i>=0 && !startTelegram); i--)
  {
    if (p1Buffer[i] == '/')
    {
      startTelegram = true;
      startTelegramPos = i;
      Serial.printf("\r\nStart Telegram [/] found at pos[%d] ...\r\n\n", startTelegramPos);
      break;
    }
  }
  if (!startTelegram)
  {
    Serial.println("Not a valid start [/] found.. Bailout\r\n");
    blinkLed(10, 50);
    return;
  }

  Serial.println("-Telegram-----------------------------------------------------");
  Serial.print(&p1Buffer[startTelegramPos]);

  p1BuffLen = strlen(p1Buffer) - startTelegramPos;
  Serial.printf("p1Telegram is [%d]bytes long\r\n", p1BuffLen);
  Serial.flush();

  //-- transmit Telegram in packages .. 
  transmitTelegram(&p1Buffer[startTelegramPos], p1BuffLen);

  digitalWrite(PIN_LED, LOW);

} //  loopTransmitter


//==============================================================
void setup() 
{
  pinMode(PIN_LED,  OUTPUT);
  //--------
  pinMode(SWITCH4,  INPUT_PULLUP);  //-- power Max (LOW) or LOW (HIGH)
  pinMode(SWITCH1,  INPUT_PULLUP);
  pinMode(SWITCH2,  INPUT_PULLUP);
  pinMode(SWITCH3,  INPUT_PULLUP);
  //--------
  pinMode(DSMR_VRS, INPUT_PULLUP);  //-- Pré 4 (LOW) or 4+ (HIGH)
  pinMode(PIN_MODE, INPUT_PULLUP);  //-- Transmitter (LOW) or Receiver (HIGH)

  blinkLed(10, 200);
  
  Serial.begin(115200);
  while(!Serial) { delay(10); }
  Serial.flush();
  Serial.println("\n\n\nAnd then it begins ....");
  Serial.printf("Firmware version v%s\r\n", _FW_VERSION);

  readSettings();
  //-- P1 data type
  if (isDSMR_4Plus)
  {
    Serial.println("  * P1 port set to 115200bps, 8N1");
    Serial.println("  * Set for DSMR version 4.n or 5.n.");
    Serial1.begin(115200, SERIAL_8N1);
  }
  else
  {
    Serial.println("  * P1 port set to 9600bps, 7E1");
    Serial.println("  * Set for DSMR version 2.n or 3.n");
    Serial1.begin(9600, SERIAL_7E1);
  }
  while(!Serial1) { delay(10); }

  char tmpName[7];
  //-- create a pipe name derived from channelNr
  snprintf(tmpName, 6, "P1-%02x", channelNr);
  for(int n=0; n<6; n++)  pipeName[n] = tmpName[n];
  
  //--- now setup radio's ----
  if (!radio.begin())
  {
    Serial.println("\r\nNo NRF24L01 transceiver found!\r");
    while(true)
    {
      digitalWrite(PIN_LED, CHANGE);
      delay(100);
    }
  }
  //-- Set Transmitter Power 
  radio.setPALevel( powerMode );
  //-- Min speed (for better range I presume)
  radio.setDataRate( RF24_250KBPS ) ; 
  //-- 8 bits CRC
  radio.setCRCLength( RF24_CRC_8 ) ; 
  //-- Enable dynamic payloads 
  radio.enableDynamicPayloads();
  //-- save on transmission time by setting the radio to only transmit the
  //-- number of bytes we need to transmit
  radio.setPayloadSize(_DATA_SIZE);  
  //-- increase the delay between retries and # of retries 
  radio.setRetries(10,10);  //-- 15 - 15 is the max!

  //-- set Channel
  Serial.printf("  * Set RF24 channel to [0x%x]/[%d]\r\n", channelNr, channelNr);
  radio.setChannel(channelNr);

  Serial.printf("Set payLoad to [%d]bytes with [%d]bytes data\r\n", payLoadSize, (payLoadSize - sizeof(uint8_t)));

  if (isReceiver)
  {
    Serial.println("I'm a Receiver!");
    setupReceiver();
  }
  else  
  {
    Serial.println("I'm a Transmitter!");
    setupTransmitter();
  }
  
} //  setup()


//==============================================================
void loop() 
{
  if (isReceiver) loopReceiver();
  else            loopTransmitter();

} //  loop()
