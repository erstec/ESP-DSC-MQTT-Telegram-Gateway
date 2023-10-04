//#include <Arduino.h>

#include <SPIFFS.h>

#include <ota_secret.h>
#include <settings.h>

// For ESP32
#ifndef LED_BUILTIN
#define LED_BUILTIN       2         // Pin D2 mapped to pin GPIO2/ADC12 of ESP32, control on-board LED
#endif

#define LED_OFF     LOW
#define LED_ON      HIGH

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>

#if defined(USE_MQTT)
#include <PubSubClient.h>
#endif

#if defined(USE_TELEGRAM)
#include <UniversalTelegramBot.h>
#include <HTTPUpdate.h>
#endif

#include <dscKeybusInterface.h>

#include <DNSServer.h>            //Local DNS Server used for redirecting all requests to the configuration portal
#include <WebServer.h>            //Local WebServer used to serve the configuration portal
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager WiFi Configuration Magic

#include <WiFiClientSecure.h>

WebServer server(80);    // Create a webserver object that listens for HTTP request on port 80

void handleRoot();              // function prototypes for HTTP handlers
void handleSaveParams();

#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson

#include <esp32_wdt.h>

// WiFi settings
String wifiSSID = "";
String wifiPassword = "";

bool saveResult = false;

time_t startTime;

#if defined(USE_MQTT)
//define your default values here, if there are different values in config.json, they are overwritten.
char mqtt_server[MQTT_SERVER_LEN];
char mqtt_port[MQTT_PORT_LEN] = "1883";
char mqtt_user[MQTT_USER_LEN] = "";
char mqtt_password[MQTT_PASSWORD_LEN] = "";
#endif

#if defined(USE_TELEGRAM)
char telegram_chat_id[TELEGRAM_CHAT_ID_LEN] = "";
char telegram_bot_token[TELEGRAM_BOT_TOKEN_LEN] = "";
char telegram_msg_prefix[TELEGRAM_MSG_PREFIX_LEN] = "";
//const char* telegram_msg_prefix = "[Security system] ";

bool telegramEnabled = false;

char keybuf[32];
#endif

#if defined(USE_MQTT)
WiFiClient wifiClient;
#endif

#if defined(USE_TELEGRAM)
WiFiClientSecure wifiClientSecured;
#endif

#if defined(USE_MQTT) || defined(USE_TELEGRAM)
char dsc_access_code[DSC_ACCESS_CODE_LEN];
#endif

#if defined(USE_MQTT)
// MQTT topics - match to Homebridge's config.json
char mqttClientName[32];
const char* mqttPartitionTopic = "dsc/Get/Partition";  // Sends armed and alarm status per partition: dsc/Get/Partition1 ... dsc/Get/Partition8
const char* mqttZoneTopic = "dsc/Get/Zone";            // Sends zone status per zone: dsc/Get/Zone1 ... dsc/Get/Zone64
const char* mqttFireTopic = "dsc/Get/Fire";            // Sends fire status per partition: dsc/Get/Fire1 ... dsc/Get/Fire8
const char* mqttPgmTopic = "dsc/Get/PGM";              // Sends PGM status per PGM: dsc/Get/PGM1 ... dsc/Get/PGM14
const char* mqttSubscribeTopic = "dsc/Set";            // Receives messages to write to the panel
const char* mqttLWTTopic = "dsc/status/LWT";
const char* mqttLWTonline = "Online";
const char* mqttLWToffline = "Offline";
unsigned long mqttPreviousTime;
char exitState;

PubSubClient mqtt(mqtt_server, atoi(mqtt_port), wifiClient);

void mqttCallback(char* topic, byte* payload, unsigned int length);
bool mqttConnect();
void mqttHandle();

bool mqttEnabled = false;

void publishState(const char* sourceTopic, byte partition, const char* targetSuffix, const char* currentState);
#endif

#if defined(USE_TELEGRAM)
UniversalTelegramBot telegramBot(telegram_bot_token, wifiClientSecured);
const int telegramCheckInterval = 1000;

void handleTelegram(byte telegramMessages);
bool sendMessage(const char* messageContent);
void appendPartition(byte sourceNumber, char* message);

void bot_setup();
#endif

//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

// Configures the Keybus interface with the specified pins - dscWritePin is
// optional, leaving it out disables the virtual keypad
#define dscClockPin 18  // esp32: 4,13,16-39
#define dscReadPin 19   // esp32: 4,13,16-39
#define dscWritePin 21  // esp32: 4,13,16-33

bool wifiConnected = true;

// Initialize components
dscKeybusInterface dsc(dscClockPin, dscReadPin, dscWritePin);

typedef struct {
  char *val;
  size_t len;
  String name;
  String webFormName;
  String webFormText;
} tsConfig;

tsConfig _config[] = {
#if defined(USE_MQTT)
  { mqtt_server, sizeof(mqtt_server), "mqtt_server", "mqtt-server", "MQTT Server" },
  { mqtt_port, sizeof(mqtt_port), "mqtt_port", "mqtt-port", "MQTT Port" },
  { mqtt_user, sizeof(mqtt_user), "mqtt_user", "mqtt-user", "MQTT User" },
  { mqtt_password, sizeof(mqtt_password), "mqtt_password", "mqtt-psw", "MQTT Password" },
#endif
#if defined(USE_MQTT) || defined(USE_TELEGRAM)
  { dsc_access_code, sizeof(dsc_access_code), "dsc_access_code", "dsc-access-code", "DSC Panel Access Code" },
#endif
#if defined(USE_TELEGRAM)
  { telegram_bot_token, sizeof(telegram_bot_token), "telegram_bot_token", "telegram-bot-token", "Telegram Bot Token" },
  { telegram_chat_id, sizeof(telegram_chat_id), "telegram_chat_id", "telegram-chat-id", "Telegram Chat ID" },
  { telegram_msg_prefix, sizeof(telegram_msg_prefix), "telegram_msg_prefix", "telegram-msg-prefix", "Telegram Message Prefix" },
#endif
};

#define COMMON_NUMEL(ARRAY) (sizeof(ARRAY) / sizeof(ARRAY[0]))

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);

  Serial.begin(115200);
  Serial.println();

  wdt_enable(WDT_TMO);
  
  //read configuration from FS json
  Serial.println("mounting FS...");

  if (SPIFFS.begin(true)) {
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
        DynamicJsonDocument json(1024);
        DeserializationError error = deserializeJson(json, buf.get());
        serializeJson(json, Serial);
        if (!error) {
          Serial.println("\nparsed json");

          for (int idx = 0; idx < COMMON_NUMEL(_config); idx++) {
            if (json.containsKey(_config[idx].name)) {
              strcpy(_config[idx].val, json[_config[idx].name]);
            }
          }
        } else {
          Serial.println("failed to load json config");
        }
        configFile.close();
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  //end read

  //WiFiManager
  //Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;

  // set Hostname
  String hostname = "ESP-DSC-" + String((uint32_t)(ESP.getEfuseMac() >> 32), HEX);
  wifiManager.setHostname(hostname.c_str());

  //set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //set static ip
  //wifiManager.setSTAStaticIPConfig(IPAddress(10,0,1,99), IPAddress(10,0,1,1), IPAddress(255,255,255,0));
  
  //reset settings - for testing
  //wifiManager.resetSettings();

  // set dark theme
  wifiManager.setClass("invert");

  //set minimu quality of signal so it ignores AP's under that quality
  //defaults to 8%
  //wifiManager.setMinimumSignalQuality();
  
  //sets timeout until configuration portal gets turned off
  //useful to make it all retry or go to sleep
  //in seconds
  wifiManager.setTimeout(300);
  wifiManager.setConfigPortalTimeout(300);

  wdt_reset();
  //fetches ssid and pass and tries to connect
  //if it does not connect it starts an access point with the specified name
  //here  "AutoConnectAP"
  //and goes into a blocking loop awaiting configuration
//  if (!wifiManager.autoConnect("AutoConnectAP", "password")) {
  if (!wifiManager.autoConnect()) {
    Serial.println("failed to connect and hit timeout");
    wdt_reset();
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.restart();
    delay(5000);
  } else {
    Serial.println("AutoConnect ended");
  }

  if (shouldSaveConfig) {
    ESP.restart();
  }

  //if you get here you have connected to the WiFi
  Serial.println("connected...yeey");

  // WiFi.onEvent(WiFiStationDisconnected, SYSTEM_EVENT_STA_DISCONNECTED);

  if (MDNS.begin("esp32")) {              // Start the mDNS responder for esp8266.local
    Serial.println("mDNS responder started");
  } else {
    Serial.println("Error setting up MDNS responder!");
  }

  server.on("/", handleRoot);
  server.on("/SaveParams", HTTP_POST, handleSaveParams);
  server.onNotFound([](){
    server.send(404, "text/plain", "404: Not found");
  });

  server.begin();                           // Actually start the server
  Serial.println("HTTP server started");

  Serial.print(F("NTP time..."));
  configTime(0, 0, "pool.ntp.org");
  time_t now = time(nullptr);
  while (now < 24 * 3600)
  {
    Serial.print(".");
    delay(2000);
    now = time(nullptr);
  }
  Serial.println(F("synchronized."));
  startTime = time(nullptr);

  Serial.print("Local IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("SSID: ");
  wifiSSID = WiFi.SSID();
  Serial.println(wifiSSID);  
  Serial.print("PSK: ");
  wifiPassword = WiFi.psk();
  Serial.println(wifiPassword);

  wdt_reset();

#if defined(USE_MQTT)
  // MQTT
  String mqttClientId = "DSC-";
  mqttClientId += String(random(0xffff), HEX);
  strcpy(mqttClientName, mqttClientId.c_str());
  Serial.print("MQTT configuration: server: ");
  Serial.print(mqtt_server);
  Serial.print(" port: ");
  Serial.println(mqtt_port);
  if (mqtt_server[0] != 0x00 && mqtt_port[0] != 0x00) {
    Serial.println("Starting MQTT");
    mqttEnabled = true;
    mqtt.setServer(mqtt_server, atoi(mqtt_port));
    mqtt.setCallback(mqttCallback);
    if (mqttConnect()) mqttPreviousTime = millis();
    else mqttPreviousTime = 0;
  } else {
    Serial.println("Wrong MQTT configuration, start aborted!");
  }
#endif

#if defined(USE_TELEGRAM)
  // TeleGram
  if (telegram_bot_token[0] != 0x00) {
    telegramEnabled = true;
    telegramBot.updateToken(telegram_bot_token);
    wifiClientSecured.setCACert(TELEGRAM_CERTIFICATE_ROOT); // Add root certificate for api.telegram.org
    // Sends a message on startup to verify connectivity
    Serial.print(F("Telegram..."));
    String tgHelloMsg = "";
    tgHelloMsg = "Initializing v";
    tgHelloMsg += version;
    tgHelloMsg += "... ";
    if (sendMessage(tgHelloMsg.c_str())) Serial.println(F("connected."));
    else Serial.println(F("connection error."));
    //bot_setup();
  } else {
    Serial.println("Wrong Telegram configuration, start aborted!");
  }
#endif

  // Port defaults to 8266
  // ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  // ArduinoOTA.setHostname("myesp8266");

  // Authentication passwrord (default: no auth)
  ArduinoOTA.setPassword(OTA_PASSWORD);

  // Password can be set with it's md5 value as well
  // MD5(admin) = 
  // ArduinoOTA.setPasswordHash("md5_hash_here");

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_FS
      type = "filesystem";
    }

    // NOTE: if updating FS this would be the place to unmount FS using FS.end()
    Serial.println("Start updating " + type);

    dsc.stop();

    wdt_reset();
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
    wdt_reset();
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    wdt_reset();
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      Serial.println("Auth Failed");
    } else if (error == OTA_BEGIN_ERROR) {
      Serial.println("Begin Failed");
    } else if (error == OTA_CONNECT_ERROR) {
      Serial.println("Connect Failed");
    } else if (error == OTA_RECEIVE_ERROR) {
      Serial.println("Receive Failed");
    } else if (error == OTA_END_ERROR) {
      Serial.println("End Failed");
    }
    wdt_reset();
  });
  ArduinoOTA.begin();

  // Starts the Keybus interface and optionally specifies how to print data.
  // begin() sets Serial by default and can accept a different stream: begin(Serial1), etc.
  dsc.begin();

  Serial.println(F("DSC Keybus Interface is online."));

//  MDNS.addService("http", "tcp", 80);
}

// void WiFiStationDisconnected(WiFiEvent_t event, WiFiEventInfo_t info){
//   Serial.println("Disconnected from WiFi access point");
//   Serial.println("Trying to Reconnect");
//   wifiConnected = false;
//   dsc.pauseStatus = true;
//   WiFi.reconnect();
// }

void loop() {
  if (!saveResult) {
    wdt_reset();
  }
  
  //MDNS.update();

  // Updates status if WiFi drops and reconnects
  if (!wifiConnected && WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi reconnected");
    wifiConnected = true;
    dsc.pauseStatus = false;
    dsc.statusChanged = true;
  }
  else if (WiFi.status() != WL_CONNECTED && wifiConnected) {
    Serial.println("WiFi disconnected");
    wifiConnected = false;
    dsc.pauseStatus = true;
    Serial.println("Reconnecting to WiFi...");
    WiFi.disconnect();
    WiFi.reconnect();
  }

#if defined(USE_MQTT)
  if (mqttEnabled)   mqttHandle();
#endif

#if defined(USE_TELEGRAM)
  if (telegramEnabled) {
    // Checks for incoming Telegram messages
    static unsigned long telegramPreviousTime;
    if (millis() - telegramPreviousTime > telegramCheckInterval) {
      wifiClientSecured.setHandshakeTimeout(30);  // Workaround for https://github.com/espressif/arduino-esp32/issues/6165
      byte telegramMessages = telegramBot.getUpdates(telegramBot.last_message_received + 1);
      while (telegramMessages) {
        handleTelegram(telegramMessages);
        telegramMessages = telegramBot.getUpdates(telegramBot.last_message_received + 1);
      }
      telegramPreviousTime = millis();
    }
  }
#endif

  ArduinoOTA.handle();

  server.handleClient();                    // Listen for HTTP requests from clients

  dsc.loop();

  if (dsc.statusChanged) {                  // Checks if the security system status has changed
    dsc.statusChanged = false;              // Resets the status flag

    // If the Keybus data buffer is exceeded, the sketch is too busy to process all Keybus commands.  Call
    // handlePanel() more often, or increase dscBufferSize in the library: src/dscKeybusInterface.h
    if (dsc.bufferOverflow) Serial.println(F("Keybus buffer overflow"));
    dsc.bufferOverflow = false;

#if defined(USE_TELEGRAM)
    // Checks if the interface is connected to the Keybus
    if (dsc.keybusChanged) {
      dsc.keybusChanged = false;                 // Resets the Keybus data status flag
      if (dsc.keybusConnected) {
        sendMessage("Connected");
      }
      else {
        sendMessage("Disconnected");
      }
    }
#endif

#if defined(USE_MQTT) || defined(USE_TELEGRAM)
    // Sends the access code when needed by the panel for arming
    if (dsc.accessCodePrompt) {
      dsc.accessCodePrompt = false;
      dsc.write(dsc_access_code);
    }
#endif

    // Publishes status per partition
    for (byte partition = 0; partition < dscPartitions; partition++) {
#if defined(USE_MQTT) || defined(USE_TELEGRAM)
      // Skips processing if the partition is disabled or in installer programming
      if (dsc.disabled[partition]) continue;

      // Publishes armed/disarmed status
      if (dsc.armedChanged[partition]) {
        if (dsc.armed[partition]) {
#if defined(USE_MQTT)
          exitState = 0;
#endif
#if defined(USE_TELEGRAM)
        char messageContent[25];
#endif

          // Night armed away
          if (dsc.armedAway[partition] && dsc.noEntryDelay[partition]) {
#if defined(USE_MQTT)
            publishState(mqttPartitionTopic, partition, "N", "NA");
#endif
#if defined(USE_TELEGRAM)
            strcpy(messageContent, "Armed night: Partition ");
#endif
          }

          // Armed away
          else if (dsc.armedAway[partition]) {
#if defined(USE_MQTT)
            publishState(mqttPartitionTopic, partition, "A", "AA");
#endif
#if defined(USE_TELEGRAM)
            strcpy(messageContent, "Armed away: Partition ");
#endif
          }

          // Night armed stay
          else if (dsc.armedStay[partition] && dsc.noEntryDelay[partition]) {
#if defined(USE_MQTT)
            publishState(mqttPartitionTopic, partition, "N", "NA");
#endif
#if defined(USE_TELEGRAM)
            strcpy(messageContent, "Armed night: Partition ");
#endif
          }

          // Armed stay
          else if (dsc.armedStay[partition]) {
#if defined(USE_MQTT)
            publishState(mqttPartitionTopic, partition, "S", "SA");
#endif
#if defined(USE_TELEGRAM)
            strcpy(messageContent, "Armed stay: Partition ");
#endif
          }

#if defined(USE_TELEGRAM)
          appendPartition(partition, messageContent);  // Appends the message with the partition number
          sendMessage(messageContent);
#endif
        }

        // Disarmed
        else {
#if defined(USE_TELEGRAM)
          char messageContent[22] = "Disarmed: Partition ";
          appendPartition(partition, messageContent);  // Appends the message with the partition number
          sendMessage(messageContent);
#endif
#if defined(USE_MQTT)
          publishState(mqttPartitionTopic, partition, "D", "D");
#endif
        }
      }

      // Checks exit delay status
      if (dsc.exitDelayChanged[partition]) {
        dsc.exitDelayChanged[partition] = false;  // Resets the exit delay status flag
#if defined(USE_MQTT)
        // Exit delay in progress
        if (dsc.exitDelay[partition]) {

          // Sets the arming target state if the panel is armed externally
          if (exitState == 0 || dsc.exitStateChanged[partition]) {
            dsc.exitStateChanged[partition] = 0;
            switch (dsc.exitState[partition]) {
              case DSC_EXIT_STAY: {
                exitState = 'S';
                publishState(mqttPartitionTopic, partition, "S", 0);
                break;
              }
              case DSC_EXIT_AWAY: {
                exitState = 'A';
                publishState(mqttPartitionTopic, partition, "A", 0);
                break;
              }
              case DSC_EXIT_NO_ENTRY_DELAY: {
                exitState = 'N';
                publishState(mqttPartitionTopic, partition, "N", 0);
                break;
              }
            }
          }
        }
        // Disarmed during exit delay
        else if (!dsc.armed[partition]) {
          exitState = 0;
          publishState(mqttPartitionTopic, partition, "D", "D");
        }
#endif
#if defined(USE_TELEGRAM)
        if (dsc.exitDelay[partition]) {
          char messageContent[36] = "Exit delay in progress: Partition ";
          appendPartition(partition, messageContent);  // Appends the message with the partition number
          sendMessage(messageContent);
        }
        else if (!dsc.exitDelay[partition] && !dsc.armed[partition]) {
          char messageContent[22] = "Disarmed: Partition ";
          appendPartition(partition, messageContent);  // Appends the message with the partition number
          sendMessage(messageContent);
        }
#endif
      }

      // Publishes alarm triggered status
      if (dsc.alarmChanged[partition]) {
        dsc.alarmChanged[partition] = false;  // Resets the partition alarm status flag
        if (dsc.alarm[partition]) {
#if defined(USE_MQTT)
          publishState(mqttPartitionTopic, partition, 0, "T");
#endif
#if defined(USE_TELEGRAM)
          char messageContent[19] = "Alarm: Partition ";
          appendPartition(partition, messageContent);  // Appends the message with the partition number
          sendMessage(messageContent);
#endif
        }
        else if (!dsc.armedChanged[partition]) {
#if defined(USE_MQTT)
          publishState(mqttPartitionTopic, partition, "D", "D");
#endif
#if defined(USE_TELEGRAM)
          char messageContent[22] = "Disarmed: Partition ";
          appendPartition(partition, messageContent);  // Appends the message with the partition number
          sendMessage(messageContent);
#endif
        }
      }
      if (dsc.armedChanged[partition]) dsc.armedChanged[partition] = false;  // Resets the partition armed status flag
#endif

      // Publishes fire alarm status
      if (dsc.fireChanged[partition]) {
        dsc.fireChanged[partition] = false;  // Resets the fire status flag
#if defined(USE_MQTT) || defined(USE_TELEGRAM)
        if (dsc.fire[partition]) {
#if defined(USE_MQTT)
          publishState(mqttFireTopic, partition, 0, "1");  // Fire alarm tripped
#endif
#if defined(USE_TELEGRAM)
          char messageContent[24] = "Fire alarm: Partition ";
          appendPartition(partition, messageContent);  // Appends the message with the partition number
          sendMessage(messageContent);
#endif
        }
        else {
#if defined(USE_MQTT)
          publishState(mqttFireTopic, partition, 0, "0");  // Fire alarm restored
#endif
#if defined(USE_TELEGRAM)
          char messageContent[33] = "Fire alarm restored: Partition ";
          appendPartition(partition, messageContent);  // Appends the message with the partition number
          sendMessage(messageContent);
#endif
        }
#endif
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
#if defined(USE_MQTT)
            // Appends the mqttZoneTopic with the zone number
            char zonePublishTopic[strlen(mqttZoneTopic) + 3];
            char zone[3];
            strcpy(zonePublishTopic, mqttZoneTopic);
            itoa(zoneBit + 1 + (zoneGroup * 8), zone, 10);
            strcat(zonePublishTopic, zone);

            if (mqttEnabled) {
              if (bitRead(dsc.openZones[zoneGroup], zoneBit)) {
                mqtt.publish(zonePublishTopic, "1", true);            // Zone open
              }
              else mqtt.publish(zonePublishTopic, "0", true);         // Zone closed
            }
#endif
          }
        }
      }
    }

#if defined(USE_TELEGRAM)
    // Zone alarm status is stored in the alarmZones[] and alarmZonesChanged[] arrays using 1 bit per zone, up to 64 zones
    //   alarmZones[0] and alarmZonesChanged[0]: Bit 0 = Zone 1 ... Bit 7 = Zone 8
    //   alarmZones[1] and alarmZonesChanged[1]: Bit 0 = Zone 9 ... Bit 7 = Zone 16
    //   ...
    //   alarmZones[7] and alarmZonesChanged[7]: Bit 0 = Zone 57 ... Bit 7 = Zone 64
    if (dsc.alarmZonesStatusChanged) {
      dsc.alarmZonesStatusChanged = false;                           // Resets the alarm zones status flag
      for (byte zoneGroup = 0; zoneGroup < dscZones; zoneGroup++) {
        for (byte zoneBit = 0; zoneBit < 8; zoneBit++) {
          if (bitRead(dsc.alarmZonesChanged[zoneGroup], zoneBit)) {  // Checks an individual alarm zone status flag
            bitWrite(dsc.alarmZonesChanged[zoneGroup], zoneBit, 0);  // Resets the individual alarm zone status flag
            if (bitRead(dsc.alarmZones[zoneGroup], zoneBit)) {
              char messageContent[15] = "Zone alarm: ";
              char zoneNumber[3];
              itoa((zoneBit + 1 + (zoneGroup * 8)), zoneNumber, 10); // Determines the zone number
              strcat(messageContent, zoneNumber);
              sendMessage(messageContent);
            }
            else {
              char messageContent[24] = "Zone alarm restored: ";
              char zoneNumber[3];
              itoa((zoneBit + 1 + (zoneGroup * 8)), zoneNumber, 10); // Determines the zone number
              strcat(messageContent, zoneNumber);
              sendMessage(messageContent);
            }
          }
        }
      }
    }
#endif

    // Publishes PGM outputs 1-14 status in a separate topic per zone
    // PGM status is stored in the pgmOutputs[] and pgmOutputsChanged[] arrays using 1 bit per PGM output:
    //   pgmOutputs[0] and pgmOutputsChanged[0]: Bit 0 = PGM 1 ... Bit 7 = PGM 8
    //   pgmOutputs[1] and pgmOutputsChanged[1]: Bit 0 = PGM 9 ... Bit 5 = PGM 14
    if (dsc.pgmOutputsStatusChanged) {
      dsc.pgmOutputsStatusChanged = false;  // Resets the PGM outputs status flag
      for (byte pgmGroup = 0; pgmGroup < 2; pgmGroup++) {
        for (byte pgmBit = 0; pgmBit < 8; pgmBit++) {
          if (bitRead(dsc.pgmOutputsChanged[pgmGroup], pgmBit)) {  // Checks an individual PGM output status flag
            bitWrite(dsc.pgmOutputsChanged[pgmGroup], pgmBit, 0);  // Resets the individual PGM output status flag
#if defined(USE_MQTT)
            // Appends the mqttPgmTopic with the PGM number
            char pgmPublishTopic[strlen(mqttPgmTopic) + 3];
            char pgm[3];
            strcpy(pgmPublishTopic, mqttPgmTopic);
            itoa(pgmBit + 1 + (pgmGroup * 8), pgm, 10);
            strcat(pgmPublishTopic, pgm);

            if (mqttEnabled) {
              if (bitRead(dsc.pgmOutputs[pgmGroup], pgmBit)) {
                mqtt.publish(pgmPublishTopic, "1", true);           // PGM enabled
              }
              else mqtt.publish(pgmPublishTopic, "0", true);        // PGM disabled
            }
#endif
          }
        }
      }
    }

#if defined(USE_MQTT)
    if (mqttEnabled) {
      mqtt.subscribe(mqttSubscribeTopic);
    }
#endif

#if defined(USE_TELEGRAM)
    // Checks trouble status
    if (dsc.troubleChanged) {
      dsc.troubleChanged = false;  // Resets the trouble status flag
      if (dsc.trouble) sendMessage("Trouble status on");
      else sendMessage("Trouble status restored");
    }

    // Checks for AC power status
    if (dsc.powerChanged) {
      dsc.powerChanged = false;  // Resets the battery trouble status flag
      if (dsc.powerTrouble) sendMessage("AC power trouble");
      else sendMessage("AC power restored");
    }

    // Checks panel battery status
    if (dsc.batteryChanged) {
      dsc.batteryChanged = false;  // Resets the battery trouble status flag
      if (dsc.batteryTrouble) sendMessage("Panel battery trouble");
      else sendMessage("Panel battery restored");
    }

    // Checks for keypad fire alarm status
    if (dsc.keypadFireAlarm) {
      dsc.keypadFireAlarm = false;  // Resets the keypad fire alarm status flag
      sendMessage("Keypad Fire alarm");
    }

    // Checks for keypad aux auxiliary alarm status
    if (dsc.keypadAuxAlarm) {
      dsc.keypadAuxAlarm = false;  // Resets the keypad auxiliary alarm status flag
      sendMessage("Keypad Aux alarm");
    }

    // Checks for keypad panic alarm status
    if (dsc.keypadPanicAlarm) {
      dsc.keypadPanicAlarm = false;  // Resets the keypad panic alarm status flag
      sendMessage("Keypad Panic alarm");
    }
#endif
  }
}

#if defined(USE_MQTT)
// Handles messages received in the mqttSubscribeTopic
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Handles unused parameters
  (void)topic;
  (void)length;

  byte partition = 0;
  byte payloadIndex = 0;

  // Checks if a partition number 1-8 has been sent and sets the second character as the payload
  if (payload[0] >= 0x31 && payload[0] <= 0x38) {
    partition = payload[0] - 49;
    payloadIndex = 1;
  }

  // Resets the HomeKit target state if attempting to change the armed mode while armed or not ready
  if (payload[payloadIndex] != 'D' && !dsc.ready[partition]) {
    dsc.armedChanged[partition] = true;
    dsc.statusChanged = true;
    return;
  }

  // Resets the HomeKit target state if attempting to change the arming mode during the exit delay
  if (payload[payloadIndex] != 'D' && dsc.exitDelay[partition] && exitState != 0) {
    if (exitState == 'S') publishState(mqttPartitionTopic, partition, "S", 0);
    else if (exitState == 'A') publishState(mqttPartitionTopic, partition, "A", 0);
    else if (exitState == 'N') publishState(mqttPartitionTopic, partition, "N", 0);
  }


  // homebridge-mqttthing STAY_ARM
  if (payload[payloadIndex] == 'S' && !dsc.armed[partition] && !dsc.exitDelay[partition]) {
    dsc.writePartition = partition + 1;    // Sets writes to the partition number
    dsc.write('s');  // Keypad stay arm
    publishState(mqttPartitionTopic, partition, "S", 0);
    exitState = 'S';
    return;
  }

  // homebridge-mqttthing AWAY_ARM
  if (payload[payloadIndex] == 'A' && !dsc.armed[partition] && !dsc.exitDelay[partition]) {
    dsc.writePartition = partition + 1;    // Sets writes to the partition number
    dsc.write('w');  // Keypad away arm
    publishState(mqttPartitionTopic, partition, "A", 0);
    exitState = 'A';
    return;
  }

  // homebridge-mqttthing NIGHT_ARM
  if (payload[payloadIndex] == 'N' && !dsc.armed[partition] && !dsc.exitDelay[partition]) {
    dsc.writePartition = partition + 1;    // Sets writes to the partition number
    dsc.write('n');  // Keypad arm with no entry delay
    publishState(mqttPartitionTopic, partition, "N", 0);
    exitState = 'N';
    return;
  }

  // homebridge-mqttthing DISARM
  if (payload[payloadIndex] == 'D' && (dsc.armed[partition] || dsc.exitDelay[partition] || dsc.alarm[partition])) {
    dsc.writePartition = partition + 1;    // Sets writes to the partition number
    dsc.write(dsc_access_code);
    return;
  }
}

bool MQTTclient_must_send_LWT_connected = false;

#if 0
int mqttLWTPeriod;
static const int MQTT_LWT_PERIOD = 10000;
#endif

void mqttHandle() {
  if (!mqtt.connected()) {
    unsigned long mqttCurrentTime = millis();
    if (mqttCurrentTime - mqttPreviousTime > 5000) {
      mqttPreviousTime = mqttCurrentTime;
      if (mqttConnect()) {
        Serial.println(F("MQTT disconnected, successfully reconnected."));
        mqttPreviousTime = 0;
      }
      else Serial.println(F("MQTT disconnected, failed to reconnect."));
    }
  }
  else {
#if 0
    if (mqttLWTPeriod + MQTT_LWT_PERIOD < millis()) {
      MQTTclient_must_send_LWT_connected = true;
      mqttLWTPeriod = millis();
    }
#endif
    if (MQTTclient_must_send_LWT_connected) {
      if (mqtt.publish(mqttLWTTopic, mqttLWTonline, true)) {
        MQTTclient_must_send_LWT_connected = false;
      }
    }
    mqtt.loop();
  }
}

bool mqttConnect() {
  mqtt.setKeepAlive(10);
  if (mqtt.connect(mqttClientName, 
                   mqtt_user, 
                   mqtt_password,
                   mqttLWTTopic,
                   0,
                   true,
                   mqttLWToffline)) {
    Serial.print(F("MQTT connected: "));
    Serial.println(mqtt_server);
    Serial.println(mqttClientName);
    //dsc.resetStatus();
    mqtt.subscribe(mqttSubscribeTopic);
    if (!mqtt.publish(mqttLWTTopic, mqttLWTonline, true)) MQTTclient_must_send_LWT_connected = true;
#if 0
    mqttLWTPeriod = millis();
#endif
  }
  else {
    Serial.print(F("MQTT connection failed: "));
    Serial.println(mqtt_server);
  }
  return mqtt.connected();
}
#endif

void handleRoot() {                          // When URI / is requested, send a web page with a button to toggle the LED
  String s = "";

  s += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">";

  s +=  "<style> \
          body { \
            background-color: #505050; \
            text-align: center; \
            color: white; \
            font-family: Arial, Helvetica, sans-serif; \
            max-width: 150px \
            margin: auto; \
          } \
        </style>";

  s +=  "<h1>WiFi DSC";
#if defined(USE_MQTT)
  if (mqttEnabled) {
    s +=  " MQTT";
  }
#endif
#if defined(USE_TELEGRAM)
  if (telegramEnabled) {
    s +=  " TeleGram";
  }
#endif
  s +=  " Bridge</h1> \
        <p>Service Stopped<br />Save and Reboot<br />MANDATORY!</p> \
        <form action=\"/SaveParams\" method=\"POST\"> \
          <p> \
            <label for=\"ssid\">SSID</label> \
            <br /> \
            <input name=\"ssid\" type=\"text\" value=\"";
  s +=      wifiSSID;
  s +=      "\" /> \
            <br /> \
            <br /> \
            <label for=\"psk\">PSK</label> \
            <br /> \
            <input name=\"psk\" type=\"text\" value=\"";
  s +=      wifiPassword;
  s +=      "\" /> \
            <br />";

  s +=      "<hr width=\"200px\" />";

  for (int idx = 0; idx < COMMON_NUMEL(_config); idx++) {
    s +=      "<label for=\"";
    s +=      _config[idx].webFormName;
    s +=      "\">";
    s +=      _config[idx].webFormText;
    s +=      "</label> \
              <br /> \
              <input name=\"";
    s +=      _config[idx].webFormName;
    s +=      "\" type=\"text\" value=\"";
    s +=      _config[idx].val;
    s +=      "\" /> \
              <br /> \
              <br />";
  }

  s +=  "</p> \
        <p> \
          <br /> \
          <button type=\"submit\" value=\"Submit\">Save and Reboot</button> \
        </p> \
      </form>";

  server.send(200, "text/html", s);
}

void handleSaveParams() {
  if (!server.hasArg("ssid")
    || !server.hasArg("psk") 
      || server.arg("ssid") == NULL
    || server.arg("psk") == NULL) 
  {
    server.send(400, "text/plain", "400: Invalid Request");
    return;
  }
  else
  {
    saveResult = true;
    
    // Saving settings
    //read updated parameters
    String tempStr = "";

    for (int idx = 0; idx < COMMON_NUMEL(_config); idx++) {
      size_t sz = _config[idx].len;
      tempStr = server.arg(_config[idx].webFormName);
      tempStr.toCharArray(_config[idx].val, sz);
    }

    //save the custom parameters to FS
    Serial.println("saving config");
    DynamicJsonDocument json(1024);

    for (int idx = 0; idx < COMMON_NUMEL(_config); idx++) {
      json[_config[idx].name] = _config[idx].val;
    }

    dsc.stop();

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
      saveResult = false;
    } else {
      serializeJson(json, Serial);
      serializeJson(json, configFile);
    
      configFile.close();
      //end save
    }

    if (saveResult) {
      server.send(200, "text/html", "<h1>Settings updated!</h1><p>Resetting device...</p>");
    } else {
      server.send(200, "text/html", "<h1>Settings NOT saved!</h1>");
    }
  }
}

#if defined(USE_MQTT)
// Publishes HomeKit target and current states with partition numbers
void publishState(const char* sourceTopic, byte partition, const char* targetSuffix, const char* currentState) {
  char publishTopic[strlen(sourceTopic) + 2];
  char partitionNumber[2];

  if (!mqttEnabled) return;

  // Appends the sourceTopic with the partition number
  itoa(partition + 1, partitionNumber, 10);
  strcpy(publishTopic, sourceTopic);
  strcat(publishTopic, partitionNumber);

  if (targetSuffix != 0) {

    // Prepends the targetSuffix with the partition number
    char targetState[strlen(targetSuffix) + 2];
    strcpy(targetState, partitionNumber);
    strcat(targetState, targetSuffix);

    // Publishes the target state
    mqtt.publish(publishTopic, targetState, true);
  }

  // Publishes the current state
  if (currentState != 0) {
    mqtt.publish(publishTopic, currentState, true);
  }
}
#endif

#if defined(USE_TELEGRAM)
int handleOTA(int i) {
  int numNewMessages = 0;
  if (telegramBot.messages[i].type == "message")
    {
      if (telegramBot.messages[i].hasDocument == true)
      {
        httpUpdate.rebootOnUpdate(false);
        t_httpUpdate_return ret = (t_httpUpdate_return)3;
        if (telegramBot.messages[i].file_caption == "write spiffs")
        {
          numNewMessages = 1;
          size_t spiffsFreeSize = SPIFFS.totalBytes() - SPIFFS.usedBytes();
          if (telegramBot.messages[i].file_size < spiffsFreeSize)
          {
            telegramBot.sendMessage(telegramBot.messages[i].chat_id, "File downloading.", "");
            HTTPClient http;
            if (http.begin(wifiClientSecured, telegramBot.messages[i].file_path))
            {
              int code = http.GET();
              if (code == HTTP_CODE_OK)
              {
                int total = http.getSize();
                int len = total;
                uint8_t buff[128] = {0};
                WiFiClient *tcp = http.getStreamPtr();
                if (SPIFFS.exists("/" + telegramBot.messages[i].file_name))
                  SPIFFS.remove(("/" + telegramBot.messages[i].file_name));
                File fl = SPIFFS.open("/" + telegramBot.messages[i].file_name, FILE_WRITE);
                if (!fl)
                {
                  telegramBot.sendMessage(telegramBot.messages[i].chat_id, "File open error.", "");
                }
                else
                {
                  while (http.connected() && (len > 0 || len == -1))
                  {
                    size_t size_available = tcp->available();
                    Serial.print("%");
                    Serial.println(100 - ((len * 100) / total));
                    if (size_available)
                    {
                      int c = tcp->readBytes(buff, ((size_available > sizeof(buff)) ? sizeof(buff) : size_available));
                      fl.write(buff, c);
                      if (len > 0)
                      {
                        len -= c;
                      }
                    }
                    delay(1);
                  }
                  fl.close();
                  if (len == 0)
                    telegramBot.sendMessage(telegramBot.messages[i].chat_id, "Success.", "");
                  else
                    telegramBot.sendMessage(telegramBot.messages[i].chat_id, "Error.", "");
                }
              }
              http.end();
            }
          }
          else
          {
            telegramBot.sendMessage(telegramBot.messages[i].chat_id, "SPIFFS size to low (" + String(spiffsFreeSize) + ") needed: " + String(telegramBot.messages[i].file_size), "");
          }
        }
        else
        {
          if (telegramBot.messages[i].file_caption == "update firmware")
          {
            wdt_disable();
            numNewMessages = 1;
            telegramBot.sendMessage(telegramBot.messages[i].chat_id, "Firmware writing...", "");
            ret = httpUpdate.update(wifiClientSecured, telegramBot.messages[i].file_path);
          }
          if (telegramBot.messages[i].file_caption == "update spiffs")
          {
            numNewMessages = 1;
            telegramBot.sendMessage(telegramBot.messages[i].chat_id, "SPIFFS writing...", "");
            ret = httpUpdate.updateSpiffs(wifiClientSecured, telegramBot.messages[i].file_path);
          }
          switch (ret)
          {
          case HTTP_UPDATE_FAILED:
            telegramBot.sendMessage(telegramBot.messages[i].chat_id, "HTTP_UPDATE_FAILED Error (" + String(httpUpdate.getLastError()) + "): " + httpUpdate.getLastErrorString(), "");
            break;

          case HTTP_UPDATE_NO_UPDATES:
            telegramBot.sendMessage(telegramBot.messages[i].chat_id, "HTTP_UPDATE_NO_UPDATES", "");
            break;

          case HTTP_UPDATE_OK:
            telegramBot.sendMessage(telegramBot.messages[i].chat_id, "UPDATE OK\nRestarting...", "");
            numNewMessages = telegramBot.getUpdates(telegramBot.last_message_received + 1);
            ESP.restart();
            break;
          default:
            break;
          }
        }
      }
      if (telegramBot.messages[i].text == "/dir")
      {
        numNewMessages = 1;
        File root = SPIFFS.open("/");
        File file = root.openNextFile();
        String files = "";
        while (file)
        {
          files += String(file.name()) + " " + String(file.size()) + "B\n";
          file = root.openNextFile();
        }
        telegramBot.sendMessage(telegramBot.messages[i].chat_id, files, "");
      }
      else if (telegramBot.messages[i].text == "/formattt")
      {
        numNewMessages = 1;
        bool res = SPIFFS.format();
        if (!res)
          telegramBot.sendMessage(telegramBot.messages[i].chat_id, "Format unsuccessful", "");
        else
          telegramBot.sendMessage(telegramBot.messages[i].chat_id, "SPIFFS formatted.", "");
      }
      else if (telegramBot.messages[i].text.startsWith("/read_spiffs")) // "/read_spiffs <filename>")
      {
          numNewMessages = 1;

          String rcvText = telegramBot.messages[i].text;
          String fileName = "";
          
          char buf[rcvText.length() + 1];
          rcvText.toCharArray(buf, sizeof(buf));
          char *p = buf;
          char *str;
          while ((str = strtok_r(p, " ", &p)) != NULL) { // delimiter is the space
              fileName = str;
              //telegramBot.sendMessage(telegramBot.messages[i].chat_id, str, "");
          }
          
          telegramBot.sendMessage(telegramBot.messages[i].chat_id, "Getting file " + fileName + "... ", "");

          if (fileName != "") {
              if (SPIFFS.exists(fileName)) {
                File fl = SPIFFS.open(fileName, FILE_READ);
                if (!fl)
                {
                  telegramBot.sendMessage(telegramBot.messages[i].chat_id, "File open error!", "");
                } else {
                  size_t size = fl.size();

                  if (size <= 4096) {
                    // Allocate a buffer to store contents of the file.
                    std::unique_ptr<char[]> buf(new char[size]);
                    fl.readBytes(buf.get(), size);

                    // send file here
                    //telegramBot.sendChatAction(telegramBot.messages[i].chat_id, "upload_document");
                    String ss = String(buf.get()).substring(0, size);
                    telegramBot.sendMessage(telegramBot.messages[i].chat_id, ss);
                    //Serial.println(ss);
                  } else {
                    telegramBot.sendMessage(telegramBot.messages[i].chat_id, " File size exceed 4096 bytes = " + size, "");
                  }
                  
                  fl.close();
                  telegramBot.sendMessage(telegramBot.messages[i].chat_id, " OK", "");
                }
              } else {
                telegramBot.sendMessage(telegramBot.messages[i].chat_id, " File Not Found!", "");
              }
          }
          else {
              telegramBot.sendMessage(telegramBot.messages[i].chat_id, " fileName ERROR!", "");
          }
      }
    }

  return numNewMessages;
}

void handleTelegram(byte telegramMessages) {
  static byte partition = 0;
  static byte oldPartition = 0;

  for (byte i = 0; i < telegramMessages; i++) {

    String chat_id = telegramBot.messages[i].chat_id;
    String text = telegramBot.messages[i].text;
    String from_name = telegramBot.messages[i].from_name;
    if (from_name == "")
      from_name = "Guest";

    String tgUserId = String(telegram_chat_id);
    if (tgUserId != "" && tgUserId != chat_id) continue;  // don't process requests from unknown sender

    if (0 != handleOTA(i)) continue; // FW/SPIFFS things handler

    // ============================= FOR TESTING THINGS =========================

    if (text == "/chat_id") {
      telegramBot.sendMessage(chat_id, chat_id + " " + telegramBot.messages[i].type);
      continue;
    } 
    else if (text == "/start")
    {
      String welcome = "Welcome, " + from_name + "!\n\n";
      welcome += "Usage:\n";
      welcome += "/chat_id : get ChatID\n";
      telegramBot.sendMessage(chat_id, welcome);
      continue;
    }

    // ============================= DSC THINGS =========================

    // answer ONLY to know UserID
    if (tgUserId == "") continue;

    if (text == "/reset") {
      telegramBot.sendMessage(telegramBot.messages[i].chat_id, "Restarting...");
      telegramBot.getUpdates(telegramBot.last_message_received + 1);
      ESP.restart();
    } 
    else if (text == "/send_test_action")
    {
      telegramBot.sendChatAction(chat_id, "typing");
      delay(3000);
      telegramBot.sendMessage(chat_id, "Did you see the action message?");

      // You can't use own message, just choose from one of bellow

      //typing for text messages
      //upload_photo for photos
      //record_video or upload_video for videos
      //record_audio or upload_audio for audio files
      //upload_document for general files
      //find_location for location data

      //more info here - https://core.telegram.org/bots/api#sendchataction
    }
    else if (text == "/status") 
    {
      String s = "";

      // ======================================

      s += "Partition status:\n";

      for (byte partition = 0; partition < PARTITION_COUNT; partition++) {
        s += "Partition ";
        s += partition + 1;
        s += " ";
        if (dsc.disabled[partition]) {
          s += "Disabled\n";
          continue;
        }

        // Ready
        if (dsc.ready[partition]) {
          s += "READY ";
        }

        // Exit delay in progress
        if (dsc.exitDelay[partition]) {
          s += "Exit Delay in progress ";
          switch (dsc.exitState[partition]) {
            case DSC_EXIT_STAY: {
              s += "Stay\n";
              break;
            }
            case DSC_EXIT_AWAY: {
              s += "Away\n";
              break;
            }
            case DSC_EXIT_NO_ENTRY_DELAY: {
              s += "No Exit Delay\n";
              break;
            }
          }
        }
        // // Disarmed during exit delay
        // else if (!dsc.armed[partition]) {
        //   s += "Disarmed\n";
        // }

        if (dsc.alarm[partition]) {
          s += "Alarm!\n";
        }
        if (dsc.fire[partition]) {
          s += "Fire!\n";
        }


        if (dsc.armed[partition]) {
          // Night armed away
          if (dsc.armedAway[partition] && dsc.noEntryDelay[partition]) {
            s += "Night Arm\n";
          }
          // Armed away
          else if (dsc.armedAway[partition]) {
            s += "Away Arm\n";
          }
          // Night armed stay
          else if (dsc.armedStay[partition] && dsc.noEntryDelay[partition]) {
            s += "Night Stay Arm\n";
          }
          // Armed stay
          else if (dsc.armedStay[partition]) {
            s += "Stay Arm\n";
          }
        }
        // Disarmed
        else {
          s += "Disarmed\n";
        }
      }

      // ======================================

      s += "---\n";
      s += "Zone status:\n";

      byte zonesTouchedCount = 0;

      for (byte zoneGroup = 0; zoneGroup < dscZones; zoneGroup++) {
        for (byte zoneBit = 0; zoneBit < 8; zoneBit++) {
          char zone[3];
          itoa(zoneBit + 1 + (zoneGroup * 8), zone, 10);

          bool zoneTouched = false;
          if (bitRead(dsc.openZones[zoneGroup], zoneBit)) {  
            s += zone;
            s += ": opened";
            zoneTouched = true;
            zonesTouchedCount++;
          }
          // else {
          //   s += "closed";
          // }

          if (bitRead(dsc.alarmZones[zoneGroup], zoneBit)) {
            s += "ALARM!\n";
          } else if (zoneTouched) {
            s += "\n";
          }
        }
      }

      if (0 == zonesTouchedCount) {
        s += "All closed\n";
      }

      // ======================================

      s += "---\n";
      s += "Keybus ";
      if (dsc.keybusConnected) {
        s += "connected";
      } else {
        s += "connected";
      }
      s += "\n";

      // s += "Panel version: ";
      // s += dsc.panelVersion;
      // s += "\n";

      // ======================================
      s += "---\n";

      if (!dsc.trouble) {
        s += "No Troubles\n";
      }
      if (dsc.powerTrouble) {
        s += "Power Trouble\n";
      }
      if (dsc.batteryTrouble) {
        s += "Battery Trouble\n";
      }

      if (dsc.keypadFireAlarm) {
        s += "Keypad Fire Alarm!\n";
      }
      if (dsc.keypadAuxAlarm) {
        s += "Keypad Aux Alarm!\n";
      }
      if (dsc.keypadPanicAlarm) {
        s += "Keypad Panic Alarm!\n";
      }

      // ======================================
      
      s += "---\n";
      s += "PGMs:\n";

      byte _pgmGroups = (PGM_COUNT > 8) ? 2 : 1;
      byte _pgmCount = PGM_COUNT;

      for (byte pgmGroup = 0; pgmGroup < _pgmGroups; pgmGroup++) {
        for (byte pgmBit = 0; pgmBit < _pgmCount; pgmBit++) {
          char pgm[3];
          itoa(pgmBit + 1 + (pgmGroup * 8), pgm, 10);
          s += pgm;
          s += ": ";

          if (bitRead(dsc.pgmOutputs[pgmGroup], pgmBit)) {
            s += "on\n";
          } else {
            s += "off\n";
          }
        }
      }

      //
      telegramBot.sendMessage(chat_id, s);
    }
    else if (text == "/version") 
    {
      String s = "";
      s += "FW version: ";
      s += version;
      s += "\n";
      s += "WiFi SSID: ";
      s += WiFi.SSID();
      s += "\n";
      s += "WiFi PSK: ";
      s += WiFi.psk();
      s += "\n";
      s += "WiFi RSSI: ";
      s += WiFi.RSSI();
      s += "\n";
      s += "IP: ";
      s += WiFi.localIP().toString();

      s += "\n";
      struct tm timeInfo;
      gmtime_r(&startTime, &timeInfo);
      char strftime_buf[64];
      strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeInfo);
      s += "Start time (UTC): ";
      s += "[";
      s += startTime;
      s += "] ";
      s += String(strftime_buf);

      telegramBot.sendMessage(chat_id, s);
    }
    else if (text == "/listconfig") 
    {
      String s = "";

      s += "Current configuration:\n";

      for (int idx = 0; idx < COMMON_NUMEL(_config); idx++) {
        s += _config[idx].name;
        s += " = [";
        s += String(_config[idx].val);
        s += "]\n";
      }

      telegramBot.sendMessage(chat_id, s);
    }
    else if (text.startsWith("/getconfig"))
    {
      String rcvText = text;
      String subCommand = "";
      
      char buf[rcvText.length() + 1];
      rcvText.toCharArray(buf, sizeof(buf));
      char *p = buf;
      char *str;
      while ((str = strtok_r(p, " ", &p)) != NULL) { // delimiter is the space
          subCommand = str;
      }

      String s = subCommand;
      s += " = [";

      bool idFound = false;
      for (int idx = 0; idx < COMMON_NUMEL(_config); idx++) {
        if (_config[idx].name == subCommand) {
          s += String(_config[idx].val);
          idFound = true;
          break;
        }
      }

      if (!idFound) s += "n/a";

      s += "]";

      telegramBot.sendMessage(chat_id, s);
    }
    else if (text.startsWith("/setconfig"))
    {
      String rcvText = text;
      String subCommand[3];

      int subCommandIdx = 0;
      char buf[rcvText.length() + 1];
      rcvText.toCharArray(buf, sizeof(buf));
      char *p = buf;
      char *str;
      while ((str = strtok_r(p, " ", &p)) != NULL) { // delimiter is the space
        subCommand[subCommandIdx++] = str;
        if (subCommandIdx >= (COMMON_NUMEL(subCommand))) break;
      }
      if (subCommandIdx != COMMON_NUMEL(subCommand)) {
        telegramBot.sendMessage(chat_id, "Wrong parameters count!");
        return;
      }

      String paramId = subCommand[1];
      String paramVal = subCommand[2];

      String s = paramId + " ";

      bool idFound = false;
      for (int idx = 0; idx < COMMON_NUMEL(_config); idx++) {
        if (_config[idx].name == paramId) {
          size_t sz = _config[idx].len - 1;
          if (paramVal == "empty") {
            strncpy(_config[idx].val, "", sz);
          } else {
            strncpy(_config[idx].val, paramVal.c_str(), sz);
          }
          if (paramVal.length() > sz) {
            s += "size too big, truncated to ";
            s += sz;
            s += " ";
            paramVal = paramVal.substring(0, sz);
          }
          idFound = true;
          break;
        }
      }

      if (!idFound)
      {
        s += "n/a";
      }
      else
      {
        s += "-> " + paramVal;

        //save the parameter to FS
        Serial.println("saving config");
        DynamicJsonDocument json(1024);

        for (int idx = 0; idx < COMMON_NUMEL(_config); idx++) {
          json[_config[idx].name] = _config[idx].val;
        }

        bool sResult = true;

        File configFile = SPIFFS.open("/config.json", "w");
        if (!configFile) {
          Serial.println("failed to open config file for writing");
          sResult = false;
        } else {
          serializeJson(json, Serial);
          serializeJson(json, configFile);
        
          configFile.close();
          //end save
        }

        if (sResult) {
          s += " Saved OK";
        } else {
          s += " Saving Error!";
        }
      }

      telegramBot.sendMessage(chat_id, s);
    }
    else if (text == "/wdt") 
    {
      telegramBot.sendMessage(telegramBot.messages[i].chat_id, "Awaiting WDT to restart...");
      telegramBot.getUpdates(telegramBot.last_message_received + 1);
      wdt_enable(WDT_TMO);
      while(1);
    }
    else if (text == "/wdtoff")
    {
      telegramBot.sendMessage(telegramBot.messages[i].chat_id, "WDT disabled...");
      wdt_disable();
    }
    else if (text == "/help")
    {
      String s = "Welcome, " + from_name + "!\n\n";
      s += "Usage:\n";
      s += "/start\n";
      s += "/help\n";
      s += "/send_test_action : to send test chat action message\n";
      s += "/chat_id  : get ChatID\n";
      s += "---\n";
      s += "/X : X - partition number to control\n";
      s += "/disarm\n";
      s += "/armstay\n";
      s += "/armaway\n";
      s += "/armnight\n";
      s += "/status\n";
      s += "/version\n";
      s += "/wdt\n";
      s += "/wdtoff\n";
      s += "/cmd ABCD, ABCD - key sequence to send to panel\n";
      s += "---\n";
      s += "/listconfig\n";
      s += "/getconfig <param_id>\n";
      s += "/setconfig <param_id> <new_value>, <new_value> can be word 'empty'\n";
      s += "---\n";
      s += "/reset\n";
      s += "/dir\n";
      s += "/format tt\n";
      s += "/read_spiffs <filename>\n";
      s += "File Message Caption can be:\n";
      s += "write spiffs\n";
      s += "update firmware\n";
      s += "update spiffs\n";
      telegramBot.sendMessage(chat_id, s);
    }
    // Checks if a partition number 1-8 has been sent and sets the partition
    else if (telegramBot.messages[i].text[1] >= 0x31 && telegramBot.messages[i].text[1] <= 0x38) {
      oldPartition = partition;
      partition = telegramBot.messages[i].text[1] - 49;
      char messageContent[17];
      if (dsc.status[partition] != 0xC7) {  // partition available
        strcpy(messageContent, "Set: Partition ");
        appendPartition(partition, messageContent);  // Appends the message with the partition number
      } else {
        strcpy(messageContent, "ERR: Partition ");
        //sprintf(messageContent, "ERR: Partition %d switching back to %d", partition, oldPartition);
        appendPartition(partition, messageContent);  // Appends the message with the partition number
        partition = oldPartition;
      }
      sendMessage(messageContent);
    }
    // Resets status if attempting to change the armed mode while armed or not ready
    else if (telegramBot.messages[i].text != "/disarm" && !dsc.ready[partition]) {
      dsc.armedChanged[partition] = true;
      dsc.statusChanged = true;
      return;
    }
    // Arm stay
    else if (telegramBot.messages[i].text == "/armstay" && !dsc.armed[partition] && !dsc.exitDelay[partition]) {
      dsc.writePartition = partition + 1;  // Sets writes to the partition number
      dsc.write('s');
    }
    // Arm away
    else if (telegramBot.messages[i].text == "/armaway" && !dsc.armed[partition] && !dsc.exitDelay[partition]) {
      dsc.writePartition = partition + 1;  // Sets writes to the partition number
      dsc.write('w');
    }
    // Arm night
    else if (telegramBot.messages[i].text == "/armnight" && !dsc.armed[partition] && !dsc.exitDelay[partition]) {
      dsc.writePartition = partition + 1;  // Sets writes to the partition number
      dsc.write('n');
    }
    // Disarm
    else if (telegramBot.messages[i].text == "/disarm" && (dsc.armed[partition] || dsc.exitDelay[partition] || dsc.alarm[partition])) {
      dsc.writePartition = partition + 1;  // Sets writes to the partition number
      dsc.write(dsc_access_code);
    }
    else if (telegramBot.messages[i].text.startsWith("/cmd")) {
      String rcvText = telegramBot.messages[i].text;
      String cmd = "";
      
      // Convert from String Object to String.
      char buf[rcvText.length() + 1];
      rcvText.toCharArray(buf, sizeof(buf));
      char *p = buf;
      char *str;
      while ((str = strtok_r(p, " ", &p)) != NULL) { // delimiter is the space
          cmd = str;
          //telegramBot.sendMessage(telegramBot.messages[i].chat_id, str, "");
      }
      
      telegramBot.sendMessage(telegramBot.messages[i].chat_id, "Executing command " + cmd + "... ", "");
      strcpy(keybuf, cmd.c_str());
      dsc.write(keybuf);
    }
  }
}


bool sendMessage(const char* messageContent) {
  wifiClientSecured.setHandshakeTimeout(30);  // Workaround for https://github.com/espressif/arduino-esp32/issues/6165
  String tgUID = String(telegram_chat_id);
  String tgBT = String(telegram_bot_token);
  if (tgUID == "" || tgBT == "") return false;
  byte messageLength = strlen(telegram_msg_prefix) + strlen(messageContent) + 1;
  char message[messageLength];
  strcpy(message, telegram_msg_prefix);
  strcat(message, messageContent);
  if (telegramBot.sendMessage(telegram_chat_id, message, "")) return true;
  else return false;
}


void appendPartition(byte sourceNumber, char* message) {
  String tgUID = String(telegram_chat_id);
  String tgBT = String(telegram_bot_token);
  if (tgUID == "" || tgBT == "") return;
  char partitionNumber[2];
  itoa(sourceNumber + 1, partitionNumber, 10);
  strcat(message, partitionNumber);
}

void bot_setup()
{
  const String commands = F("["
                            "{\"command\":\"start\", \"description\":\"Message sent when you open a chat with a bot\"},"
                            "{\"command\":\"help\",  \"description\":\"Get bot usage help\"},"
                            "{\"command\":\"send_test_action\", \"description\":\"FOR TESTING\"},"
                            "{\"command\":\"chat_id\", \"description\":\"Answer current ChatID\"},"
                            "{\"command\":\"X\", \"description\":\"Set Partition X\"},"
                            "{\"command\":\"disarm\", \"description\":\"Disarm\"},"
                            "{\"command\":\"armstay\", \"description\":\"Arm STAY\"},"
                            "{\"command\":\"armaway\", \"description\":\"Arm AWAY\"},"
                            "{\"command\":\"armnight\", \"description\":\"Arm NIGHT\"}" // no comma on last command
                            "]");
  telegramBot.setMyCommands(commands);
  //bot.sendMessage("25235518", "Hola amigo!", "Markdown");
}
#endif
