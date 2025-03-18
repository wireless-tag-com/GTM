#include "lvgl.h"
#include "qmsd_board.h"
#include "mkpb2016.h"

lv_obj_t *value;
uint8_t sound_recording_status = 0; // 0:空闲 1:开始录制 2：录制结束开始播放录制
LV_IMG_DECLARE(lv_img_sound);

uint32_t proximity = 0;
void proximity_task(void *param)
{
    while (1)
    {
        mktp2016_read_value(&proximity);
        // printf("proximity_task  %ld \r\n",proximity);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void ui_timer(lv_timer_t *timer)
{
    lv_label_set_text_fmt(value, "%ld", proximity);
}

static void btn_event(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    lv_obj_t *target = lv_event_get_target(e);

    if (event_code == LV_EVENT_PRESSED)
    {
        sound_recording_status = 1;
        printf("LV_EVENT_PRESSED\r\n");
    }

    if (event_code == LV_EVENT_RELEASED)
    {
        sound_recording_status = 2;
        printf("LV_EVENT_RELEASED\r\n");
    }
}

void test_ui()
{
    lv_obj_t *src = lv_img_create(lv_scr_act());
    lv_img_set_src(src, &lv_img_sound);
    lv_obj_align(src, LV_ALIGN_TOP_MID, 0, 50);

    lv_obj_t *label = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(label, "Long press to record, \n release to play the recording.");
    lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -100);

    lv_obj_t *btn = lv_btn_create(lv_scr_act());
    lv_obj_set_size(btn, 200, 50);
    lv_obj_set_pos(btn, 50, 300);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 50);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_obj_center(btn_label);
    lv_label_set_text(btn_label, "sound recording");

    value = lv_label_create(lv_scr_act());
    lv_label_set_text(value, "0");
    lv_obj_align(value, LV_ALIGN_BOTTOM_LEFT, 50, -20);

    lv_obj_add_event_cb(btn, btn_event, LV_EVENT_ALL, NULL);

    lv_timer_create(ui_timer, 1000, NULL);
    qmsd_board_backlight_set(100);
}