// MattzoTrainController for Power Functions Firmware
// Author: Dr. Matthias Runte
// Libraries can be downloaded easily from within the Arduino IDE using the library manager.
// TinyXML2 must be downloaded from https://github.com/leethomason/tinyxml2 (required files: tinyxml2.cpp, tinyxml2.h)

// Copyright 2020 by Dr. Matthias Runte
// License:
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <EEPROM.h>  // EEPROM library
#include <ESP8266WiFi.h>  // WiFi library
#include <PubSubClient.h>  // MQTT library
#include <tinyxml2.h>  // tiny xml 2 library
#include "MattzoPowerFunctions.h"  // Power Functions library

// The following includes need some enum definitions before the compiler gets to them...
// MOTORSHIELD_TYPE represents the type motor shield that this controller uses.
// 1 = L298N, 2 = L9110, 3 = Lego IR Receiver 8884
// Types 1 and 2 are motorshields that are physically connected to the controller
// Commands to type 3 are transmitted via an infrared LED
enum struct MotorShieldType
{
  NONE = 0x0,
  L298N = 0x1,
  L9110 = 0x2,
  LEGO_IR_8884 = 0x3
};

#include "MTC4PF_Configuration.h"  // this file should be placed in the same folder
#include "MattzoController_Library.h"  // this file needs to be placed in the Arduino library folder

using namespace tinyxml2;

/* MattzoController specifics */
String eepromIDString = "MattzoTrainController4PF";  // ID String. If found in EEPROM, the controller id is deemed to be set and used by the controller; if not, a random controller id is generated and stored in EEPROM memory
const int eepromIDStringLength = 24;  // length of the ID String. Needs to be updated if the ID String is changed.
unsigned int controllerNo;  // controllerNo. Read from memory upon starting the controller. Ranges between 1 and MAX_CONTROLLER_ID.
const int MAX_CONTROLLER_ID = 65000;

/* MQTT */
String mqttClientName;
char mqttClientName_char[eepromIDStringLength + 5 + 1];  // the name of the client must be given as char[]. Length must be the ID String plus 5 figures for the controller ID.

/* Functions (lights) */
bool functionCommand[NUM_FUNCTIONS];  // Desired state of a function
bool functionState[NUM_FUNCTIONS];    // Actual state of a function
const uint8_t IR_LIGHT_RED = 254;     // Constants for lights connected to Lego IR Receiver 8884. Function pins should only be set to this constant if MOTORSHIELD_TYPE == LEGO_IR_8884.
const uint8_t IR_LIGHT_BLUE = 255;
enum struct LightEventType
{
  STOP = 0x0,
  FORWARD = 0x1,
  REVERSE = 0x2
};


// Power functions object for LEGO IR Receiver 8884
MattzoPowerFunctions powerFunctions(IR_LED_PIN, IR_CHANNEL);

/* Send battery level  */
const int SEND_BATTERYLEVEL_INTERVAL = 60000; // interval for sending battery level in milliseconds
unsigned long lastBatteryLevelMsg = millis();    // time of the last sent battery level
const int BATTERY_PIN = A0;
const int VOLTAGE_MULTIPLIER = 20000/5000 - 1;   // Rbottom = 5 kOhm; Rtop = 20 kOhm; => voltage split factor
const int MAX_AI_VOLTAGE = 5100;  // maximum analog input voltage on pin A0. Usually 5000 = 5V = 5000mV. Can be slightly adapted to correct small deviations

// Motor acceleration parameters
const int ACCELERATION_INTERVAL = 100;       // pause between individual speed adjustments in milliseconds
const int ACCELERATE_STEP = 2;               // acceleration increment for a single acceleration step
const int BRAKE_STEP = 3;                    // brake decrement for a single braking step
int currentTrainSpeed = 0;                   // current speed of this train
int targetTrainSpeed = 0;                    // Target speed of this train
int maxTrainSpeed = 0;                       // Maximum speed of this train as configured in Rocrail
unsigned long lastAccelerate = millis();     // time of the last speed adjustment

// ebreak
boolean ebreak = false;   // Global emergency break flag.

// Wifi and MQTT objects
WiFiClient espClient;
PubSubClient client(espClient);




void setup() {
    Serial.begin(115200);
    randomSeed(ESP.getCycleCount());
    Serial.println("");
    Serial.println("MattzoController booting...");

    // initialize pins
    for (int i = 0; i < NUM_FUNCTIONS; i++) {
      pinMode(FUNCTION_PIN[i], OUTPUT);
      functionCommand[i] = false;
    }

    switch (MOTORSHIELD_TYPE) {
      case MotorShieldType::L298N:
        // initialize motor pins for L298N
        pinMode(enA, OUTPUT);
        pinMode(enB, OUTPUT);
      case MotorShieldType::L9110:
        // initialize motor pins for L298N (continued) and L9110
        pinMode(in1, OUTPUT);
        pinMode(in2, OUTPUT);
        pinMode(in3, OUTPUT);
        pinMode(in4, OUTPUT);
        break;
      case MotorShieldType::LEGO_IR_8884:
        // Power Functions instance is declared and initialized in the global section at the beginning of the code
        break;
    }

    // stop motors
    setTrainSpeed(0);

    loadPreferences();
    setupWifi();
    setupSysLog(mqttClientName_char);
    setupMQTT();

    mcLog("MattzoController setup completed.");
}

void loadPreferences() {
  int i;
  int controllerNoHiByte;
  int controllerNoLowByte;

  // set-up EEPROM read/write operations
  EEPROM.begin(512);

  // Check if the first part of the memory is filled with the MattzoController ID string.
  // This is the case if the controller has booted before with a MattzoController firmware.
  bool idStringCheck = true;
  for (i = 0; i < eepromIDString.length(); i++) {
    char charEeprom = EEPROM.read(i);
    char charIDString = eepromIDString.charAt(i);
    if (charEeprom != charIDString) {
      idStringCheck = false;
      break;
    }
  }

  // TODO: also write / read SSID, Wifi-Password and MQTT Server from EEPROM

  int paramsStartingPosition = eepromIDString.length();
  if (idStringCheck) {
    // load controller number from preferences
    controllerNoHiByte = EEPROM.read(paramsStartingPosition);
    controllerNoLowByte = EEPROM.read(paramsStartingPosition + 1);
    controllerNo = controllerNoHiByte * 256 + controllerNoLowByte;
    Serial.println("Loaded controllerNo from EEPROM: " + String(controllerNo));

  } else {
    // preferences not initialized yet -> initialize controller
    // this runs only a single time when starting the controller for the first time

    // Wait a bit to give the user some time to open the serial console...
    delay (5000);
    
    Serial.println("Initializing controller preferences on first start-up...");
    for (i = 0; i < eepromIDString.length(); i++) {
      EEPROM.write(i, eepromIDString.charAt(i));
    }

    // assign random controller number between 1 and 65000 and store in EEPROM
    controllerNo = random(1, MAX_CONTROLLER_ID);
    controllerNoHiByte = controllerNo / 256;
    controllerNoLowByte = controllerNo % 256;
    EEPROM.write(paramsStartingPosition, controllerNoHiByte);
    EEPROM.write(paramsStartingPosition + 1, controllerNoLowByte);

    // Commit EEPROM write operation
    EEPROM.commit();

    Serial.println("Assigned random controller no " + String(controllerNo) + " and stored to EEPROM");
  }

  // set MQTT client name
  mqttClientName = eepromIDString + String(controllerNo);
  mqttClientName.toCharArray(mqttClientName_char, mqttClientName.length() + 1);
}

void setupWifi() {
    delay(10);
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(WIFI_SSID);
 
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
 
    while (WiFi.status() != WL_CONNECTED) {
      delay(1000);
      Serial.print(".");

      // TODO: Support WPS! Store found Wifi network found via WPS in EEPROM and use next time!
    }
 
    Serial.println("");
    mcLog("WiFi connected. My IP address is " + WiFi.localIP().toString() + ".");
}
 
void setupMQTT() {
  client.setServer(MQTT_BROKER_IP, 1883);
  client.setCallback(mqttCallback);
  client.setBufferSize(2048);
  client.setKeepAlive(MQTT_KEEP_ALIVE_INTERVAL);   // keep alive interval
}

void sendMQTTBatteryLevel(){
  if (millis() - lastBatteryLevelMsg >= SEND_BATTERYLEVEL_INTERVAL) {
    lastBatteryLevelMsg = millis();
    int a0Value = analogRead(BATTERY_PIN);
    int voltage = map(a0Value, 0, 1023, 0, MAX_AI_VOLTAGE * VOLTAGE_MULTIPLIER);  // Battery Voltage in mV (Millivolt)
    if (voltage > 99999) voltage = 99999;

    mcLog("sending battery level raw=" + String(a0Value) + ", mv=" + String(voltage));
    String batteryMessage = mqttClientName + " raw=" + String(a0Value) + ", mv=" + String(voltage);
    char batteryMessage_char[batteryMessage.length() + 1];  // client name + 5 digits for voltage in mV plus terminating char
    batteryMessage.toCharArray(batteryMessage_char, batteryMessage.length() + 1);

    client.publish("roc2bricks/battery", batteryMessage_char);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char msg[length + 1];
  for (int i = 0; i < length; i++) {
    msg[i] = (char)payload[i];
  }
  msg[length] = '\0';

  mcLog("Received MQTT message [" + String(topic) + "]: " + String(msg));

  XMLDocument xmlDocument;
  if(xmlDocument.Parse(msg)!= XML_SUCCESS){
    mcLog("Error parsing");
    return;
  }

  mcLog("Parsing XML successful");

  const char * rr_id = "-unknown--unknown--unknown--unknown--unknown--unknown--unknown-";
  int rr_addr = 0;

  XMLElement *element;
  
  // check for lc message
  element = xmlDocument.FirstChildElement("lc");
  if (element != NULL) {
    mcLog("<lc> node found. Processing loco message...");

    // -> process lc (loco) message

    // query id attribute. This is the loco id.
    // The id is a mandatory field. If not found, the message is discarded.
    // Nevertheless, the id has no effect on the controller behaviour. Only the "addr" attribute is relevant for checking if the message is for this controller - see below.
    if (element->QueryStringAttribute("id", &rr_id) != XML_SUCCESS) {
      mcLog("id attribute not found or wrong type.");
      return;
    }
    mcLog("loco id: " + String(rr_id));
  
    // query addr attribute. This is the MattzoController id.
    // If this does not equal the LOCO_ADDRESS of this controller, the message is disregarded.
    if (element->QueryIntAttribute("addr", &rr_addr) != XML_SUCCESS) {
      mcLog("addr attribute not found or wrong type. Message disregarded.");
      return;
    }
    mcLog("addr: " + String(rr_addr));
    if (rr_addr != LOCO_ADDRESS) {
      mcLog("Message disgarded, as it is not for me, but for MattzoController No. " + String(rr_addr));
      return;
    }
  
    // query dir attribute. This is direction information for the loco (forward, backward)
    const char * rr_dir = "xxxxxx";  // expected values are "true" or "false"
    int dir;
    if (element->QueryStringAttribute("dir", &rr_dir) != XML_SUCCESS) {
      mcLog("dir attribute not found or wrong type.");
      return;
    }
    mcLog("dir (raw): " + String(rr_dir));
    if (strcmp(rr_dir, "true")==0) {
      mcLog("direction: forward");
      dir = 1;
    }
    else if (strcmp(rr_dir, "false")==0) {
      mcLog("direction: backward");
      dir = -1;
    }
    else {
      mcLog("unknown dir value - disregarding message.");
      return;
    }
  
    // query V attribute. This is the speed information for the loco and ranges from 0 to V_max (see below).
    int rr_v = 0;
    if (element->QueryIntAttribute("V", &rr_v) != XML_SUCCESS) {
      mcLog("V attribute not found or wrong type. Message disregarded.");
      return;
    }
    mcLog("V: " + String(rr_v));
  
    // query V_max attribute. This is maximum speed of the loco. It must be set in the loco settings in Rocrail as percentage value.
    // The V_max attribute is required to map to loco speed from rocrail to a power setting in the MattzoController.
    int rr_vmax = 0;
    if (element->QueryIntAttribute("V_max", &rr_vmax) != XML_SUCCESS) {
      mcLog("V_max attribute not found or wrong type. Message disregarded.");
      return;
    }
    mcLog("V_max: " + String(rr_vmax));
  
    // set target train speed
    targetTrainSpeed = rr_v * dir;
    maxTrainSpeed = rr_vmax;
    mcLog("Message parsing complete, target speed set to " + String(targetTrainSpeed) + " (current: " + String(currentTrainSpeed) + ", max: " + String(maxTrainSpeed) + ")");

    return;
  }

  // Check for fn message
  element = xmlDocument.FirstChildElement("fn");
  if (element != NULL) {
    mcLog("<fn> node found. Processing fn message...");

    // -> process fn (function) message

    // query id attribute. This is the loco id.
    // The id is a mandatory field. If not found, the message is discarded.
    // Nevertheless, the id has no effect on the controller behaviour. Only the "addr" attribute is relevant for checking if the message is for this controller - see below.
    if (element->QueryStringAttribute("id", &rr_id) != XML_SUCCESS) {
      mcLog("id attribute not found or wrong type.");
      return;
    }
    mcLog("function id: " + String(rr_id));
  
    // query addr attribute. This is the MattzoController id.
    // If this does not equal the LOCO_ADDRESS of this controller, the message is disregarded.
    if (element->QueryIntAttribute("addr", &rr_addr) != XML_SUCCESS) {
      mcLog("addr attribute not found or wrong type. Message disregarded.");
      return;
    }
    mcLog("addr: " + String(rr_addr));
    if (rr_addr != LOCO_ADDRESS) {
      mcLog("Message disgarded, as it is not for me, but for MattzoController No. " + String(rr_addr));
      return;
    }

    // query fnchanged attribute. This is information which function shall be set.
    int rr_functionNo;
    if (element->QueryIntAttribute("fnchanged", &rr_functionNo) != XML_SUCCESS) {
      mcLog("fnchanged attribute not found or wrong type. Message disregarded.");
      return;
    }
    mcLog("fnchanged: " + String(rr_functionNo));

    if (rr_functionNo < 1 || rr_functionNo > NUM_FUNCTIONS) {
      mcLog("fnchanged out of range. Message disregarded.");
      return;
    }
    int functionPinId = rr_functionNo - 1;
    mcLog("Function PIN Id: " + String(functionPinId));

    // query fnchangedstate attribute. This is value if the function shall be set on or off
    const char * rr_state = "xxxxxx";  // expected values are "true" or "false"
    if (element->QueryStringAttribute("fnchangedstate", &rr_state) != XML_SUCCESS) {
      mcLog("fnchangedstate attribute not found or wrong type.");
      return;
    }
    mcLog("fnchangedstate (raw): " + String(rr_state));
    if (strcmp(rr_state, "true")==0) {
      mcLog("fnchangedstate: true");
      functionCommand[functionPinId] = true;
    }
    else if (strcmp(rr_state, "false")==0) {
      mcLog("fnchangedstate: false");
      functionCommand[functionPinId] = false;
    }
    else {
      mcLog("unknown fnchangedstate value - disregarding message.");
      return;
    }

    return;
  }

  // Check for sys message
  element = xmlDocument.FirstChildElement("sys");
  if (element != NULL) {
    mcLog("<sys> node found. Processing sys message...");

    const char * rr_cmd = "-unknown--unknown--unknown--unknown--unknown--unknown--unknown-";

    // query cmd attribute. This is the system message type.
    if (element->QueryStringAttribute("cmd", &rr_cmd) != XML_SUCCESS) {
      mcLog("cmd attribute not found or wrong type.");
      return;
    }

    String rr_cmd_s = String(rr_cmd);
    mcLog("rocrail system command: " + String(rr_cmd_s));

    // Upon receiving "stop", "ebreak" or "shutdown" system command from Rocrail, the global emergency break flag is set. Train will stop immediately.
    // Upon receiving "go" command, the emergency break flag is be released (i.e. pressing the light bulb in Rocview).

    if (rr_cmd_s == "ebreak" || rr_cmd_s == "stop" || rr_cmd_s == "shutdown") {
      mcLog("received ebreak, stop or shutdown command. Stopping train.");
      ebreak = true;
    } else if (rr_cmd_s == "go") {
      mcLog("received go command. Releasing emergency break.");
      ebreak = false;
    } else {
      mcLog("received other system command, disregarded.");
    }
    return;
  }

  mcLog("Unknown message, disregarded.");
}

// set the motor to a desired power.
void setTrainSpeed(int newTrainSpeed) {
  const int MIN_ARDUINO_POWER = 400;   // minimum useful arduino power
  const int MAX_ARDUINO_POWER = 1023;  // maximum arduino power
  const int MAX_IR_SPEED = 100;  // maximum speed to power functions speed mapping function

  boolean dir = currentTrainSpeed >= 0;  // true = forward, false = reverse
  int power = 0;

  switch (MOTORSHIELD_TYPE) {
    case MotorShieldType::L298N:
      ;
    case MotorShieldType::L9110:
      // motor shield types L298N and L9110
      if (newTrainSpeed != 0) {
        // TODO: needs review!
        power = map(abs(newTrainSpeed), 0, maxTrainSpeed, MIN_ARDUINO_POWER, MAX_ARDUINO_POWER * maxTrainSpeed / 100);
      }
      mcLog("Setting motor speed: " + String(newTrainSpeed) + " (power: " + String(power) + "/" + MAX_ARDUINO_POWER + ")");

      if (MOTORSHIELD_TYPE == MotorShieldType::L298N) {
        // motor shield type L298N
        if (CONFIG_MOTOR_A != 0) {
          if (dir ^ (CONFIG_MOTOR_A < 0)) {
            digitalWrite(in1, LOW);
            digitalWrite(in2, HIGH);
          }
          else {
            digitalWrite(in1, HIGH);
            digitalWrite(in2, LOW);
          }
          analogWrite(enA, power);
        }
        if (CONFIG_MOTOR_B != 0) {
          if (dir ^ (CONFIG_MOTOR_B < 0)) {
            digitalWrite(in3, LOW);
            digitalWrite(in4, HIGH);
          }
          else {
            digitalWrite(in3, HIGH);
            digitalWrite(in4, LOW);
          }
          analogWrite(enB, power);
        }
      } else if (MOTORSHIELD_TYPE == MotorShieldType::L9110) {
        // motor shield type L9110
        if (CONFIG_MOTOR_A != 0) {
          if (dir ^ (CONFIG_MOTOR_A < 0)) {
            analogWrite(in1, 0);
            analogWrite(in2, power);
          }
          else {
            analogWrite(in1, power);
            analogWrite(in2, 0);
          }
          analogWrite(enA, power);
        }
        if (CONFIG_MOTOR_B != 0) {
          if (dir ^ (CONFIG_MOTOR_B < 0)) {
            analogWrite(in3, 0);
            analogWrite(in4, power);
          }
          else {
            analogWrite(in3, power);
            analogWrite(in4, 0);
          }
          analogWrite(enB, power);
        }
      }

      break;

    // motor shield type Lego IR Receiver 8884
    case MotorShieldType::LEGO_IR_8884:
      int irSpeed = 0;
      if (maxTrainSpeed > 0) {
        irSpeed = newTrainSpeed * MAX_IR_SPEED / maxTrainSpeed;
      }
      MattzoPowerFunctionsPwm pfPWMRed = powerFunctions.speedToPwm(IR_PORT_RED * irSpeed);
      MattzoPowerFunctionsPwm pfPWMBlue = powerFunctions.speedToPwm(IR_PORT_BLUE * irSpeed);
      mcLog("Setting motor speed: " + String(newTrainSpeed) + " (IR speed: " + irSpeed + ")");

      if (IR_PORT_RED) {
        powerFunctions.single_pwm(MattzoPowerFunctionsPort::RED, pfPWMRed);
      }
      if (IR_PORT_BLUE) {
        powerFunctions.single_pwm(MattzoPowerFunctionsPort::BLUE, pfPWMBlue);
      }
  } // of outer switch

  // Execute light events
  if (newTrainSpeed == 0 && currentTrainSpeed != 0) {
    lightEvent(LightEventType::STOP);
  } else if (newTrainSpeed > 0 && currentTrainSpeed <= 0) {
    lightEvent(LightEventType::FORWARD);
  } else if (newTrainSpeed < 0 && currentTrainSpeed >= 0) {
    lightEvent(LightEventType::REVERSE);
  }

  currentTrainSpeed = newTrainSpeed;
}

void reconnectMQTT() {
  while (!client.connected()) {
      mcLog("Reconnecting MQTT (" + String(MQTT_BROKER_IP) + ")...");

      String lastWillMessage = String(mqttClientName_char) + " " + "last will and testament";
      char lastWillMessage_char[lastWillMessage.length() + 1];
      lastWillMessage.toCharArray(lastWillMessage_char, lastWillMessage.length() + 1);

      if (!client.connect(mqttClientName_char, "roc2bricks/lastWill", 0, false, lastWillMessage_char)) {
        Serial.print("Failed, rc=");
        Serial.print(client.state());
        Serial.println(". Retrying in 5 seconds...");
        delay(5000);
      }
  }
  client.subscribe("rocrail/service/command");
  mcLog("MQTT connected, listening on topic [rocrail/service/command].");
}


// gently adapt train speed (increase/decrease slowly)
void accelerateTrainSpeed() {
  if (ebreak) {
    // emergency break pulled and train moving -> stop train immediately
    if (currentTrainSpeed != 0) {
      setTrainSpeed(0);
    }
  } else if (currentTrainSpeed != targetTrainSpeed) {
    if (targetTrainSpeed == 0){
      // stop train -> execute immediately
      setTrainSpeed(0);
    } else if (millis() - lastAccelerate >= ACCELERATION_INTERVAL) {
      lastAccelerate = millis();

      // determine if trains accelerates or brakes
      boolean accelerateFlag = abs(currentTrainSpeed) < abs(targetTrainSpeed) && (currentTrainSpeed * targetTrainSpeed > 0);
      int step = accelerateFlag ? ACCELERATE_STEP : BRAKE_STEP;

      int nextSpeed;
      // accelerate / brake gently
      if (currentTrainSpeed < targetTrainSpeed) {
        nextSpeed = min(currentTrainSpeed + step, targetTrainSpeed);
      } else {
        nextSpeed = max(currentTrainSpeed - step, targetTrainSpeed);
      }
      setTrainSpeed(nextSpeed);
    }
  }
}


// execute light event
void lightEvent(LightEventType le) {
  switch (le) {
  case LightEventType::STOP:
    // functionCommand[0] = false;
    // functionCommand[1] = false;
    // functionCommand[2] = false;
    break;
  case LightEventType::FORWARD:
    functionCommand[0] = true;
    functionCommand[1] = false;
    functionCommand[2] = false;
    break;
  case LightEventType::REVERSE:
    functionCommand[0] = true;
    functionCommand[1] = true;
    functionCommand[2] = true;
  }
}

// switch lights on or off
void setLights() {
  for (int i = 0; i < NUM_FUNCTIONS; i++) {
    bool onOff = functionCommand[i];

    if (ebreak) {
      // override function state on ebreak (alternate lights on/off every 500ms)
      long phase = (millis() / 500) % 2;
      onOff = (phase + i) % 2 == 0;
    }

    if (functionState[i] != onOff) {
      functionState[i] = onOff;
      mcLog("Flipping function " + String(i + 1));

      MattzoPowerFunctionsPwm irPwmLevel = onOff ? MattzoPowerFunctionsPwm::FORWARD7 : MattzoPowerFunctionsPwm::BRAKE;

      switch (FUNCTION_PIN[i]) {
      case IR_LIGHT_RED:
        powerFunctions.single_pwm(MattzoPowerFunctionsPort::RED, irPwmLevel);
        break;

      case IR_LIGHT_BLUE:
        powerFunctions.single_pwm(MattzoPowerFunctionsPort::BLUE, irPwmLevel);
        break;

      default:
        if (onOff) {
          digitalWrite(FUNCTION_PIN[i], HIGH);
        } else {
          digitalWrite(FUNCTION_PIN[i], LOW);
        }
      } // of switch
    } // of if
  } // of for
}


void loop() {
  if (!client.connected()) {
    reconnectMQTT();
  }
  client.loop();

  accelerateTrainSpeed();
  setLights();

  sendMQTTPing(&client, mqttClientName_char);
  sendMQTTBatteryLevel();
}
