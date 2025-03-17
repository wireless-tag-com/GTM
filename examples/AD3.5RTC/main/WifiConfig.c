
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "ui_code/ui.h" // LVGL UI 代码

#define TAG "WIFI_CONFIG"
extern uint8_t exit_rtc_task;
// LVGL 相关对象定义
static lv_obj_t *ssid_textarea;
static lv_obj_t *password_textarea;
static lv_obj_t *connect_btn;
static lv_obj_t *status_label;
static lv_obj_t *kb;

// WIFI 事件组
 EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;

// WIFI 事件处理函数
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
      
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
        lv_label_set_text(ui_Label5, "正在联接...");
        ESP_LOGI(TAG, "retry to connect to the AP");
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        // lv_label_set_text(status_label, "Connected!");
        lv_label_set_text(ui_Label5, "已经联网");
        lv_label_set_text(ui_Label2, " 接通 ");
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        lv_disp_load_scr(ui_Screen1);
    }
}

// 连接到 WIFI 函数
static void connect_to_wifi(const char *ssid, const char *password)
{
   // esp_netif_init();
    s_wifi_event_group = xEventGroupCreate();
   // esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &event_handler,
                                        NULL,
                                        NULL);
    esp_event_handler_instance_register(IP_EVENT,
                                        IP_EVENT_STA_GOT_IP,
                                        &event_handler,
                                        NULL,
                                        NULL);

    wifi_config_t wifi_config = {
        .sta = {
        //     .ssid = "QMYD2.4G",
        //     .password = "QMYD13286306350",
            .ssid = "QMYD2.4G",
            .password = "QMYD13286306350",
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false},
        },
    };
    strcpy((char *)wifi_config.sta.ssid, ssid);
    strcpy((char *)wifi_config.sta.password, password);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "wifi_init_sta finished.");
    ESP_LOGI(TAG, "connect to ap SSID:%s password:%s",
             ssid, password);
}

// 连接按钮点击事件回调函数
static void connect_btn_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED)
    {
        const char *ssid = lv_textarea_get_text(ssid_textarea);
        const char *password = lv_textarea_get_text(password_textarea);
       //   connect_to_wifi(ssid, password);
       
    }
}

// 文本框焦点事件回调函数
static void textarea_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *textarea = lv_event_get_target(e);
    if (code == LV_EVENT_FOCUSED)
    {
        lv_keyboard_set_textarea(kb, textarea);
        lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
    else if (code == LV_EVENT_DEFOCUSED)
    {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    }
}

#if 0

// LVGL 初始化函数
static void lvgl_init(void)
{
    lv_init(); tft_init();

    // 创建屏幕对象
    lv_obj_t *scr = lv_scr_act();

    // 创建 SSID 文本框
    ssid_textarea = lv_textarea_create(scr);
    lv_textarea_set_placeholder_text(ssid_textarea, "SSID");
    lv_obj_set_width(ssid_textarea, 200);
    lv_obj_align(ssid_textarea, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_add_event_cb(ssid_textarea, textarea_event_cb, LV_EVENT_ALL, NULL);

    // 创建密码文本框
    password_textarea = lv_textarea_create(scr);
    lv_textarea_set_placeholder_text(password_textarea, "Password");
    lv_textarea_set_password_mode(password_textarea, true);
    lv_textarea_set_one_line(password_textarea, true);
    lv_obj_set_width(password_textarea, 200);
    lv_obj_align(password_textarea, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_add_event_cb(password_textarea, textarea_event_cb, LV_EVENT_ALL, NULL);

    // 创建连接按钮
    connect_btn = lv_btn_create(scr);
    lv_obj_set_width(connect_btn, 100);
    lv_obj_align(connect_btn, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_add_event_cb(connect_btn, connect_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(connect_btn);
    lv_label_set_text(label, "Connect");
    lv_obj_center(label);

    // 创建状态标签
    status_label = lv_label_create(scr);
    lv_label_set_text(status_label, "Enter SSID and password");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 170);

    // 创建虚拟键盘
    kb = lv_keyboard_create(scr);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
}

// LVGL 任务函数
static void lvgl_task(void *arg)
{
    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}



void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    lvgl_init();

    xTaskCreate(lvgl_task, "lvgl_task", 4096, NULL, 5, NULL);
}
#endif

void configwifi_key(lv_event_t *e)
{
    printf("wifi:%s,pwd:%s\n", lv_textarea_get_text(ui_TextArea1), lv_textarea_get_text(ui_TextArea2));
    connect_to_wifi(lv_textarea_get_text(ui_TextArea1), lv_textarea_get_text(ui_TextArea2));
}

void screen1_key(lv_event_t *e)
{
    EventBits_t uxBits;

    // 获取事件组的当前状态


   // ;

    if (s_wifi_event_group != NULL)
    {
        uxBits = xEventGroupGetBits(s_wifi_event_group);
        if (uxBits & BIT1)
        {
            // 处理事件 1 发生的情况
            printf("Event 1 has occurred.\n");
            lv_label_set_text(ui_Label2, " 接通");
           // exit_rtc_task =1;
        }
        else
        {
            if (uxBits & BIT0)
            {
              //  exit_rtc_task =0;
                // 处理事件 0 发生的情况
               /// printf("Event 0 has occurred.\n");
             //  StartRtc( );
              
            }
    
            else
            {
               // exit_rtc_task =1;
                lv_disp_load_scr(ui_configwifi);
            }
        }
      
        // 处理事件组状态
        // printf("Event group state: %d\n", uxBits);
    }
    else
    {
        printf("Event group is NULL.\n");
        lv_disp_load_scr(ui_configwifi);
        //lv_disp_load_scr(ui_configwifi);
    } 
}