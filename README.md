# ESP32 DSC Security Panel MQTT and Telegram Gateway
**For use with ESP32-WROOM DevKit Board**

# Features
- Telegram Bot for two-way interaction with security system. Send statuses, alarms, troubles and listen for various commands (described below)
- MQTT publishing/subscribing to integrate with various Home Automation systems (like Apple HomeKit, using HomeBridge and etc.)
- Update via OTA
- Update via Telegram
- Fully configurable via WEB interface and/or Telegram Bot commands (except OTA credentials)
- Power-up, then power-down in 10 seconds and power-up again will initiate WiFi Manager and device will be accessible as WiFi Access Point, with captive portan enabled (192.168.4.1) to connect device to WiFi network. Same behavior will be enabled in device is "new" and don't have any WiFi credentials stored.

# Connections and Wiring
```
       DSC Aux(+) --- 5v voltage regulator --- esp32 development board 5v pin
       DSC Aux(-) --- esp32 Ground
                                          +--- dscClockPin (esp32: 18)
       DSC Yellow --- 33k ohm resistor ---|
                                          +--- 10k ohm resistor --- Ground
 
                                          +--- dscReadPin (esp32: 19)
       DSC Green ---- 33k ohm resistor ---|
                                          +--- 10k ohm resistor --- Ground
 
   Virtual keypad (optional):
       DSC Green ---- NPN collector --\
                                       |-- NPN base --- 1k ohm resistor --- dscWritePin (esp32: 21)
             Ground --- NPN emitter --/
 
   Virtual keypad uses an NPN transistor to pull the data line low - most small signal NPN transistors should
   be suitable, for example:
    -- 2N3904
    -- BC547, BC548, BC549
```

# Build and Flash
- Two `define` settings are in `src/settings.h`. `USE_MQTT` and `USE_TELEGRAM`. You can uncomment one you need or both at same time.
- Build in VS Code with PlatformIO extension
- Upload using native USB of DevKit
- Further Upload via OTA supported, just edit according `platformio.ini` lines with your device IP address and OTA password
- Further Upload via Telegram supported, send firmware file to Telegram bot with subject `update firmware`

# Initial preparation
- Power up device and connect to it WiFi Access Point, go to captive portal (if it not opens automatically) 192.168.4.1 and connect device to WiFi Network.
- Go to device IP http://your_device_ip and make configuration changes
## Telegram
* Send a message to BotFather: https://t.me/botfather
* Create a new bot through BotFather: `/newbot`
* Copy the bot token to the configuration `Telegram Bot Token`
* Send a message to the newly created bot to start a conversation
* Send a message to @myidbot: https://telegram.me/myidbot
* Get your user ID: /getid
* Copy the user ID to the configuration `Telegram Chat ID`
* Enter custom `Telegram message prefix` (if you prefer)
- NOTE: GETTING CHAT GROUP ID: https://api.telegram.org/botXXX:YYY/getUpdates
## MQTT
- Enter MQTT broker IP, Port, User and Password (if required)
## Press `Save and Reboot`

# Usage
## Telegram 
- To communicate with Telegram Bot `Telegram Chat ID` should present. Device will check if it is valid user request and answer only if it is.
- Commands `/chat_id` and `/start` are available for ALL users, as they are used only for initial setup or testing and can't control Security Panel.
- /help - shows list of all supported commands
## MQTT Topics
| Topic Name | Description |
| --- | --- |
| dsc/Get/Partition | Sends armed and alarm status per partition: dsc/Get/Partition1 ... dsc/Get/Partition8 |
| dsc/Get/Zone | Sends zone status per zone: dsc/Get/Zone1 ... dsc/Get/Zone64 |
| dsc/Get/Fire | Sends fire status per partition: dsc/Get/Fire1 ... dsc/Get/Fire8 |
| dsc/Get/PGM | Sends PGM status per PGM: dsc/Get/PGM1 ... dsc/Get/PGM14 |
| dsc/Set | Receives messages to write to the panel |
| dsc/status/LWT | LWT Status Topic |

# References
All libraries used are copyrighted by owners
