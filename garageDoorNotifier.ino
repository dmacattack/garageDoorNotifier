//---------------------------------------------------------------
// includes
//---------------------------------------------------------------
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <BlynkSimpleEsp8266.h>
#include "WifiCredentials.h"

//---------------------------------------------------------------
// macros
//---------------------------------------------------------------
// Comment this out to disable prints and save space 
// TODO this was moved from above the includes. that doesnt make sense to me.does it matter?
//#define BLYNK_PRINT Serial   
#define DBG_ENABLED 1
#if DBG_ENABLED
   #define DBG_PRINT(fn, msg) debug_print(fn, msg)
#else
  #define DBG_PRINT(fn, msg) 0; // do nothing   
#endif

//---------------------------------------------------------------
// Types
//---------------------------------------------------------------
typedef unsigned int   UInt32; 
typedef unsigned short UInt16;
typedef unsigned char  UInt8;
typedef int Int32;
typedef short Int16;
typedef char Int8;

// LED state machine 
// TODO change name to pin/garage door state machine
enum eLEDSTATUS
{
  eUNINIT,
  eON,
  eWAIT_ON,
  eOFF
};   

enum eDOORSTATUS
{
  eDOOR_UNKNOWN,
  eDOOR_OPEN,
  eDOOR_CLOSED
};

//---------------------------------------------------------------
// constants
//---------------------------------------------------------------
String mVersionNumber        = "-------VERSION 0.5-------";
const Int32 LED_PIN          = 5;    // ESP8266 onboard, green LED
const Int32 TOGGLE_DURATION  = 1000; // full second
const Int32 US_PIN           = 15; // ultrasonic sensor Pulse Width pin
const Int32 SENREAD_INTERVAL = 5000 ; // five seconds in ms.
const Int32 SENREAD_ERR      = 100;  // 2% error in case other processes take too long
const Int32 PULSE_TIMEOUT    = 100000; //100ms timeout // TODO decrease?
const Int32 SEN_THRESH_ERR   = 580; // 10cm (580us /29/2 = 10cm)
const Int32 EEPROM_ADDR      = 0;   // address where to save threshold

//---------------------------------------------------------------
// member variables
//---------------------------------------------------------------                                         
// used for different data to the email/callback
Int32 mButtonPressCnt       = 0; 
// used for the current LED status (toggling the pin)
eLEDSTATUS mStatus         = eUNINIT;
// used for counting how long the pin has been pressed
UInt32 mTargetTime = 0;
// used for counting how long we should run until checking the Ultrasonic sensor
UInt32 mReadSenCnt  = 0;
// used for the Ultrasonic sensor read value
UInt32 mSenVal      = 0;
// threshold to trigger when the door is open (0 is unset)
UInt32 mSenThreshold   = 0; 
// state of the garage door
eDOORSTATUS mDoorStatus = eDOOR_UNKNOWN;
// cheater way for hysteresis
bool doorStatusHyst = false;
// terminal for status updates
WidgetTerminal mTerminal(V10);

//---------------------------------------------------------------
// setup
//
// setup the wifi, pins, and assign interupts
//---------------------------------------------------------------
void setup()
{
  #if DBG_ENABLED
  Serial.begin(9600);
  #endif

  // print the version number
  DBG_PRINT("", ""); // intentional blank line
  DBG_PRINT("", "--------------------------");
  DBG_PRINT("", mVersionNumber );
  DBG_PRINT("", "--------------------------");
  
  // connect to wifi
  DBG_PRINT("setup", " connect to wifi ");
  Blynk.begin(AUTH, WIFI_SSID, WIFI_PASS);
  DBG_PRINT("setup", "connection success");

  // read in the persisted threshold from EEPROM
  mSenThreshold = readEE(EEPROM_ADDR);
  DBG_PRINT("setup", String("EEPROM threshold = " + String(microsecondsToCentimeters(mSenThreshold)) + "cm" ) );

  mTerminal.println( mVersionNumber );
  mTerminal.println("--------------------------");
  mTerminal.println("Door threshold = " + String(microsecondsToCentimeters(mSenThreshold)) + " cm");
  mTerminal.flush();
  
  // start the sensor as output
  pinMode(US_PIN, OUTPUT);
  digitalWrite(US_PIN, LOW); 
  // setup the LED/garage door pin
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  // attach interrrupt to the button
}

//---------------------------------------------------------------
// loop
// keep the blynk running
// update the pin state machine
// read the sensor
//---------------------------------------------------------------
void loop()
{
  Blynk.run(); 
  pinOutStateMachine();  

#if 0
  if ((millis() % SENREAD_INTERVAL) < SENREAD_ERR) // once every 5 secs + 100 ms threshold
  {
    readSen();
  }
  else
  {
    // keep waiting
  }
#else

  #define THREE_MINS 0x59ff4c
  #define TWENTY_SECS 0x9FFF6
  #define TWO_SECS 0xFFFF
  
  if (mReadSenCnt >= TWENTY_SECS ) 
  {
    mReadSenCnt = 0;
    readSen();
  }
  else
  {
    mReadSenCnt++;
  }
  
 #endif
}

//---------------------------------------------------------------
// BLYNK_WRITE(VirtualPin #) - openGarageDoor
//
// Virtual Pin (button) on the blynk app triggers this event
// This initializes the output pin to toggle 
// (press the garage door button)
//---------------------------------------------------------------
BLYNK_WRITE(V1)
{
  String paramStr = param.asStr();
  String msg =  "Got a value: " + paramStr;
  DBG_PRINT("BLYNK_WRITE - V1", msg);

  if (param.asInt() == 0)
  {
    DBG_PRINT("BLYNK_WRITE - V1", "toggle the output");
    mStatus = eON;
  }
  else
  {
    // do nothing, because active LOW
  }
}

//---------------------------------------------------------------
// BLYNK_WRITE(VirtualPin #) - setThreshold
//
// Virtual Pin (button) on the blynk app triggers this event
// This sets the opened door threshold 
//---------------------------------------------------------------
BLYNK_WRITE(V2)
{
  String paramStr = param.asStr();
  String msg =  "Got a value: " + paramStr;
  DBG_PRINT("BLYNK_WRITE - V2", msg);

  if (param.asInt() == 0)
  {
    mSenThreshold = mSenVal + SEN_THRESH_ERR;
    
    writeEE(EEPROM_ADDR, mSenThreshold);
    
    String thresh = "set the sensor output as the open Threshold as < " +
    String(microsecondsToCentimeters(mSenThreshold)) + " cm " ;
    
    DBG_PRINT("BLYNK_WRITE - V2", thresh);
    mTerminal.println(":: Set threshold to " + String(microsecondsToCentimeters(mSenThreshold)) + " cm ") ;
    mTerminal.flush();
  }
  else
  {
    // do nothing, because active LOW
  }
}

//---------------------------------------------------------------
// BLYNK_WRITE(VirtualPin #) - getInfo
//
// Virtual Pin (button) on the blynk app triggers this event
// This pushes a status to the app
//---------------------------------------------------------------
BLYNK_WRITE(V3)
{
  String paramStr = param.asStr();
  String msg =  "Got a value: " + paramStr;
  DBG_PRINT("BLYNK_WRITE - V3", msg);

  if (param.asInt() == 0)
  {
    String info = "Sen: " 
    + String(microsecondsToCentimeters(mSenVal))
    + "/" 
    + String(microsecondsToCentimeters(mSenThreshold)) 
    + " cm. Door is " + 
    getString(mDoorStatus);
        
    DBG_PRINT("BLYNK_WRITE - V3", info);
    mTerminal.println(":: " + info);
    mTerminal.flush();
  }
  else
  {
    // do nothing, because active LOW
  }
}

//---------------------------------------------------------------
// pinOutStateMachine
//
// simple state machine to toggle the output pin without 
// interrupting the code too much
//---------------------------------------------------------------
void pinOutStateMachine()
{
  switch (mStatus)
  {
    case eON:
    {
      mTargetTime = millis() + TOGGLE_DURATION; 
      digitalWrite(LED_PIN, HIGH);
      mTerminal.println(">> Pressing Garage Door Button ");
      mTerminal.flush();
      mStatus = eWAIT_ON;
      break;  
    }
    case eWAIT_ON:
    {     
      if ( millis() > mTargetTime)
      {
        mStatus = eOFF;
      }
      else
      {
        // keep waiting
      }
      break;  
    }
    case eOFF:
    {
      digitalWrite(LED_PIN, LOW);
      mTerminal.println(">> Release Garage Door Button");
      mTerminal.flush();
      mStatus = eUNINIT;
      break;
    }
    case eUNINIT:
    // intentionally do nothing
    default:
    // do nothing
    break;
  }
}

//---------------------------------------------------------------
// readSen
//
// read the ultraSonicSensor in cm
//---------------------------------------------------------------
void readSen()
{
  // reset it // TODO why is this needed? 
  mSenVal = 0;

  // low for 2us, high for 5us, back to low
  pinMode(US_PIN, OUTPUT);
  digitalWrite(US_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(US_PIN, HIGH);
  delayMicroseconds(4); // its supposed to be 5, but the scoope syas 5 --> 6.6 us, and 4--> 5.2us
  digitalWrite(US_PIN, LOW);

  // the same pin is used for feedback
  pinMode(US_PIN, INPUT);
  mSenVal = pulseIn(US_PIN, HIGH, PULSE_TIMEOUT); 

  if (mSenVal == 0)
  {
    String str = String(millis()/1000) + ") timeout " + String(microsecondsToCentimeters(mSenVal)) + " cm" ;
    DBG_PRINT("readSen", str);
    mTerminal.println(">> " + str );
  }
  else
  {
    String str = String(millis()/1000) + ") sensor = " + String(microsecondsToCentimeters(mSenVal)) + " cm";
    DBG_PRINT("readSen" , str );
    mTerminal.println(">> " + str );
    eDOORSTATUS doorStatus = eDOOR_UNKNOWN;
    
    // check if the door is closed
    if (mSenVal < mSenThreshold )
    {
      doorStatus = eDOOR_OPEN;
    }
    else
    {
      // nothing to see here. continue
      doorStatus = eDOOR_CLOSED;
    }

    if (doorStatus != mDoorStatus) 
    {
        String msg = String("!!! " + getString(mDoorStatus) + " ---> " + getString(doorStatus) + " !!!" );      
        DBG_PRINT("readSen", msg );
      
      if (doorStatusHyst == true)
      {
        // send event to the app only if the status changed
        DBG_PRINT("readSen", "hysteresis is true, send the notification");
        mTerminal.println(">> " + str + ". Send notification" );
        
        mDoorStatus = doorStatus;
        Blynk.virtualWrite(V5, getString(mDoorStatus) );
        Blynk.notify(msg);        
      }
      else
      {
        DBG_PRINT("readSen", "hysteresis is false, do one more check");
        mTerminal.println(">> " + str + ". hysteresis check" );
        doorStatusHyst = true;
      }
    }
    else
    {
      //status is the same. reset the hysteresis if it happened to be triggered
      doorStatusHyst = false;
    }
  }
  mTerminal.flush();
}

//---------------------------------------------------------------
// microsecondsToCentimeters
// read the ultraSonicSensor in cm
//---------------------------------------------------------------
unsigned long microsecondsToCentimeters(unsigned long microseconds) {
  // The speed of sound is 340 m/s or 29 microseconds per centimeter.
  // The ping travels out and back, so to find the distance of the
  // object we take half of the distance travelled.
  return microseconds / 29 / 2;
}

//---------------------------------------------------------------
// readEE
// read a UInt32 from the EEPROM at address addr
//---------------------------------------------------------------
UInt32 readEE(Int32 addr)
{
  UInt32 val = 0;
  EEPROM.begin(512);
  EEPROM.get(addr, val);
  EEPROM.end();
  
  return val;
}

//---------------------------------------------------------------
// writeEE
// write the val UInt32 to the EEPROM at address addr
//---------------------------------------------------------------
void writeEE(Int32 addr, UInt32 val)
{
  EEPROM.begin(512);
  EEPROM.put(addr, val);
  EEPROM.commit();  
  EEPROM.end();
}

//---------------------------------------------------------------
// getString
// get the door status string
//---------------------------------------------------------------
String getString(const eDOORSTATUS stat)
{
  String retStr = "?";
  
  switch(stat)
  {
    case eDOOR_UNKNOWN:
      retStr = "UNKNOWN";    
    break;
    case eDOOR_OPEN:
      retStr = "OPEN";
    break;
    case eDOOR_CLOSED:
      retStr = "CLOSED";
    break;
    default:
      retStr = "??????";
    break;
  }

  return retStr;
}

//---------------------------------------------------------------
// debug_print
//
// easy way to enable/disable debug prints. 
// change the macro at the top of the file
//---------------------------------------------------------------
void debug_print(String fn, String msg)
{
  Serial.println(fn + ": " + msg);
}

