

#pragma once
#include "esp_log.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

#define botBaseUrl "https://rtc.volcengineapi.com?Action=StartVoiceChat&Version=2024-12-01"
// #define API_KEY1 "17815d46-db77-473a-a9f0-376a65c5bc5a"  // 将此替换为你的API密钥

bool startRTCBot(void);
bool updateRTCBot(void);
bool closeRTCBot(void);

extern char * global_appid ;
extern char * global_token ;
extern char * global_roomid;
extern char * global_uid ;

#ifdef __cplusplus
}

#endif