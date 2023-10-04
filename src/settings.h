#ifndef SETTINGS_H
#define SETTINGS_H

const char* version = "1.8.2";

#define USE_MQTT
#define USE_TELEGRAM

#define WDT_TMO 60000

#define MQTT_SERVER_LEN         40
#define MQTT_PORT_LEN           6
#define MQTT_USER_LEN           8
#define MQTT_PASSWORD_LEN       8

#define TELEGRAM_CHAT_ID_LEN    32
#define TELEGRAM_BOT_TOKEN_LEN  64
#define TELEGRAM_MSG_PREFIX_LEN 40

#define DSC_ACCESS_CODE_LEN     5

#define PGM_COUNT               4
#define PARTITION_COUNT         2

#endif
