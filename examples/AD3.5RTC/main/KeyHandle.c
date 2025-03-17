#include <stdio.h>
#include "string.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "start_bot/start_bot.h"
#include "ui_code/ui.h"

uint8_t char_to_num(uint8_t *chart)
{
    uint8_t num = 0;
    if (chart[1] >= '0' && chart[1] <= '9')
    {
        num = (chart[0] - '0') * 10;
        num += chart[1] - '0';
    }
    else
    {
        if (chart[0] >= '0' && chart[0] <= '9')
            num = chart[0] - '0';
        else
            num = 0;
    }
    return num;
}

void rotation_screen(lv_event_t *e)
{

    const char *get_txt;
    uint8_t value;
    uint32_t txt_len = 0;

    get_txt = lv_textarea_get_text(ui_TextArea1);
    lv_textarea_set_accepted_chars(ui_TextArea1, "QMYD2.4G");
    value = char_to_num(get_txt);
}

void keyboardValue(lv_event_t *e)
{


    bool is_focused = lv_obj_has_state(ui_TextArea1, LV_STATE_FOCUSED);
    if (is_focused)
    {
        printf("Text area is focused.\n");
        lv_keyboard_set_textarea(ui_Keyboard1, ui_TextArea1);
    }
    else
    {
        printf("Text area is not focused.\n");
        lv_keyboard_set_textarea(ui_Keyboard1, ui_TextArea2);
    }
}

void TextInput2(lv_event_t *e)
{
    printf("TextInput2");
}

void TextInput1(lv_event_t *e)
{
    printf("TextInput1");
}