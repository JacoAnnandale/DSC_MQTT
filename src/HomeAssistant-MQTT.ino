

#include <FS.h>  // must change to littleFS  

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <dscKeybusInterface.h>
#include <WiFiManager.h>
#include <ArduinoJson.h> // https://github.com/bblanchon/ArduinoJson


// Settings
///const char* wifiSSID = " ";
//const char* wifiPassword = " ";
const char* accessCode = "";  // An access code is required to disarm/night arm and may be required to arm based on panel configuration.
//      char* mqttServer = "";    // MQTT server domain name or IP address
 //     int mqttPort = 1883;      // MQTT server port
//const char* mqttUsername = "";  // Optional, leave blank if not required
//const char* mqttPassword = "";  // Optional, leave blank if not required

// MQTT topics - match to Home Assistant's configuration.yaml
const char* mqttClientName = "dscKeybusInterface";
const char* mqttPartitionTopic = "dsc/Get/Partition";  // Sends armed and alarm status per partition: dsc/Get/Partition1 ... dsc/Get/Partition8
const char* mqttZoneTopic = "dsc/Get/Zone";            // Sends zone status per zone: dsc/Get/Zone1 ... dsc/Get/Zone64
const char* mqttFireTopic = "dsc/Get/Fire";            // Sends fire status per partition: dsc/Get/Fire1 ... dsc/Get/Fire8
const char* mqttTroubleTopic = "dsc/Get/Trouble";      // Sends trouble status
const char* mqttStatusTopic = "dsc/Status";
const char* mqttBirthMessage = "online";
const char* mqttLwtMessage = "offline";
const char* mqttSubscribeTopic = "dsc/Set";            // Receives messages to write to the panel
unsigned long mqttPreviousTime;



// Configures the Keybus interface with the specified pins - dscWritePin is optional, leaving it out disables the
// virtual keypad.
#define dscClockPin D1  // esp8266: D1, D2, D8 (GPIO 5, 4, 15)
#define dscReadPin D2   // esp8266: D1, D2, D8 (GPIO 5, 4, 15)
#define dscWritePin D8  // esp8266: D1, D2, D8 (GPIO 5, 4, 15)
dscKeybusInterface dsc(dscClockPin, dscReadPin, dscWritePin);


#define buttonPin D5    // to call up configportal during normal operation
                        // or if pressed on boot to reset the wifi settings.


//***************************************
//define your default values here, if there are different values in config.json, they are overwritten.
char mqtt_server[40] = "192.168.0.2";
char mqtt_port[6]  = "1883";
char mqtt_Username[40] = "admin";  // Optional, leave blank if not required
char mqtt_Password[40] = "admin";  // Optional, leave blank if not required
//char api_token[32] = "YOUR_API_TOKEN";

//flag for saving data
bool shouldSaveConfig = false;



WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
//PubSubClient mqtt(mqttServer, mqttPort, wifiClient);
//*****************************************************************************************************




void setup() {
  Serial.begin(115200);
  Serial.println("Starting Up..........");
  Serial.println();
  pinMode(buttonPin,INPUT );
  
  WiFi.mode(WIFI_STA);

//**WIFI SETTINGS****************************************************************************************************

  SetupWifi();

  mqtt.setServer(mqtt_server,atoi(mqtt_port));
  


//**********************************************************************************************************

  
  mqtt.setCallback(mqttCallback);
  if (mqttConnect()) mqttPreviousTime = millis();
  else mqttPreviousTime = 0;

  // Starts the Keybus interface and optionally specifies how t print data.
  // begin() sets Serial by default and can accept a different stream: begin(Serial1), etc.
  dsc.begin();
  Serial.println(F("DSC Keybus Interface is online.")); 

  
}


bool buttonPressed(){

  if (digitalRead(buttonPin) == HIGH) 
    return true ;
    else return false;
  
}



void loop() {
  
  
  mqttHandle();

  // Test DSC Panel without being activated or connected
  // mqtt.subscribe(mqttSubscribeTopic);

  if (dsc.handlePanel() && dsc.statusChanged) {  // Processes data only when a valid Keybus command has been read
    dsc.statusChanged = false;                   // Reset the status tracking flag

    // If the Keybus data buffer is exceeded, the sketch is too busy to process all Keybus commands.  Call
    // handlePanel() more often, or increase dscBufferSize in the library: src/dscKeybusInterface.h
    if (dsc.bufferOverflow) Serial.println(F("Keybus buffer overflow"));
    dsc.bufferOverflow = false;

    // Checks if the interface is connected to the Keybus
    if (dsc.keybusChanged) {
      dsc.keybusChanged = false;  // Resets the Keybus data status flag
      if (dsc.keybusConnected) mqtt.publish(mqttStatusTopic, mqttBirthMessage, true);
      else mqtt.publish(mqttStatusTopic, mqttLwtMessage, true);
    }

    // Sends the access code when needed by the panel for arming
    if (dsc.accessCodePrompt && dsc.writeReady) {
      dsc.accessCodePrompt = false;
      dsc.write(accessCode);
    }

    if (dsc.troubleChanged) {
      dsc.troubleChanged = false;  // Resets the trouble status flag
      if (dsc.trouble) mqtt.publish(mqttTroubleTopic, "1", true);
      else mqtt.publish(mqttTroubleTopic, "0", true);
    }

    // Publishes status per partition
    for (byte partition = 0; partition < dscPartitions; partition++) {

      // Publishes exit delay status
      if (dsc.exitDelayChanged[partition]) {
        dsc.exitDelayChanged[partition] = false;  // Resets the exit delay status flag

        // Appends the mqttPartitionTopic with the partition number
        char publishTopic[strlen(mqttPartitionTopic) + 1];
        char partitionNumber[2];
        strcpy(publishTopic, mqttPartitionTopic);
        itoa(partition + 1, partitionNumber, 10);
        strcat(publishTopic, partitionNumber);

        if (dsc.exitDelay[partition]) mqtt.publish(publishTopic, "pending", true);  // Publish as a retained message
        else if (!dsc.exitDelay[partition] && !dsc.armed[partition]) mqtt.publish(publishTopic, "disarmed", true);
      }

      // Publishes armed/disarmed status
      if (dsc.armedChanged[partition]) {
        dsc.armedChanged[partition] = false;  // Resets the partition armed status flag

        // Appends the mqttPartitionTopic with the partition number
        char publishTopic[strlen(mqttPartitionTopic) + 1];
        char partitionNumber[2];
        strcpy(publishTopic, mqttPartitionTopic);
        itoa(partition + 1, partitionNumber, 10);
        strcat(publishTopic, partitionNumber);

        if (dsc.armed[partition]) {
          if (dsc.armedAway[partition]) mqtt.publish(publishTopic, "armed_away", true);
          else if (dsc.armedStay[partition]) mqtt.publish(publishTopic, "armed_home", true);
        }
        else mqtt.publish(publishTopic, "disarmed", true);
      }

      // Publishes alarm status
      if (dsc.alarmChanged[partition]) {
        dsc.alarmChanged[partition] = false;  // Resets the partition alarm status flag
        if (dsc.alarm[partition]) {

          // Appends the mqttPartitionTopic with the partition number
          char publishTopic[strlen(mqttPartitionTopic) + 1];
          char partitionNumber[2];
          strcpy(publishTopic, mqttPartitionTopic);
          itoa(partition + 1, partitionNumber, 10);
          strcat(publishTopic, partitionNumber);

          mqtt.publish(publishTopic, "triggered", true);  // Alarm tripped
        }
      }

      // Publishes fire alarm status
      if (dsc.fireChanged[partition]) {
        dsc.fireChanged[partition] = false;  // Resets the fire status flag

        // Appends the mqttFireTopic with the partition number
        char firePublishTopic[strlen(mqttFireTopic) + 1];
        char partitionNumber[2];
        strcpy(firePublishTopic, mqttFireTopic);
        itoa(partition + 1, partitionNumber, 10);
        strcat(firePublishTopic, partitionNumber);

        if (dsc.fire[partition]) mqtt.publish(firePublishTopic, "1");  // Fire alarm tripped
        else mqtt.publish(firePublishTopic, "0");                      // Fire alarm restored
      }
    }

    // Publishes zones 1-64 status in a separate topic per zone
    // Zone status is stored in the openZones[] and openZonesChanged[] arrays using 1 bit per zone, up to 64 zones:
    //   openZones[0] and openZonesChanged[0]: Bit 0 = Zone 1 ... Bit 7 = Zone 8
    //   openZones[1] and openZonesChanged[1]: Bit 0 = Zone 9 ... Bit 7 = Zone 16
    //   ...
    //   openZones[7] and openZonesChanged[7]: Bit 0 = Zone 57 ... Bit 7 = Zone 64
    if (dsc.openZonesStatusChanged) {
      dsc.openZonesStatusChanged = false;                           // Resets the open zones status flag
      for (byte zoneGroup = 0; zoneGroup < dscZones; zoneGroup++) {
        for (byte zoneBit = 0; zoneBit < 8; zoneBit++) {
          if (bitRead(dsc.openZonesChanged[zoneGroup], zoneBit)) {  // Checks an individual open zone status flag
            bitWrite(dsc.openZonesChanged[zoneGroup], zoneBit, 0);  // Resets the individual open zone status flag

            // Appends the mqttZoneTopic with the zone number
            char zonePublishTopic[strlen(mqttZoneTopic) + 2];
            char zone[3];
            strcpy(zonePublishTopic, mqttZoneTopic);
            itoa(zoneBit + 1 + (zoneGroup * 8), zone, 10);
            strcat(zonePublishTopic, zone);

            if (bitRead(dsc.openZones[zoneGroup], zoneBit)) {
              mqtt.publish(zonePublishTopic, "1", true);            // Zone open
            }
            else mqtt.publish(zonePublishTopic, "0", true);         // Zone closed
          }
        }
      }
    }

    mqtt.subscribe(mqttSubscribeTopic);
  }
}


// Handles messages received in the mqttSubscribeTopic
void mqttCallback(char* topic, byte* payload, unsigned int length) {

  // Handles unused parameters
  (void)topic;
  (void)length;

  byte partition = 0;
  byte payloadIndex = 0;

  Serial.print(topic);
  Serial.print("  ");
  Serial.println(payload[0]);


  // Checks if a partition number 1-8 has been sent and sets the second character as the payload
  if (payload[0] >= 0x31 && payload[0] <= 0x38) {
    partition = payload[0] - 49;
    payloadIndex = 1;
  }


  if (payload[0] == 'R' && payload[1] == 'S' && payload[2] == 'T'){
    Serial.println("Reset Command Received");
    delay(2000);
    ESP.reset();

  }





  // Arm stay
  if (payload[payloadIndex] == 'S' && !dsc.armed[partition] && !dsc.exitDelay[partition]) {
    while (!dsc.writeReady) dsc.handlePanel();  // Continues processing Keybus data until ready to write
    dsc.writePartition = partition + 1;         // Sets writes to the partition number
    dsc.write('s');                             // Virtual keypad arm stay
  }

  // Arm away
  else if (payload[payloadIndex] == 'A' && !dsc.armed[partition] && !dsc.exitDelay[partition]) {
    while (!dsc.writeReady) dsc.handlePanel();  // Continues processing Keybus data until ready to write
    dsc.writePartition = partition + 1;         // Sets writes to the partition number
    dsc.write('w');                             // Virtual keypad arm away
  }

  // Disarm
  else if (payload[payloadIndex] == 'D' && (dsc.armed[partition] || dsc.exitDelay[partition])) {
    while (!dsc.writeReady) dsc.handlePanel();  // Continues processing Keybus data until ready to write
    dsc.writePartition = partition + 1;         // Sets writes to the partition number
    dsc.write(accessCode);
    
  }
}


void mqttHandle() {
  if (!mqtt.connected()) {
    unsigned long mqttCurrentTime = millis();
    if (mqttCurrentTime - mqttPreviousTime > 5000) {
      mqttPreviousTime = mqttCurrentTime;
      if (mqttConnect()) {
        if (dsc.keybusConnected) mqtt.publish(mqttStatusTopic, mqttBirthMessage, true);
        Serial.println(F("MQTT disconnected, successfully reconnected."));
        mqttPreviousTime = 0;
      }
      else Serial.println(F("MQTT disconnected, failed to reconnect."));
    }
  }
  else mqtt.loop();
}


bool mqttConnect() {
  

  if (!mqtt.connected()) {
    mqttReconnect();
  }

  return mqtt.connected();

}
  /*if ((mqtt.connect(mqttClientName, mqttUsername, mqttPassword, mqttStatusTopic, 0, true, mqttLwtMessage)))
  {
  //if (mqttConnect()) 
    
    Serial.print(F("MQTT connected: "));
    Serial.println(mqttServer);
  }
  else {
    Serial.print(F("MQTT connection failed: "));
    Serial.println(mqttServer);
  }
  return mqtt.connected();
  
}*/

//************************************************************
//************************************************************
//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void SetupWifi() {

   
  WiFiManager wm; //WiFiManager, Local intialization. Once its business is done, there is no need to keep it around

  if (buttonPressed() == true) {
    Serial.println("Button has been pressed");
    Serial.println("SPIFFS FoRMAT......");
    SPIFFS.format();
    Serial.println("Wifi Settings ERASED......");
    wm.resetSettings();
   }

  setupSpiffs(); 
  
  wm.setSaveConfigCallback(saveConfigCallback); //set config save notify callback

  
 // setup custom parameters
  // 
  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
 
  
  WiFiManagerParameter custom_mqtt_server("server", "mqtt server", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "port", mqtt_port, 6);
  WiFiManagerParameter custom_mqtt_Username("username", "username", mqtt_Username, 40);
  WiFiManagerParameter custom_mqtt_Password("password", "password", mqtt_Password, 40);
  
 

 // WiFiManagerParameter custom_api_token("api", "api token", "", 32);

  //add all your parameters here
  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_port);
  wm.addParameter(&custom_mqtt_Username);
  wm.addParameter(&custom_mqtt_Password);
  
  // wm.addParameter(&custom_api_token);
  //reset settings - wipe credentials for testing
  //wm.resetSettings();

  wm.setDebugOutput(true);
  if (!wm.autoConnect("DSC_Alarm", "")) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    // if we still have not connected restart and try all over again
    ESP.restart();
    delay(5000);
  }
  

  //if you get here you have connected to the WiFi
  Serial.println("connected...yeey :)");

  //read updated parameters
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(mqtt_Username, custom_mqtt_Username.getValue());
  strcpy(mqtt_Password, custom_mqtt_Password.getValue());
  



  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["mqtt_server"] = mqtt_server;
    json["mqtt_port"]   = mqtt_port;
    json["mqtt_Username"]   = mqtt_Username;
    json["mqtt_Password"]   = mqtt_Password;
    
    // json["api_token"]   = api_token;
    // json["ip"]          = WiFi.localIP().toString();
    // json["gateway"]     = WiFi.gatewayIP().toString();
    // json["subnet"]      = WiFi.subnetMask().toString();

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    json.prettyPrintTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
    shouldSaveConfig = false;
  }

  Serial.println("local ip");
  Serial.println(WiFi.localIP());
  Serial.println(WiFi.gatewayIP());
  Serial.println(WiFi.subnetMask());
  
  
    

  /*wm.setDebugOutput(false);
  wm.autoConnect("DSC_Alarm_AP");
  Serial.print("WiFi connected: ");
  Serial.println(WiFi.localIP());
 */

}

void setupSpiffs(){
  //clean FS, for testing
  // SPIFFS.format();

  
  //read configuration from FS json
  Serial.println("mounting FS...");

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");
          strcpy(mqtt_server, json["mqtt_server"]);
          strcpy(mqtt_port, json["mqtt_port"]);
          strcpy(mqtt_Username, json["mqtt_Username"]);
          strcpy(mqtt_Password, json["mqtt_Password"]);

          // if(json["ip"]) {
          //   Serial.println("setting custom ip from config");
          //   strcpy(static_ip, json["ip"]);
          //   strcpy(static_gw, json["gateway"]);
          //   strcpy(static_sn, json["subnet"]);
          //   Serial.println(static_ip);
          // } else {
          //   Serial.println("no custom ip in config");
          // }

        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  //end read
}


void mqttReconnect() {
  // Loop until we're reconnected
  while (!mqtt.connected()) {
    Serial.print("Attempting MQTT connection...");
       // Attempt to connect

    //if (mqtt.connect(mqttClientName, mqttUsername, mqttPassword, mqttStatusTopic, 0, true, mqttLwtMessage)) {

    if (mqtt.connect("DscAlarm", mqtt_Username, mqtt_Password, mqttStatusTopic, 0, true, mqttLwtMessage)) {
      Serial.println("connected");
      // Once connected, publish an announcement...
      //client.publish("outTopic", "hello world");
      // ... and resubscribe
      //client.subscribe("inTopic");
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqtt.state());
      Serial.println(" try again in 5 seconds"); 
      Serial.println(mqtt_server);
      Serial.println(mqtt_port);
      Serial.println(mqtt_Username);
      Serial.println(mqtt_Password);
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}
