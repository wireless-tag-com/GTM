#include <stdio.h>
#include "string.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qmsd_board.h"
#include "qmsd_utils.h"
#include "lvgl.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "board.h"

#include "i2c_device.h"
#include "aw9523.h"
#include "mkpb2016.h"

#include "spiffs_stream.h"
#include "periph_spiffs.h"

#define TAG "QMSD-MAIN"

void gui_user_init()
{
    extern void test_ui();
    ui_init();
}

void board_aw9523_device_init(void)
{
    aw9523_init(AW9523_I2C_SDA_PIN, AW9523_I2C_SCL_PIN);
    aw9523_set_addr(0xB2 >> 1); // 0x59

    aw9523_io_set_gpio_or_led(BOARD_RESET_PIN >> 4, BOARD_RESET_PIN & 0x0f, AW9523_MODE_GPIO);
    aw9523_io_set_inout(BOARD_RESET_PIN >> 4, BOARD_RESET_PIN & 0x0f, AW9523_MODE_OUTPUT);
    aw9523_io_set_level(BOARD_RESET_PIN >> 4, BOARD_RESET_PIN & 0x0f, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    aw9523_io_set_level(BOARD_RESET_PIN >> 4, BOARD_RESET_PIN & 0x0f, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    aw9523_set_led_max_current(AW9523_37mA);
    aw9523_io_set_gpio_or_led(LCD_BL_0_PIN >> 4, LCD_BL_0_PIN & 0x0f, AW9523_MODE_LED);
    aw9523_io_set_gpio_or_led(LCD_BL_1_PIN >> 4, LCD_BL_1_PIN & 0x0f, AW9523_MODE_LED);
    aw9523_io_set_gpio_or_led(LCD_BL_2_PIN >> 4, LCD_BL_2_PIN & 0x0f, AW9523_MODE_LED);
    aw9523_io_set_gpio_or_led(LCD_BL_3_PIN >> 4, LCD_BL_3_PIN & 0x0f, AW9523_MODE_LED);
    aw9523_io_set_gpio_or_led(LCD_BL_4_PIN >> 4, LCD_BL_4_PIN & 0x0f, AW9523_MODE_LED);
    aw9523_io_set_gpio_or_led(LCD_BL_5_PIN >> 4, LCD_BL_5_PIN & 0x0f, AW9523_MODE_LED);
    aw9523_led_set_duty(LCD_BL_0_PIN >> 4, LCD_BL_0_PIN & 0x0f, 128);
    aw9523_led_set_duty(LCD_BL_1_PIN >> 4, LCD_BL_1_PIN & 0x0f, 128);
    aw9523_led_set_duty(LCD_BL_2_PIN >> 4, LCD_BL_2_PIN & 0x0f, 128);
    aw9523_led_set_duty(LCD_BL_3_PIN >> 4, LCD_BL_3_PIN & 0x0f, 128);
    aw9523_led_set_duty(LCD_BL_4_PIN >> 4, LCD_BL_4_PIN & 0x0f, 128);
    aw9523_led_set_duty(LCD_BL_5_PIN >> 4, LCD_BL_5_PIN & 0x0f, 128);

    aw9523_io_set_gpio_or_led(PA_CTRL_PIN >> 4, PA_CTRL_PIN & 0x0f, AW9523_MODE_GPIO);
    aw9523_io_set_inout(PA_CTRL_PIN >> 4, PA_CTRL_PIN & 0x0f, AW9523_MODE_OUTPUT);
    aw9523_io_set_level(PA_CTRL_PIN >> 4, PA_CTRL_PIN & 0x0f, 1);
}


void app_main(void)
{
    board_aw9523_device_init();

    ESP_LOGI(TAG, "[ 1 ] 挂载");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG(); // 创建外设配置
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);   // 创建外设集

    //--------------------------

    ESP_LOGI(TAG, "[ 2 ] 启动编解码芯片");
    audio_board_handle_t board_handle = audio_board_init();                                         // 初始化编码芯片
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START); // 启动编码芯片

    audio_hal_set_volume(board_handle->audio_hal, 40);
    ESP_LOGI(TAG, "audio_board & audio_board_key init");

    gpio_install_isr_service(ESP_INTR_FLAG_SHARED);
    qmsd_board_config_t config = QMSD_BOARD_DEFAULT_CONFIG;
    config.board_dir = BOARD_ROTATION_0;
    config.touch.en = 1;
    config.backlight.value = 0;
    qmsd_board_init(&config);
    printf("Fine qmsd!\r\n");

    gui_user_init();

    rtc_initial();
    while (1)
    {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}
