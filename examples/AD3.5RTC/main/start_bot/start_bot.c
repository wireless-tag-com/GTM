/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2018 <ESPRESSIF SYSTEMS (SHANGHAI) PTE LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "start_bot.h"
#include "cJSON.h"
#include "config.h"
#if START_BOT
static const char *TAG = "start_bot";
static char response_data[1048];
static int recived_len;
// http客户端的事件处理回调函数
static esp_err_t http_client_event_handler(esp_http_client_event_t *evt)
{
    static int recived_len;

    switch (evt->event_id)
    {
    case HTTP_EVENT_ON_CONNECTED:
        recived_len = 0;
        break;
    case HTTP_EVENT_ON_DATA:
        if (evt->user_data)
        {
            memcpy(evt->user_data + recived_len, evt->data, evt->data_len); // 将分片的每一片数据都复制到user_data
            recived_len += evt->data_len;                                   // 累计偏移更新
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        recived_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        recived_len = 0;
        break;
    case HTTP_EVENT_ERROR:
        recived_len = 0;
        break;
    default:
        break;
    }

    return ESP_OK;
}

bool startRTCBot(void)
{
    // 创建根对象
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "deviceId", DEVICE_ID);
    cJSON_AddStringToObject(root, "productId", PRODUCT_ID);
    char *request_params = cJSON_PrintUnformatted(root);
    if (!request_params)
    {
        ESP_LOGE(TAG, "Failed to create request parameters");
        return 0;
    }
    ESP_LOGW(TAG, "Chat request: %s", request_params);
    cJSON_Delete(root);
    esp_http_client_config_t http_config = {
        .url = TEST_SERVER_URL,
        .event_handler = http_client_event_handler,
        .user_data = response_data,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(request_params);
        return NULL;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, request_params, strlen(request_params));
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {

        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %" PRId64,
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
        ESP_LOGI(TAG, "Chat Response Data: %s", response_data);

        cJSON *json = cJSON_Parse(response_data);
        #if 0
        if(json)
        {
            cJSON *token = cJSON_GetObjectItem(json, "token");

            if (token)
            {
                global_token = strdup(token->valuestring);
                ESP_LOGI(TAG, "token->: %s", global_token);
            }
            else 
            ESP_LOGI(TAG, "token_error");
            
            cJSON *appid = cJSON_GetObjectItem(json, "appid");
            if (appid)
            {
                global_appid = strdup(appid->valuestring);
                ESP_LOGI(TAG, "appid->: %s", global_appid);
            }
            cJSON *roomid = cJSON_GetObjectItem(json, "room_id");
            if (roomid)
            {
                global_roomid = strdup(roomid->valuestring);
                ESP_LOGI(TAG, "roomid->: %s", global_roomid);
            }

            cJSON *uid = cJSON_GetObjectItem(json, "uid");
            if (uid)
            {
                global_uid = strdup(uid->valuestring);
                ESP_LOGI(TAG, "uid->: %s", global_uid);
            }
#else
        // 提取 data 中的信息
        cJSON *data = cJSON_GetObjectItemCaseSensitive(json, "data");
        if (cJSON_IsObject(data))
        {
            cJSON *innerData = cJSON_GetObjectItemCaseSensitive(data, "data");
            if (cJSON_IsObject(innerData))
            {
                cJSON *room_id = cJSON_GetObjectItemCaseSensitive(innerData, "room_id");
                if (cJSON_IsString(room_id) && (room_id->valuestring != NULL))
                {
                    ESP_LOGI("JSON", "room_id: %s", room_id->valuestring);
                    global_roomid = strdup(room_id->valuestring);
                }
                cJSON *uid = cJSON_GetObjectItemCaseSensitive(innerData, "uid");
                if (cJSON_IsString(uid) && (uid->valuestring != NULL))
                {
                    ESP_LOGI("JSON", "uid: %s", uid->valuestring);
                    global_uid = strdup(uid->valuestring);
                }
                cJSON *app_id = cJSON_GetObjectItemCaseSensitive(innerData, "app_id");
                if (cJSON_IsString(app_id) && (app_id->valuestring != NULL))
                {
                    ESP_LOGI("JSON", "app_id: %s", app_id->valuestring);
                    global_appid = strdup(app_id->valuestring);
                }
                cJSON *token = cJSON_GetObjectItemCaseSensitive(innerData, "token");
                if (cJSON_IsString(token) && (token->valuestring != NULL))
                {
                    ESP_LOGI("JSON", "token: %s", token->valuestring);
                    global_token = strdup(token->valuestring);
                }
            }

#endif

            cJSON_Delete(json);
        }
    }
    else
    {
        ESP_LOGE(TAG, "Chat HTTP Post failed: %s", esp_err_to_name(err));
        return 0;
    }

    free(request_params);
    esp_http_client_cleanup(client);
    return 1;
}
#endif