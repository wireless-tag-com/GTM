#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <inttypes.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_task_info.h"
#include "esp_random.h"
#include <VolcEngineRTCLite.h>
#include "opus_frames.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_littlefs.h"
#include "config.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "audio_sys.h"
#include "board.h"
#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "i2s_stream.h"
#include "AudioPipeline.h"
#include "start_bot/start_bot.h"
#include "lvgl.h"
#include "ui_code/ui.h"

#define STATS_TASK_PRIO 5
#define DEFAULT_READ_COUNT 50000
#define DEFAULT_RUN_RTC_COUNT 20
uint8_t exit_rtc_task = 0;

static const char *TAG = "VolcRTCDemo";
static bool joined = false;
static bool fini_notifyed = false;
char *global_appid = NULL;
char *global_token = NULL;
char *global_roomid = NULL;
char *global_uid = NULL;
extern EventGroupHandle_t s_wifi_event_group;

static void esp_dump_per_task_heap_info(void);
static void realtime_stats_timer_callback(void *arg)
{
#ifdef CONFIG_ENABLE_RUN_TIME_STATS
	audio_sys_get_real_time_stats();
	ESP_LOGE(TAG, "MALLOC_CAP_INTERNAL:%d/%d (free/total) Bytes, MALLOC_CAP_SPIRAM:%d/%d (free/total) Bytes",
			 heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
			 heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
			 heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
			 heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
#endif

#ifdef CONFIG_HEAP_TASK_TRACKING
	esp_dump_per_task_heap_info();
#endif
}

// byte rtc lite callbacks
static void byte_rtc_on_join_room_success(byte_rtc_engine_t engine, const char *channel, int elapsed_ms)
{
	ESP_LOGI(TAG, "join channel success %s elapsed %d ms now %d ms\n", channel, elapsed_ms, elapsed_ms);
	joined = true;
};

static void byte_rtc_on_user_joined(byte_rtc_engine_t engine, const char *channel, const char *user_name, int elapsed_ms)
{
	ESP_LOGI(TAG, "remote user joined  %s:%s\n", channel, user_name);
};

static void byte_rtc_on_user_offline(byte_rtc_engine_t engine, const char *channel, const char *user_name, int reason)
{
	ESP_LOGI(TAG, "remote user offline  %s:%s\n", channel, user_name);
};

static void byte_rtc_on_user_mute_audio(byte_rtc_engine_t engine, const char *channel, const char *user_name, int muted)
{
	ESP_LOGI(TAG, "remote user mute audio  %s:%s %d\n", channel, user_name, muted);
};

static void byte_rtc_on_room_error(byte_rtc_engine_t engine, const char *channel, int code, const char *msg)
{
	ESP_LOGI(TAG, "error occur %s %d %s\n", channel, code, msg ? msg : "");
};

static void byte_rtc_on_audio_data(byte_rtc_engine_t engine, const char *channel, const char *uid, uint16_t sent_ts,
								   audio_codec_type_e codec, const void *data_ptr, size_t data_len)
{
	player_pipeline_handle_t player_pipeline = (player_pipeline_handle_t)byte_rtc_get_user_data(engine);
	if (player_pipeline != NULL)
	{
		player_pipeline_write(player_pipeline, data_ptr, data_len);
	}
}

static void on_message_received(byte_rtc_engine_t engine, const char *room, const char *uid, const uint8_t *message, int size, bool binary)
{
	ESP_LOGI(TAG, "on_message_received uid: %s, message size: %d", uid, size);
}

static void on_fini_notify(byte_rtc_engine_t engine)
{
	fini_notifyed = true;
}

static void byte_rtc_task(void *pvParameters)
{
	int run_count = 0;
	uint8_t *audio_buffer = 0;
	esp_timer_handle_t realtime_stats_timer = NULL;
	esp_timer_create_args_t create_args = {.callback = realtime_stats_timer_callback, .arg = NULL, .name = "fps timer"};
	esp_timer_create(&create_args, &realtime_stats_timer);
	esp_timer_start_periodic(realtime_stats_timer, 20 * 1000 * 1000);
	if (s_wifi_event_group != NULL)
		xEventGroupSetBits(s_wifi_event_group, BIT1);
	if (ui_Label2 != NULL)
		lv_label_set_text(ui_Label2, "挂通");

	do
	{
		// clean flags
		fini_notifyed = false;
		joined = false;

		recorder_pipeline_handle_t pipeline = recorder_pipeline_open();
		const int default_read_size = recorder_pipeline_get_default_read_size(pipeline);
		audio_buffer = heap_caps_malloc(default_read_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
		if (!audio_buffer)
		{
			break;
		}
		player_pipeline_handle_t player_pipeline = player_pipeline_open();
		recorder_pipeline_run(pipeline);
		player_pipeline_run(player_pipeline);

		byte_rtc_event_handler_t handler = {0};
		handler.on_join_room_success = byte_rtc_on_join_room_success;
		handler.on_room_error = byte_rtc_on_room_error;
		handler.on_user_joined = byte_rtc_on_user_joined;
		handler.on_user_offline = byte_rtc_on_user_offline;
		handler.on_user_mute_audio = byte_rtc_on_user_mute_audio;
		handler.on_audio_data = byte_rtc_on_audio_data;
		handler.on_message_received = on_message_received;
		handler.on_fini_notify = on_fini_notify;

#if START_BOT
		byte_rtc_engine_t engine = byte_rtc_create(global_appid, &handler);

#else
		byte_rtc_engine_t engine = byte_rtc_create(DEFAULT_APPID, &handler);
#endif
		byte_rtc_set_log_level(engine, BYTE_RTC_LOG_LEVEL_ERROR);
#ifdef RTC_TEST_ENV
		byte_rtc_set_params(engine, "{\"env\":2}"); // test env
#endif
		// byte_rtc_set_params(engine, "{\"rtc\":{\"root_path\":\"/littlefs\"}}");
		// byte_rtc_config_log(engine, NULL, 1024 * 200, 8);
		byte_rtc_set_params(engine, "{\"debug\":{\"log_to_console\":1}}");
		byte_rtc_set_params(engine, "{\"rtc\":{\"thread\":{\"pinned_to_core\":1}}}");
		// byte_rtc_set_params(engine,"{\"rtc\":{\"thread\":{\"stack_size\":16000}}}");
		byte_rtc_set_params(engine, "{\"rtc\":{\"thread\":{\"priority\":5}}}");
		// byte_rtc_set_params(engine,"{\"rtc\":{\"license\":{\"enable\":1}}}");
		byte_rtc_init(engine);
		byte_rtc_set_audio_codec(engine, AUDIO_CODEC_TYPE_G711A);
		byte_rtc_set_video_codec(engine, VIDEO_CODEC_TYPE_H264);

		byte_rtc_set_user_data(engine, player_pipeline);

		byte_rtc_room_options_t options;
		options.auto_subscribe_audio = 1; // 接收远端音频
		options.auto_subscribe_video = 0; // 不接收远端视频

#if START_BOT
		byte_rtc_join_room(engine, global_roomid, global_uid, global_token, &options);
#else
		byte_rtc_join_room(engine, DEFAULT_ROOMID, DEFAULT_UID, DEFAULT_TOKEN, &options);
#endif
		int read_count = DEFAULT_READ_COUNT;
		while (--read_count > 0)
		{
			// ESP_LOGI(TAG, "录音1：%d", read_count);
			int ret = recorder_pipeline_read(pipeline, (char *)audio_buffer, default_read_size);
			//  ESP_LOGI(TAG, "录音-dafault_read_size:%d,joined:%d", default_read_size,joined);
			if (ret == default_read_size && joined)
			{
				audio_frame_info_t audio_frame_info = {0};
#if START_BOT
				byte_rtc_send_audio_data(engine, global_roomid, audio_buffer, default_read_size, &audio_frame_info);
#else
				byte_rtc_send_audio_data(engine, DEFAULT_ROOMID, audio_buffer, default_read_size, &audio_frame_info);
#endif
			}
			if (exit_rtc_task)
			{
				break;
			}
		}
		heap_caps_free(audio_buffer);

		byte_rtc_fini(engine);
		while (!fini_notifyed)
		{
			usleep(1000 * 1000);
			if (exit_rtc_task)
			{
				break;
			}
		};

		recorder_pipeline_close(pipeline);
		player_pipeline_close(player_pipeline);
		byte_rtc_destory(engine);
		ESP_LOGI(TAG, "finish %d run ", run_count);
		if (exit_rtc_task)
		{
			break;
		}

	} while (run_count++ < DEFAULT_RUN_RTC_COUNT);

	if (audio_buffer)
	{
		heap_caps_free(audio_buffer);
	}
	ESP_LOGI(TAG, "closeRtc");
	esp_timer_stop(realtime_stats_timer);
	esp_timer_delete(realtime_stats_timer);
	vTaskDelete(byte_rtc_task); // 从外部删除任务
}
void create_lvgl_task(int core_id);
void rtc_initial(void)
{
	ESP_ERROR_CHECK(nvs_flash_init());
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_log_level_set(TAG, ESP_LOG_INFO);
	esp_log_level_set("AUDIO_PIPELINE", ESP_LOG_INFO);
	esp_log_level_set("AUDIO_SYS", ESP_LOG_INFO);

	// esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
	// esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);

	// esp_lcd_panel_handle_t panel_handle = audio_board_lcd_init(set, lcd_trans_done_cb);
	// ESP_ERROR_CHECK(lv_port_init(panel_handle));

	/* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
	 * Read "Establishing Wi-Fi or Ethernet Connection" section in
	 * examples/protocols/README.md for more information about this function.
	 */
	//
#if 0
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/littlefs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
        esp_littlefs_format(conf.partition_label);
    }
    else
    {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    ESP_LOGI(TAG, "Mount sdcard");

    // audio_board_sdcard_init(set, SD_MODE_1_LINE);

#endif

	// audio_board_key_init(set);
#if 1
	ESP_ERROR_CHECK(example_connect());
	bool is_start = false;
	// 不断尝试启动 RTCBot
	while (!is_start)
	{
		is_start = startRTCBot();
		if (!is_start)
		{
			ESP_LOGE(TAG, "startRTCBot failed, retrying...");
			vTaskDelay(pdMS_TO_TICKS(1000)); // 等待 1 秒后重试，避免过于频繁的请求
		}
	}

	// vTaskDelay(pdMS_TO_TICKS(100));
	//   xTaskCreatePinnedToCore( &byte_rtc_task,  "byte_rtc_task", 1024*10, NULL, 5, NULL, 1);
	xTaskCreate(&byte_rtc_task, "byte_rtc_task", 8192, NULL, STATS_TASK_PRIO, NULL);
#endif
#if 0
    bsp_i2c_init();

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = 0,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        }
    };
    cfg.lvgl_port_cfg.task_stack = 8192;
    cfg.lvgl_port_cfg.task_affinity = 1;
    bsp_display_start_with_config(&cfg);

    ESP_LOGI(TAG, "Display LVGL demo");
    ui_init();

    vTaskDelay(pdMS_TO_TICKS(500));
    bsp_display_backlight_on();
#endif
}
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// print heap info
#ifdef CONFIG_HEAP_TASK_TRACKING
#define MAX_TASK_NUM 20	 // Max number of per tasks info that it can store
#define MAX_BLOCK_NUM 20 // Max number of per block info that it can store

static size_t s_prepopulated_num = 0;
static heap_task_totals_t s_totals_arr[MAX_TASK_NUM];
static heap_task_block_t s_block_arr[MAX_BLOCK_NUM];

static void esp_dump_per_task_heap_info(void)
{
	heap_task_info_params_t heap_info = {0};
	heap_info.caps[0] = MALLOC_CAP_8BIT; // Gets heap with CAP_8BIT capabilities
	heap_info.mask[0] = MALLOC_CAP_8BIT;
	heap_info.caps[1] = MALLOC_CAP_32BIT; // Gets heap info with CAP_32BIT capabilities
	heap_info.mask[1] = MALLOC_CAP_32BIT;
	heap_info.tasks = NULL; // Passing NULL captures heap info for all tasks
	heap_info.num_tasks = 0;
	heap_info.totals = s_totals_arr; // Gets task wise allocation details
	heap_info.num_totals = &s_prepopulated_num;
	heap_info.max_totals = MAX_TASK_NUM;  // Maximum length of "s_totals_arr"
	heap_info.blocks = s_block_arr;		  // Gets block wise allocation details. For each block, gets owner task, address and size
	heap_info.max_blocks = MAX_BLOCK_NUM; // Maximum length of "s_block_arr"

	heap_caps_get_per_task_info(&heap_info);

	for (int i = 0; i < *heap_info.num_totals; i++)
	{
		if (heap_info.totals[i].size[0] > 50000)
			ESP_LOGI(TAG, "Task: %s -> CAP_8BIT: %d ",
					 heap_info.totals[i].task ? pcTaskGetName(heap_info.totals[i].task) : "Pre-Scheduler allocs",
					 heap_info.totals[i].size[0]); // Heap size with CAP32_BIT capabilities
	}

	ESP_LOGI(TAG, "\n");
}
#endif

void StartRtc(void)
{

	bool is_start = false;
	// 不断尝试启动 RTCBot
	while (!is_start)
	{
		is_start = startRTCBot();
		if (!is_start)
		{
			ESP_LOGE(TAG, "startRTCBot failed, retrying...");
			vTaskDelay(pdMS_TO_TICKS(1000)); // 等待 1 秒后重试，避免过于频繁的请求
		}
	}

	vTaskDelay(pdMS_TO_TICKS(100));
	xTaskCreate(&byte_rtc_task, "byte_rtc_task", 8192, NULL, STATS_TASK_PRIO, NULL);
	// xTaskCreatePinnedToCore(&byte_rtc_task, "byte_rtc_task", 8192, NULL, 5, STATS_TASK_PRIO, 1);
}
