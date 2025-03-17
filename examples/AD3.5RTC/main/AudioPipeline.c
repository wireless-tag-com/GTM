#include "AudioPipeline.h"
#include <string.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "audio_sys.h"
#include "board.h"
#include "algorithm_stream.h"
#include "filter_resample.h"
// #include "esp_peripherals.h"
// #include "periph_sdcard.h"
#include "i2s_stream.h"
#include "pthread.h"

#include "esp_timer.h"

#define CONFIG_CHOICE_G711A_ENCODER 1
// #define CONFIG_CHOICE_OPUS_ENCODER 1
// #define CONFIG_CHOICE_AAC_ENCODER 1
#define CONFIG_AUDIO_SUPPORT_G711A_DECODER 1

#if defined(CONFIG_CHOICE_OPUS_ENCODER)
#include "opus_encoder.h"
#include "opus_decoder.h"
#elif defined(CONFIG_CHOICE_AAC_ENCODER)
#include "aac_encoder.h"
#include "aac_decoder.h"
#elif defined(CONFIG_CHOICE_G711A_ENCODER)
#include "g711_encoder.h"
#include "g711_decoder.h"
#endif
#include "audio_idf_version.h"
#include "raw_stream.h"

#define CHANNEL 1
#define RECORD_TIME_SECONDS (10)
static const char *TAG = "AUDIO_PIPELINE";

#if defined(CONFIG_CHOICE_OPUS_ENCODER)
#define SAMPLE_RATE 16000
#define BIT_RATE 64000
#define COMPLEXITY 10
#define FRAME_TIME_MS 20

#define DEC_SAMPLE_RATE 48000
#define DEC_BIT_RATE 64000
#endif

#if defined(CONFIG_CHOICE_AAC_ENCODER)
#define SAMPLE_RATE 16000
#define BIT_RATE 80000
#endif
typedef struct
{
    unsigned char *buffer;
    int size;
} queue_item, *queue_item_handle_t;

int play_buffer_flag = 20;
typedef struct
{
    pthread_t thread;
    int play_packet_count;
    bool stoped;
    QueueHandle_t audio_queue;
    void *user_data;
} player_thread_data, *player_thread_data_handle_t;
struct recorder_pipeline_t
{
    audio_pipeline_handle_t audio_pipeline;
    audio_element_handle_t i2s_stream_reader;
    audio_element_handle_t audio_encoder;
    audio_element_handle_t raw_reader;
    audio_element_handle_t rsp;
    audio_element_handle_t algo_aec;
};

struct player_pipeline_t
{
    player_thread_data_handle_t thread_data;
    audio_pipeline_handle_t audio_pipeline;
    audio_element_handle_t raw_writer;
    audio_element_handle_t audio_decoder;
    audio_element_handle_t i2s_stream_writer;
    esp_timer_handle_t poll_audio_timer;
};

static void player_thread(void *arg);

player_thread_data_handle_t player_thread_data_create(void *user_data)
{
    player_thread_data_handle_t handle = heap_caps_malloc(sizeof(player_thread_data), MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT);
    assert(handle != NULL);
    handle->audio_queue = xQueueCreate(256, sizeof(queue_item));
    assert(handle->audio_queue != NULL);
    handle->stoped = false;
    handle->play_packet_count = play_buffer_flag;
    return handle;
};

void player_thread_data_destory(player_thread_data_handle_t handle)
{
    assert(handle != 0);
    heap_caps_free(handle);
};

void player_thread_data_start(player_thread_data_handle_t handle) {
    // xTaskCreatePinnedToCore(player_thread, "player_thread", 4096, handle->user_data, 5, NULL, 1);
};

void player_thread_data_stop(player_thread_data_handle_t handle)
{
    assert(handle != 0);
    handle->stoped = true;
};

static audio_element_handle_t create_resample_stream(void)
{
    rsp_filter_cfg_t rsp_cfg_w = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg_w.src_rate = 16000;
    rsp_cfg_w.src_ch = 1;
    rsp_cfg_w.dest_rate = 8000;
    rsp_cfg_w.dest_ch = 1;
    rsp_cfg_w.complexity = 5;
    return rsp_filter_init(&rsp_cfg_w);
}

static audio_element_handle_t create_algo_stream(void)
{
    ESP_LOGI(TAG, "[3.1] Create algorithm stream for aec");
    algorithm_stream_cfg_t algo_config = ALGORITHM_STREAM_CFG_DEFAULT();
    algo_config.swap_ch = true;
    algo_config.sample_rate = 8000;
    algo_config.out_rb_size = 256;
    algo_config.algo_mask = ALGORITHM_STREAM_DEFAULT_MASK | ALGORITHM_STREAM_USE_AGC;
    audio_element_handle_t element_algo = algo_stream_init(&algo_config);
    audio_element_set_music_info(element_algo, 8000, 1, 16);
    audio_element_set_input_timeout(element_algo, portMAX_DELAY);
    return element_algo;
}

#include "es7210.h"
recorder_pipeline_handle_t recorder_pipeline_open()
{
    recorder_pipeline_handle_t pipeline = heap_caps_malloc(sizeof(recorder_pipeline_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT);
    // memset(&pipeline,0,sizeof(recorder_pipeline_t));
    int channel_format = I2S_CHANNEL_TYPE_RIGHT_LEFT;
    if (CHANNEL == 1)
    {
        channel_format = I2S_CHANNEL_TYPE_ONLY_LEFT;
    }
    int sample_rate = 16000;
    // es7210_mic_select(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC3);
    es7210_adc_set_gain(ES7210_INPUT_MIC3, GAIN_0DB);//GAIN_MINUS_6DB
   // es7210_adc_set_gain(ES7210_INPUT_MIC3, GAIN_MINUS_6DB);
 
    // es7210_adc_set_gain(ES7210_INPUT_MIC3, GAIN_37_5DB);

    ESP_LOGI(TAG, "[3.0] Create audio pipeline for recording");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline->audio_pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline->audio_pipeline);

    ESP_LOGI(TAG, "[3.2] Create i2s stream to read audio data from codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(CODEC_ADC_I2S_PORT, sample_rate, 32, AUDIO_STREAM_READER);

    i2s_cfg.type = AUDIO_STREAM_READER;
#if defined(CONFIG_CHOICE_OPUS_ENCODER)
    sample_rate = SAMPLE_RATE;
#elif defined(CONFIG_CHOICE_AAC_ENCODER)
    sample_rate = SAMPLE_RATE;
#elif defined(CONFIG_CHOICE_G711A_ENCODER)
    sample_rate = 8000;
#endif

    i2s_stream_set_channel_type(&i2s_cfg, channel_format);
    i2s_cfg.std_cfg.clk_cfg.sample_rate_hz = sample_rate;
    pipeline->i2s_stream_reader = i2s_stream_init(&i2s_cfg);
    ESP_LOGI(TAG, "[3.3] Create audio encoder to handle data");

#if defined(CONFIG_CHOICE_OPUS_ENCODER)
    raw_opus_enc_config_t opus_cfg = DEFAULT_OPUS_ENCODER_CONFIG();
    opus_cfg.sample_rate = SAMPLE_RATE;
    opus_cfg.channel = CHANNEL;
    opus_cfg.bitrate = BIT_RATE;
    opus_cfg.complexity = COMPLEXITY;
    pipeline->audio_encoder = raw_opus_decoder_init(&opus_cfg);
#elif defined(CONFIG_CHOICE_AAC_ENCODER)
    aac_encoder_cfg_t aac_cfg = DEFAULT_AAC_ENCODER_CONFIG();
    aac_cfg.sample_rate = SAMPLE_RATE;
    aac_cfg.channel = CHANNEL;
    aac_cfg.bitrate = BIT_RATE;
    pipeline->audio_encoder = aac_encoder_init(&aac_cfg);
#elif defined(CONFIG_CHOICE_G711A_ENCODER)
    g711_encoder_cfg_t g711_cfg = DEFAULT_G711_ENCODER_CONFIG();
    pipeline->audio_encoder = g711_encoder_init(&g711_cfg);
#endif
    ESP_LOGI(TAG, "[3.4] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline->audio_pipeline, pipeline->i2s_stream_reader, "i2s");

    pipeline->algo_aec = create_algo_stream();
    audio_pipeline_register(pipeline->audio_pipeline, pipeline->algo_aec, "algo");

#if defined(CONFIG_CHOICE_OPUS_ENCODER)
    audio_pipeline_register(pipeline->audio_pipeline, pipeline->audio_encoder, "opus");
#elif defined(CONFIG_CHOICE_AAC_ENCODER)
    audio_pipeline_register(pipeline->audio_pipeline, pipeline->audio_encoder, "aac");
#elif defined(CONFIG_CHOICE_G711A_ENCODER)
    audio_pipeline_register(pipeline->audio_pipeline, pipeline->audio_encoder, "g711a");
#endif

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_cfg.out_rb_size = 2 * 1024;
    pipeline->raw_reader = raw_stream_init(&raw_cfg);
    audio_element_set_output_timeout(pipeline->raw_reader, portMAX_DELAY);
    audio_pipeline_register(pipeline->audio_pipeline, pipeline->raw_reader, "raw");
    ESP_LOGI(TAG, "[3.5] Link it together [codec_chip]-->i2s_stream-->audio_encoder-->raw");

#if defined(CONFIG_CHOICE_OPUS_ENCODER)
    const char *link_tag[3] = {"i2s", "opus", "raw"};
#elif defined(CONFIG_CHOICE_AAC_ENCODER)
    const char *link_tag[3] = {"i2s", "aac", "raw"};
#elif defined(CONFIG_CHOICE_G711A_ENCODER)
    const char *link_tag[4] = {"i2s", "algo", "g711a", "raw"};
#endif

    audio_pipeline_link(pipeline->audio_pipeline, &link_tag[0], 4);
    return pipeline;
}

void recorder_pipeline_close(recorder_pipeline_handle_t pipeline)
{
    audio_pipeline_stop(pipeline->audio_pipeline);
    audio_pipeline_wait_for_stop(pipeline->audio_pipeline);
    audio_pipeline_terminate(pipeline->audio_pipeline);

    audio_pipeline_unregister(pipeline->audio_pipeline, pipeline->algo_aec);
    audio_pipeline_unregister(pipeline->audio_pipeline, pipeline->audio_encoder);
    audio_pipeline_unregister(pipeline->audio_pipeline, pipeline->i2s_stream_reader);
    audio_pipeline_unregister(pipeline->audio_pipeline, pipeline->raw_reader);

    /* Release all resources */
    audio_pipeline_deinit(pipeline->audio_pipeline);
    audio_element_deinit(pipeline->algo_aec);
    audio_element_deinit(pipeline->raw_reader);
    audio_element_deinit(pipeline->i2s_stream_reader);
    audio_element_deinit(pipeline->audio_encoder);

    heap_caps_free(pipeline);
};

void recorder_pipeline_run(recorder_pipeline_handle_t pipeline)
{
    audio_pipeline_run(pipeline->audio_pipeline);
};

int recorder_pipeline_get_default_read_size(recorder_pipeline_handle_t pipeline)
{
#if defined(CONFIG_CHOICE_OPUS_ENCODER)
    return BIT_RATE * FRAME_TIME_MS / 1000; //
#elif defined(CONFIG_CHOICE_AAC_ENCODER)
    return -1; //
#elif defined(CONFIG_CHOICE_G711A_ENCODER)
    return 160;
#endif
};

audio_element_handle_t recorder_pipeline_get_raw_reader(recorder_pipeline_handle_t pipeline)
{
    return pipeline->raw_reader;
};
audio_pipeline_handle_t recorder_pipeline_get_pipeline(recorder_pipeline_handle_t pipeline)
{
    return pipeline->audio_pipeline;
};

int recorder_pipeline_read(recorder_pipeline_handle_t pipeline, char *buffer, int buf_size)
{
    return raw_stream_read(pipeline->raw_reader, buffer, buf_size);
}

static void poll_audio_timer_callback(void *arg)
{
    player_pipeline_handle_t player_pipeline = (player_pipeline_handle_t)(arg);
    TickType_t delay = portMAX_DELAY;
    queue_item qitem = {0};

    if (player_pipeline->thread_data->play_packet_count <= 0)
    {
        // ESP_LOGI(TAG, "processing .......");

        if (xQueueReceive(player_pipeline->thread_data->audio_queue, &qitem, 0))
        {
            if (qitem.buffer == (unsigned char *)(&play_buffer_flag))
            {
                player_pipeline->thread_data->play_packet_count = play_buffer_flag;
            }
            else
            {
                raw_stream_write(player_pipeline->raw_writer, qitem.buffer, qitem.size);
                audio_free(qitem.buffer);
            }
        }
    }
    else
    {
        // ESP_LOGI(TAG, "buffering .......");
    }
}

// static void player_thread(void * arg) {
//     player_pipeline_handle_t  player_pipeline = (player_pipeline_handle_t)(arg);
//     TickType_t delay = portMAX_DELAY;
//     unsigned char * buffer;
//     queue_item qitem = {0};
//     int empty_frame = 0;
//     while (true) {
//         if (player_pipeline->thread_data->play_packet_count <= 0) {
//             if (xQueueReceive(player_pipeline->thread_data->audio_queue, &qitem, delay)) {
//                 if (qitem.buffer == (unsigned char*) (&play_buffer_flag)) {
//                     player_pipeline->thread_data->play_packet_count = play_buffer_flag;
//                 } else {
//                     raw_stream_write(player_pipeline->raw_writer, qitem.buffer,qitem.size);
//                     audio_free(qitem.buffer);
//                     empty_frame = 0;
//                 }
//             }
//         } else {
//             usleep(10000);
//         }
//     }
// }

player_pipeline_handle_t player_pipeline_open(void)
{
    player_pipeline_handle_t player_pipeline = heap_caps_malloc(sizeof(player_pipeline_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "[ 2 ] Start codec chip");

    assert(player_pipeline != 0);

    player_pipeline->thread_data = player_thread_data_create(player_pipeline);
    assert(player_pipeline->thread_data);
    player_pipeline->thread_data->user_data = player_pipeline;
    player_thread_data_start(player_pipeline->thread_data);

    ESP_LOGI(TAG, "[3.0] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    player_pipeline->audio_pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_READER;
    raw_cfg.out_rb_size = 8 * 1024;
    player_pipeline->raw_writer = raw_stream_init(&raw_cfg);

    ESP_LOGI(TAG, "[3.2] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(I2S_NUM_0, 8000, 32, AUDIO_STREAM_WRITER);
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_cfg.need_expand = (16 != 32);
    i2s_cfg.out_rb_size = 8 * 1024;
    i2s_stream_set_channel_type(&i2s_cfg, I2S_CHANNEL_TYPE_ONLY_LEFT);
    i2s_cfg.buffer_len = 708;
    player_pipeline->i2s_stream_writer = i2s_stream_init(&i2s_cfg);

#ifdef CONFIG_AUDIO_SUPPORT_OPUS_DECODER
    ESP_LOGI(TAG, "[3.3] Create opus decoder");
    raw_opus_dec_cfg_t opus_dec_cfg = DEFAULT_OPUS_DECODER_CONFIG();
    opus_dec_cfg.samp_rate = DEC_SAMPLE_RATE;
    opus_dec_cfg.dec_frame_size = DEC_BIT_RATE * FRAME_TIME_MS / 1000;
    player_pipeline->audio_decoder = raw_opus_decoder_init(&opus_dec_cfg);
#elif CONFIG_AUDIO_SUPPORT_AAC_DECODER
    ESP_LOGI(TAG, "[3.3] Create aac decoder");
    aac_decoder_cfg_t aac_dec_cfg = DEFAULT_AAC_DECODER_CONFIG();
    player_pipeline->audio_decoder = aac_decoder_init(&aac_dec_cfg);
#elif CONFIG_AUDIO_SUPPORT_G711A_DECODER
    g711_decoder_cfg_t g711_dec_cfg = DEFAULT_G711_DECODER_CONFIG();
    g711_dec_cfg.out_rb_size = 8 * 1024;
    player_pipeline->audio_decoder = g711_decoder_init(&g711_dec_cfg);
#endif

    ESP_LOGI(TAG, "[3.4] Register all elements to audio pipeline");
    audio_pipeline_register(player_pipeline->audio_pipeline, player_pipeline->raw_writer, "raw");
    audio_pipeline_register(player_pipeline->audio_pipeline, player_pipeline->audio_decoder, "dec");
    audio_pipeline_register(player_pipeline->audio_pipeline, player_pipeline->i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[3.5] Link it together raw-->audio_decoder-->i2s_stream-->[codec_chip]");
    const char *link_tag[3] = {"raw", "dec", "i2s"};
    audio_pipeline_link(player_pipeline->audio_pipeline, &link_tag[0], 3);

    esp_timer_create_args_t create_args = {.callback = poll_audio_timer_callback, .arg = player_pipeline, .name = "pool audio timer"};
    int ret = esp_timer_create(&create_args, &player_pipeline->poll_audio_timer);
    ESP_LOGI(TAG, "esp_timer_create ret %d", ret);
    ret = esp_timer_start_periodic(player_pipeline->poll_audio_timer, 20 * 1000);
    ESP_LOGI(TAG, "esp_timer_start_periodic ret %d", ret);
    return player_pipeline;
}

void player_pipeline_run(player_pipeline_handle_t player_pipeline)
{
    audio_pipeline_run(player_pipeline->audio_pipeline);
};

void player_pipeline_close(player_pipeline_handle_t player_pipeline)
{
    audio_pipeline_stop(player_pipeline->audio_pipeline);
    audio_pipeline_wait_for_stop(player_pipeline->audio_pipeline);
    audio_pipeline_terminate(player_pipeline->audio_pipeline);

    audio_pipeline_unregister(player_pipeline->audio_pipeline, player_pipeline->raw_writer);
    audio_pipeline_unregister(player_pipeline->audio_pipeline, player_pipeline->i2s_stream_writer);
    audio_pipeline_unregister(player_pipeline->audio_pipeline, player_pipeline->audio_decoder);

    audio_pipeline_deinit(player_pipeline->audio_pipeline);
    audio_element_deinit(player_pipeline->raw_writer);
    audio_element_deinit(player_pipeline->i2s_stream_writer);
    audio_element_deinit(player_pipeline->audio_decoder);
    player_thread_data_stop(player_pipeline->thread_data);
    player_thread_data_destory(player_pipeline->thread_data);

    esp_timer_stop(player_pipeline->poll_audio_timer);
    esp_timer_delete(player_pipeline->poll_audio_timer);
    heap_caps_free(player_pipeline);
};

#define RTC_PLAY_DATA_BUFFER_SIZE 16000
unsigned char *get_audio_play_data_buffer(int buffer_size)
{
    static unsigned char play_data_buffer[RTC_PLAY_DATA_BUFFER_SIZE];
    static int buffer_index = 0;
    if (buffer_index + buffer_size >= RTC_PLAY_DATA_BUFFER_SIZE)
    {
        buffer_index = 0;
    }

    unsigned char *ret = play_data_buffer + buffer_index;
    buffer_index += buffer_size;
    return ret;
}
static int64_t last_ts = 0;
int player_pipeline_write(player_pipeline_handle_t player_pipeline, char *buffer, int buf_size)
{

    int current_ts = esp_timer_get_time() / 1000;
    if (last_ts != 0 && current_ts - last_ts >= 800)
    {
        player_pipeline_write_play_buffer_flag(player_pipeline);
    }
    last_ts = current_ts;

    queue_item qitem;
    // assert(qitem != 0);
    qitem.buffer = audio_calloc(1, buf_size);
    assert(qitem.buffer != 0);
    memcpy(qitem.buffer, buffer, buf_size);
    qitem.size = buf_size;
    xQueueSend(player_pipeline->thread_data->audio_queue, &qitem, 0);
    // printf("write to player thread %d ...\n",buf_size);
    player_pipeline->thread_data->play_packet_count--;
    return 0;
};

void player_pipeline_write_play_buffer_flag(player_pipeline_handle_t player_pipeline)
{
    queue_item qitem;
    qitem.buffer = &play_buffer_flag;
    qitem.size = 4;
    xQueueSend(player_pipeline->thread_data->audio_queue, &qitem, 0);
};
