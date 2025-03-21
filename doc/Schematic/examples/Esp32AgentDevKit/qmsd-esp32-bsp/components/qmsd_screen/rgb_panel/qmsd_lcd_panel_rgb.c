/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stdarg.h>
#include <sys/cdefs.h>
#include <inttypes.h>
#include <sys/param.h>
#include <string.h>
#include "esp_lcd_types.h"
#include "soc/soc_caps.h"

#if SOC_LCD_RGB_SUPPORTED

#if CONFIG_LCD_ENABLE_DEBUG_LOG
// The local log level must be defined before including esp_log.h
// Set the maximum log level for this source file
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_pm.h"
#include "esp_lcd_panel_interface.h"
#include "qmsd_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "esp_rom_gpio.h"
#include "esp_private/esp_clk.h"
#include "hal/dma_types.h"
#include "hal/gpio_hal.h"
#include "esp_private/gdma.h"
#include "driver/gpio.h"
#if ESP_IDF_VERSION_MAJOR == 5
#include "esp_private/periph_ctrl.h"
#if CONFIG_SPIRAM
#include "esp_psram.h"
#endif
#else
#include "driver/periph_ctrl.h"
#if CONFIG_SPIRAM
#include "spiram.h"
#endif
#endif
#include "esp_lcd_common.h"
#include "esp_timer.h"
#include "soc/lcd_periph.h"
#include "hal/lcd_hal.h"
#include "hal/lcd_ll.h"
#include "hal/gdma_ll.h"
#include "rom/cache.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 2)
#define lcd_periph_signals lcd_periph_rgb_signals
#endif

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
#define lcd_ll_set_data_width(x, y) lcd_ll_set_dma_read_stride(x, y)
#endif

#if CONFIG_LCD_RGB_ISR_IRAM_SAFE
#define LCD_RGB_INTR_ALLOC_FLAGS     (ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_INTRDISABLED)
#else
#define LCD_RGB_INTR_ALLOC_FLAGS     ESP_INTR_FLAG_INTRDISABLED
#endif

static const char *TAG = "lcd_panel.rgb";

typedef struct esp_rgb_panel_t esp_rgb_panel_t;

static esp_err_t rgb_panel_del(esp_lcd_panel_t *panel);
static esp_err_t rgb_panel_reset(esp_lcd_panel_t *panel);
static esp_err_t rgb_panel_init(esp_lcd_panel_t *panel);
static esp_err_t rgb_panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t rgb_panel_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t rgb_panel_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t rgb_panel_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t rgb_panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
#if ESP_IDF_VERSION_MAJOR == 5
static esp_err_t rgb_panel_disp_on_off(esp_lcd_panel_t *panel, bool off);
#endif
static uint32_t qmsd_lcd_hal_cal_pclk_freq(lcd_hal_context_t *hal, uint32_t src_freq_hz, uint32_t expect_pclk_freq_hz, int lcd_clk_flags);
static esp_err_t lcd_rgb_panel_select_clock_src(esp_rgb_panel_t *panel, user_rgb_clock_source_t clk_src);
static esp_err_t lcd_rgb_panel_create_trans_link(esp_rgb_panel_t *panel);
static esp_err_t lcd_rgb_panel_configure_gpio(esp_rgb_panel_t *panel, const qmsd_lcd_rgb_panel_config_t *panel_config);
static void lcd_rgb_panel_start_transmission(esp_rgb_panel_t *rgb_panel);
static void lcd_default_isr_handler(void *args);

struct esp_rgb_panel_t {
    esp_lcd_panel_t base;  // Base class of generic lcd panel
    int panel_id;          // LCD panel ID
    lcd_hal_context_t hal; // Hal layer object
    size_t data_width;     // Number of data lines
    size_t bits_per_pixel; // Color depth, in bpp
    size_t sram_trans_align;  // Alignment for framebuffer that allocated in SRAM
    size_t psram_trans_align; // Alignment for framebuffer that allocated in PSRAM
    int disp_gpio_num;     // Display control GPIO, which is used to perform action like "disp_off"
    intr_handle_t intr;    // LCD peripheral interrupt handle
    esp_pm_lock_handle_t pm_lock; // Power management lock
    size_t num_dma_nodes;  // Number of DMA descriptors that used to carry the frame buffer
    uint8_t *fbs[2];       // Frame buffers
    uint8_t cur_fb_index;  // Current frame buffer index (0 or 1)
    uint8_t cur_fb_index_hope;
    size_t fb_size;        // Size of frame buffer
    int data_gpio_nums[SOC_LCD_RGB_DATA_WIDTH]; // GPIOs used for data lines, we keep these GPIOs for action like "invert_color"
    uint32_t src_clk_hz;   // Peripheral source clock resolution
    qmsd_lcd_rgb_timing_t timings;   // RGB timing parameters (e.g. pclk, sync pulse, porch width)
    size_t bb_size;                 // If not-zero, the driver uses two bounce buffers allocated from internal memory
    int bounce_pos_px;              // Position in whatever source material is used for the bounce buffer, in pixels
    uint8_t *bounce_buffer[2];      // Pointer to the bounce buffers
    gdma_channel_handle_t dma_chan; // DMA channel handle
    qmsd_lcd_rgb_panel_vsync_cb_t on_vsync; // VSYNC event callback
    qmsd_lcd_rgb_panel_bounce_buf_fill_cb_t on_bounce_empty; // callback used to fill a bounce buffer rather than copying from the frame buffer
    void *user_ctx;                 // Reserved user's data of callback functions
    int x_gap;                      // Extra gap in x coordinate, it's used when calculate the flush window
    int y_gap;                      // Extra gap in y coordinate, it's used when calculate the flush window
    SemaphoreHandle_t flush_ready;
    SemaphoreHandle_t swap_ready;
    portMUX_TYPE spinlock;          // to protect panel specific resource from concurrent access (e.g. between task and ISR)
    struct {
        uint32_t disp_en_level: 1;       // The level which can turn on the screen by `disp_gpio_num`
        uint32_t stream_mode: 1;         // If set, the LCD transfers data continuously, otherwise, it stops refreshing the LCD when transaction done
        uint32_t no_fb: 1;               // No frame buffer allocated in the driver
        uint32_t fb_in_psram: 1;         // Whether the frame buffer is in PSRAM
        uint32_t need_update_pclk: 1;    // Whether to update the PCLK before start a new transaction
        uint32_t bb_invalidate_cache: 1; // Whether to do cache invalidation in bounce buffer mode
        uint32_t avoid_te: 1;
    } flags;
    dma_descriptor_t *dma_links[2];    // fbs[0] <-> dma_links[0], fbs[1] <-> dma_links[1]
    dma_descriptor_t dma_restart_node; // DMA descriptor used to restart the transfer
    dma_descriptor_t dma_nodes[];      // DMA descriptors pool
};

static esp_err_t lcd_rgb_panel_alloc_frame_buffers(const qmsd_lcd_rgb_panel_config_t *rgb_panel_config, esp_rgb_panel_t *rgb_panel)
{
    bool fb_in_psram = false;
    size_t psram_trans_align = rgb_panel_config->psram_trans_align ? rgb_panel_config->psram_trans_align : 64;
    size_t sram_trans_align = rgb_panel_config->sram_trans_align ? rgb_panel_config->sram_trans_align : 4;
    rgb_panel->psram_trans_align = psram_trans_align;
    rgb_panel->sram_trans_align = sram_trans_align;

    // alloc frame buffer
    if (!rgb_panel_config->flags.no_fb) {
        // fb_in_psram is only an option, if there's no PSRAM on board, we fallback to alloc from SRAM
        if (rgb_panel_config->flags.fb_in_psram) {
            fb_in_psram = true;
        }
        for (int i = 0; i < (rgb_panel_config->flags.double_fb ? 2 : 1); i++) {
            if (fb_in_psram) {
                // the low level malloc function will help check the validation of alignment
                rgb_panel->fbs[i] = heap_caps_aligned_calloc(psram_trans_align, 1, rgb_panel->fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            } else {
                rgb_panel->fbs[i] = heap_caps_aligned_calloc(sram_trans_align, 1, rgb_panel->fb_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            }
            ESP_RETURN_ON_FALSE(rgb_panel->fbs[i], ESP_ERR_NO_MEM, TAG, "no mem for frame buffer");
        }
    }

    // alloc bounce buffer
    if (rgb_panel->bb_size) {
        for (int i = 0; i < 2; i++) {
            // bounce buffer must come from SRAM
            rgb_panel->bounce_buffer[i] = heap_caps_aligned_calloc(sram_trans_align, 1, rgb_panel->bb_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
            ESP_RETURN_ON_FALSE(rgb_panel->bounce_buffer[i], ESP_ERR_NO_MEM, TAG, "no mem for bounce buffer");
        }
    }
    rgb_panel->cur_fb_index = 0;
    rgb_panel->flags.fb_in_psram = fb_in_psram;

    return ESP_OK;
}

static esp_err_t lcd_rgb_panel_destory(esp_rgb_panel_t *rgb_panel)
{
    lcd_ll_enable_clock(rgb_panel->hal.dev, false);
    if (rgb_panel->panel_id >= 0) {
        periph_module_disable(lcd_periph_signals.panels[rgb_panel->panel_id].module);
        lcd_com_remove_device(LCD_COM_DEVICE_TYPE_RGB, rgb_panel->panel_id);
    }
    if (rgb_panel->fbs[0]) {
        free(rgb_panel->fbs[0]);
    }
    if (rgb_panel->fbs[1]) {
        free(rgb_panel->fbs[1]);
    }
    if (rgb_panel->bounce_buffer[0]) {
        free(rgb_panel->bounce_buffer[0]);
    }
    if (rgb_panel->bounce_buffer[1]) {
        free(rgb_panel->bounce_buffer[1]);
    }
    if (rgb_panel->dma_chan) {
        gdma_disconnect(rgb_panel->dma_chan);
        gdma_del_channel(rgb_panel->dma_chan);
    }
    if (rgb_panel->intr) {
        esp_intr_free(rgb_panel->intr);
    }
    if (rgb_panel->pm_lock) {
        esp_pm_lock_release(rgb_panel->pm_lock);
        esp_pm_lock_delete(rgb_panel->pm_lock);
    }
    free(rgb_panel);
    return ESP_OK;
}

esp_err_t qmsd_lcd_new_rgb_panel(const qmsd_lcd_rgb_panel_config_t *rgb_panel_config, esp_lcd_panel_handle_t *ret_panel)
{
#if CONFIG_LCD_ENABLE_DEBUG_LOG
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
    esp_err_t ret = ESP_OK;
    esp_rgb_panel_t *rgb_panel = NULL;
    ESP_GOTO_ON_FALSE(rgb_panel_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid parameter");
    ESP_GOTO_ON_FALSE(rgb_panel_config->data_width == 16 || rgb_panel_config->data_width == 8,
                      ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported data width %d", rgb_panel_config->data_width);
    ESP_GOTO_ON_FALSE(!(rgb_panel_config->flags.double_fb && rgb_panel_config->flags.no_fb),
                      ESP_ERR_INVALID_ARG, err, TAG, "invalid frame buffer number");
    ESP_GOTO_ON_FALSE(!(rgb_panel_config->flags.no_fb && rgb_panel_config->bounce_buffer_size_px == 0),
                      ESP_ERR_INVALID_ARG, err, TAG, "must set bounce buffer if there's no frame buffer");
    ESP_GOTO_ON_FALSE(!(rgb_panel_config->flags.refresh_on_demand && rgb_panel_config->bounce_buffer_size_px),
                      ESP_ERR_INVALID_ARG, err, TAG, "refresh on demand is not supported under bounce buffer mode");
#if CONFIG_LCD_RGB_ISR_IRAM_SAFE
    ESP_GOTO_ON_FALSE(rgb_panel_config->bounce_buffer_size_px == 0,
                      ESP_ERR_INVALID_ARG, err, TAG, "bounce buffer mode is not IRAM Safe");
#endif

    // bpp defaults to the number of data lines, but for serial RGB interface, they're not equal
    size_t bits_per_pixel = rgb_panel_config->data_width;
    if (rgb_panel_config->bits_per_pixel) { // override bpp if it's set
        bits_per_pixel = rgb_panel_config->bits_per_pixel;
    }
    // calculate buffer size
    size_t fb_size = rgb_panel_config->timings.h_res * rgb_panel_config->timings.v_res * bits_per_pixel / 8;
    size_t bb_size = rgb_panel_config->bounce_buffer_size_px * bits_per_pixel / 8;
    if (bb_size) {
        // we want the bounce can always end in the second buffer
        ESP_GOTO_ON_FALSE(fb_size % (2 * bb_size) == 0, ESP_ERR_INVALID_ARG, err, TAG,
                          "fb size must be even multiple of bounce buffer size, fb_size: %d, bb_size: %d", fb_size, bb_size);
    }

    // calculate the number of DMA descriptors
    size_t num_dma_nodes = 0;
    if (bb_size) {
        // in bounce buffer mode, DMA is used to convey the bounce buffer, not the frame buffer.
        // frame buffer is copied to bounce buffer by CPU
        num_dma_nodes = (bb_size + DMA_DESCRIPTOR_BUFFER_MAX_SIZE - 1) / DMA_DESCRIPTOR_BUFFER_MAX_SIZE;
    } else {
        // Not bounce buffer mode, DMA descriptors need to fit the entire frame buffer
        num_dma_nodes = (fb_size + DMA_DESCRIPTOR_BUFFER_MAX_SIZE - 1) / DMA_DESCRIPTOR_BUFFER_MAX_SIZE;
    }

    // DMA descriptors must be placed in internal SRAM (requested by DMA)
    // multiply 2 because of double frame buffer mode (two frame buffer) and bounce buffer mode (two bounce buffer)
    rgb_panel = heap_caps_calloc(1, sizeof(esp_rgb_panel_t) + num_dma_nodes * sizeof(dma_descriptor_t) * 2,
                                 MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_GOTO_ON_FALSE(rgb_panel, ESP_ERR_NO_MEM, err, TAG, "no mem for rgb panel");
    rgb_panel->num_dma_nodes = num_dma_nodes;
    rgb_panel->fb_size = fb_size;
    rgb_panel->bb_size = bb_size;
    rgb_panel->panel_id = -1;
    // register to platform
    int panel_id = lcd_com_register_device(LCD_COM_DEVICE_TYPE_RGB, rgb_panel);
    ESP_GOTO_ON_FALSE(panel_id >= 0, ESP_ERR_NOT_FOUND, err, TAG, "no free rgb panel slot");
    rgb_panel->panel_id = panel_id;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    // enable APB to access LCD registers
    PERIPH_RCC_ACQUIRE_ATOMIC(lcd_periph_signals.panels[panel_id].module, ref_count) {
        if (ref_count == 0) {
            lcd_ll_enable_bus_clock(panel_id, true);
            lcd_ll_reset_register(panel_id);
        }
    }
#else
    periph_module_enable(lcd_periph_signals.panels[panel_id].module);
    periph_module_reset(lcd_periph_signals.panels[panel_id].module);
#endif
    // allocate frame buffers + bounce buffers
    ESP_GOTO_ON_ERROR(lcd_rgb_panel_alloc_frame_buffers(rgb_panel_config, rgb_panel), err, TAG, "alloc frame buffers failed");

    // initialize HAL layer, so we can call LL APIs later
    lcd_hal_init(&rgb_panel->hal, panel_id);
    // enable clock gating
    lcd_ll_enable_clock(rgb_panel->hal.dev, true);
    // set clock source
    ret = lcd_rgb_panel_select_clock_src(rgb_panel, rgb_panel_config->clk_src);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "set source clock failed");
    // install interrupt service, (LCD peripheral shares the interrupt source with Camera by different mask)
    int isr_flags = LCD_RGB_INTR_ALLOC_FLAGS | ESP_INTR_FLAG_SHARED | ESP_INTR_FLAG_LOWMED;
    ret = esp_intr_alloc_intrstatus(lcd_periph_signals.panels[panel_id].irq_id, isr_flags,
                                    (uint32_t)lcd_ll_get_interrupt_status_reg(rgb_panel->hal.dev),
                                    LCD_LL_EVENT_VSYNC_END, lcd_default_isr_handler, rgb_panel, &rgb_panel->intr);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "install interrupt failed");
    lcd_ll_enable_interrupt(rgb_panel->hal.dev, LCD_LL_EVENT_VSYNC_END, false); // disable all interrupts
    lcd_ll_clear_interrupt_status(rgb_panel->hal.dev, UINT32_MAX); // clear pending interrupt

    // install DMA service
    rgb_panel->flags.stream_mode = !rgb_panel_config->flags.refresh_on_demand;
    ret = lcd_rgb_panel_create_trans_link(rgb_panel);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "install DMA failed");
    // configure GPIO
    ret = lcd_rgb_panel_configure_gpio(rgb_panel, rgb_panel_config);
    ESP_GOTO_ON_ERROR(ret, err, TAG, "configure GPIO failed");
    // fill other rgb panel runtime parameters
    memcpy(rgb_panel->data_gpio_nums, rgb_panel_config->data_gpio_nums, SOC_LCD_RGB_DATA_WIDTH);
    rgb_panel->timings = rgb_panel_config->timings;
    rgb_panel->data_width = rgb_panel_config->data_width;
    rgb_panel->bits_per_pixel = bits_per_pixel;
    rgb_panel->disp_gpio_num = rgb_panel_config->disp_gpio_num;
    rgb_panel->flags.disp_en_level = !rgb_panel_config->flags.disp_active_low;
    rgb_panel->flags.no_fb = rgb_panel_config->flags.no_fb;
    rgb_panel->flags.bb_invalidate_cache = rgb_panel_config->flags.bb_invalidate_cache;
    rgb_panel->flags.avoid_te = rgb_panel_config->flags.avoid_te;
    rgb_panel->spinlock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    // fill function table
    rgb_panel->base.del = rgb_panel_del;
    rgb_panel->base.reset = rgb_panel_reset;
    rgb_panel->base.init = rgb_panel_init;
    rgb_panel->base.draw_bitmap = rgb_panel_draw_bitmap;
#if ESP_IDF_VERSION_MAJOR == 5
    rgb_panel->base.disp_on_off = rgb_panel_disp_on_off;
#endif
    rgb_panel->base.invert_color = rgb_panel_invert_color;
    rgb_panel->base.mirror = rgb_panel_mirror;
    rgb_panel->base.swap_xy = rgb_panel_swap_xy;
    rgb_panel->base.set_gap = rgb_panel_set_gap;
    rgb_panel->flush_ready = xSemaphoreCreateBinary();
    rgb_panel->swap_ready = xSemaphoreCreateBinary();
    // return base class
    *ret_panel = &(rgb_panel->base);
    ESP_LOGD(TAG, "new rgb panel(%d) @%p, fb0 @%p, fb1 @%p, fb_size=%zu, bb0 @%p, bb1 @%p, bb_size=%zu",
             rgb_panel->panel_id, rgb_panel, rgb_panel->fbs[0], rgb_panel->fbs[1], rgb_panel->fb_size,
             rgb_panel->bounce_buffer[0], rgb_panel->bounce_buffer[1], rgb_panel->bb_size);
    return ESP_OK;

err:
    if (rgb_panel) {
        lcd_rgb_panel_destory(rgb_panel);
    }
    return ret;
}

esp_err_t qmsd_lcd_rgb_panel_register_event_callbacks(esp_lcd_panel_handle_t panel, const qmsd_lcd_rgb_panel_event_callbacks_t *callbacks, void *user_ctx)
{
    ESP_RETURN_ON_FALSE(panel && callbacks, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
#if CONFIG_LCD_RGB_ISR_IRAM_SAFE
    if (callbacks->on_vsync) {
        ESP_RETURN_ON_FALSE(esp_ptr_in_iram(callbacks->on_vsync), ESP_ERR_INVALID_ARG, TAG, "on_vsync callback not in IRAM");
    }
    if (callbacks->on_bounce_empty) {
        ESP_RETURN_ON_FALSE(esp_ptr_in_iram(callbacks->on_bounce_empty), ESP_ERR_INVALID_ARG, TAG, "on_bounce_empty callback not in IRAM");
    }
    if (user_ctx) {
        ESP_RETURN_ON_FALSE(esp_ptr_internal(user_ctx), ESP_ERR_INVALID_ARG, TAG, "user context not in internal RAM");
    }
#endif // CONFIG_LCD_RGB_ISR_IRAM_SAFE
    rgb_panel->on_vsync = callbacks->on_vsync;
    rgb_panel->on_bounce_empty = callbacks->on_bounce_empty;
    rgb_panel->user_ctx = user_ctx;
    return ESP_OK;
}

esp_err_t qmsd_lcd_rgb_panel_set_pclk(esp_lcd_panel_handle_t panel, uint32_t freq_hz)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
    // the pclk frequency will be updated in the `LCD_LL_EVENT_VSYNC_END` event handler
    portENTER_CRITICAL(&rgb_panel->spinlock);
    rgb_panel->flags.need_update_pclk = true;
    rgb_panel->timings.pclk_hz = freq_hz;
    portEXIT_CRITICAL(&rgb_panel->spinlock);
    return ESP_OK;
}

esp_err_t qmsd_lcd_rgb_panel_get_frame_buffer(esp_lcd_panel_handle_t panel, uint32_t fb_num, void **fb0, ...)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(fb_num && fb_num <= 2, ESP_ERR_INVALID_ARG, TAG, "invalid frame buffer number");
    esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
    void **fb_itor = fb0;
    va_list args;
    va_start(args, fb0);
    for (int i = 0; i < fb_num; i++) {
        if (fb_itor) {
            *fb_itor = rgb_panel->fbs[i];
            fb_itor = va_arg(args, void **);
        }
    }
    va_end(args);
    return ESP_OK;
}

esp_err_t qmsd_lcd_rgb_panel_get_idle_frame_buffer(esp_lcd_panel_handle_t panel,  uint8_t **buffer) {
    esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
    if (rgb_panel->cur_fb_index == 0) {
        *buffer = rgb_panel->fbs[1];
    } else {
        *buffer = rgb_panel->fbs[0];
    }
    return ESP_OK;
}

esp_err_t qmsd_lcd_rgb_panel_get_running_frame_buffer(esp_lcd_panel_handle_t panel,  uint8_t **buffer) {
    esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
    if (rgb_panel->cur_fb_index == 0) {
        *buffer = rgb_panel->fbs[0];
    } else {
        *buffer = rgb_panel->fbs[1];
    }
    return ESP_OK;
}

esp_err_t qmsd_lcd_rgb_panel_refresh(esp_lcd_panel_handle_t panel)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
    ESP_RETURN_ON_FALSE(!rgb_panel->flags.stream_mode, ESP_ERR_INVALID_STATE, TAG, "refresh on demand is not enabled");
    lcd_rgb_panel_start_transmission(rgb_panel);
    return ESP_OK;
}

static esp_err_t rgb_panel_del(esp_lcd_panel_t *panel)
{
    esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
    int panel_id = rgb_panel->panel_id;
    ESP_RETURN_ON_ERROR(lcd_rgb_panel_destory(rgb_panel), TAG, "destroy rgb panel(%d) failed", panel_id);
    ESP_LOGD(TAG, "del rgb panel(%d)", panel_id);
    return ESP_OK;
}

static esp_err_t rgb_panel_reset(esp_lcd_panel_t *panel)
{
    esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
    lcd_ll_fifo_reset(rgb_panel->hal.dev);
    lcd_ll_reset(rgb_panel->hal.dev);
    return ESP_OK;
}

static esp_err_t rgb_panel_init(esp_lcd_panel_t *panel)
{
    esp_err_t ret = ESP_OK;
    esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);

    // set pixel clock frequency
    rgb_panel->timings.pclk_hz = qmsd_lcd_hal_cal_pclk_freq(&rgb_panel->hal, rgb_panel->src_clk_hz, rgb_panel->timings.pclk_hz, 0);
    // pixel clock phase and polarity
    lcd_ll_set_clock_idle_level(rgb_panel->hal.dev, rgb_panel->timings.flags.pclk_idle_high);
    lcd_ll_set_pixel_clock_edge(rgb_panel->hal.dev, rgb_panel->timings.flags.pclk_active_neg);
    // enable RGB mode and set data width
    lcd_ll_enable_rgb_mode(rgb_panel->hal.dev, true);
    lcd_ll_set_data_width(rgb_panel->hal.dev, rgb_panel->data_width);
    lcd_ll_set_phase_cycles(rgb_panel->hal.dev, 0, 0, 1); // enable data phase only
    // number of data cycles is controlled by DMA buffer size
    lcd_ll_enable_output_always_on(rgb_panel->hal.dev, true);
    // configure HSYNC, VSYNC, DE signal idle state level
    lcd_ll_set_idle_level(rgb_panel->hal.dev, !rgb_panel->timings.flags.hsync_idle_low,
                          !rgb_panel->timings.flags.vsync_idle_low, rgb_panel->timings.flags.de_idle_high);
    // configure blank region timing
    lcd_ll_set_blank_cycles(rgb_panel->hal.dev, 1, 1); // RGB panel always has a front and back blank (porch region)
    lcd_ll_set_horizontal_timing(rgb_panel->hal.dev, rgb_panel->timings.hsync_pulse_width,
                                 rgb_panel->timings.hsync_back_porch, rgb_panel->timings.h_res * rgb_panel->bits_per_pixel / rgb_panel->data_width,
                                 rgb_panel->timings.hsync_front_porch);
    lcd_ll_set_vertical_timing(rgb_panel->hal.dev, rgb_panel->timings.vsync_pulse_width,
                               rgb_panel->timings.vsync_back_porch, rgb_panel->timings.v_res,
                               rgb_panel->timings.vsync_front_porch);
    // output hsync even in porch region
    lcd_ll_enable_output_hsync_in_porch_region(rgb_panel->hal.dev, true);
    // generate the hsync at the very beginning of line
    lcd_ll_set_hsync_position(rgb_panel->hal.dev, 0);
    // send next frame automatically in stream mode
    lcd_ll_enable_auto_next_frame(rgb_panel->hal.dev, rgb_panel->flags.stream_mode);
    // trigger interrupt on the end of frame
    lcd_ll_enable_interrupt(rgb_panel->hal.dev, LCD_LL_EVENT_VSYNC_END, true);
    // enable intr
    esp_intr_enable(rgb_panel->intr);
    // start transmission
    if (rgb_panel->flags.stream_mode) {
        lcd_rgb_panel_start_transmission(rgb_panel);
    }
    ESP_LOGD(TAG, "rgb panel(%d) start, pclk=%" PRIu32 "Hz", rgb_panel->panel_id, rgb_panel->timings.pclk_hz);
    return ret;
}

static esp_err_t rgb_panel_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
    ESP_RETURN_ON_FALSE(!rgb_panel->flags.no_fb, ESP_ERR_NOT_SUPPORTED, TAG, "no frame buffer installed");
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    bool do_copy = false;

    // avoid bounce mode te
    if (rgb_panel->bb_size) {
        if (color_data == rgb_panel->fbs[0]) {
            rgb_panel->cur_fb_index_hope = 0;
        } else if (color_data == rgb_panel->fbs[1]) {
            rgb_panel->cur_fb_index_hope = 1;
        } else {
            goto normal;
        }

        if (rgb_panel->flags.avoid_te) {
            xSemaphoreTake(rgb_panel->swap_ready, 0);
            xSemaphoreTake(rgb_panel->swap_ready, portMAX_DELAY);
        }

        return ESP_OK;
    }

normal:
    // check if we need to copy the draw buffer (pointed by the color_data) to the driver's frame buffer
    if (color_data == rgb_panel->fbs[0]) {
        rgb_panel->cur_fb_index = 0;
    } else if (color_data == rgb_panel->fbs[1]) {
        rgb_panel->cur_fb_index = 1;
    } else {
        // we do the copy only if the color_data is different from either frame buffer
        do_copy = true;
    }

    // adjust the flush window by adding extra gap
    x_start += rgb_panel->x_gap;
    y_start += rgb_panel->y_gap;
    x_end += rgb_panel->x_gap;
    y_end += rgb_panel->y_gap;
    // round the boundary
    x_start = MIN(x_start, rgb_panel->timings.h_res);
    x_end = MIN(x_end, rgb_panel->timings.h_res);
    y_start = MIN(y_start, rgb_panel->timings.v_res);
    y_end = MIN(y_end, rgb_panel->timings.v_res);

    int bytes_per_pixel = rgb_panel->bits_per_pixel / 8;
    int pixels_per_line = rgb_panel->timings.h_res;
    uint32_t bytes_per_line = bytes_per_pixel * pixels_per_line;
    uint8_t *fb = rgb_panel->fbs[rgb_panel->cur_fb_index];

    if (do_copy) {
        // copy the UI draw buffer into internal frame buffer
        const uint8_t *from = (const uint8_t *)color_data;
        uint32_t copy_bytes_per_line = (x_end - x_start) * bytes_per_pixel;
        uint8_t *to = fb + (y_start * pixels_per_line + x_start) * bytes_per_pixel;
        for (int y = y_start; y < y_end; y++) {
            memcpy(to, from, copy_bytes_per_line);
            to += bytes_per_line;
            from += copy_bytes_per_line;
        }
    }

    if (rgb_panel->flags.fb_in_psram && !rgb_panel->bb_size) {
        // CPU writes data to PSRAM through DCache, data in PSRAM might not get updated, so write back
        // Note that if we use a bounce buffer, the data gets read by the CPU as well so no need to write back
        uint32_t bytes_to_flush = (y_end - y_start) * bytes_per_line;
        Cache_WriteBack_Addr((uint32_t)(fb + y_start * bytes_per_line), bytes_to_flush);
    }

    if (!rgb_panel->bb_size) {
        if (rgb_panel->flags.stream_mode) {
            int64_t time_start = esp_timer_get_time();
            // the DMA will convey the new frame buffer next time
            rgb_panel->dma_nodes[rgb_panel->num_dma_nodes - 1].next = rgb_panel->dma_links[rgb_panel->cur_fb_index];
            rgb_panel->dma_nodes[rgb_panel->num_dma_nodes * 2 - 1].next = rgb_panel->dma_links[rgb_panel->cur_fb_index];

            if (rgb_panel->flags.avoid_te) {
                xSemaphoreTake(rgb_panel->flush_ready, 0);
                xSemaphoreTake(rgb_panel->flush_ready, portMAX_DELAY);
                if (esp_timer_get_time() - time_start < 1000) {
                    xSemaphoreTake(rgb_panel->flush_ready, portMAX_DELAY);
                }
            }
        }
    }
    return ESP_OK;
}

static esp_err_t rgb_panel_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
    int panel_id = rgb_panel->panel_id;
    // inverting the data line by GPIO matrix
    for (int i = 0; i < rgb_panel->data_width; i++) {
        esp_rom_gpio_connect_out_signal(rgb_panel->data_gpio_nums[i], lcd_periph_signals.panels[panel_id].data_sigs[i],
                                        invert_color_data, false);
    }
    return ESP_OK;
}

static esp_err_t rgb_panel_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t rgb_panel_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t rgb_panel_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
    rgb_panel->x_gap = x_gap;
    rgb_panel->y_gap = y_gap;
    return ESP_OK;
}

#if ESP_IDF_VERSION_MAJOR == 5
static esp_err_t rgb_panel_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    esp_rgb_panel_t *rgb_panel = __containerof(panel, esp_rgb_panel_t, base);
    if (rgb_panel->disp_gpio_num < 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!on_off) { // turn off screen
        gpio_set_level(rgb_panel->disp_gpio_num, !rgb_panel->flags.disp_en_level);
    } else { // turn on screen
        gpio_set_level(rgb_panel->disp_gpio_num, rgb_panel->flags.disp_en_level);
    }
    return ESP_OK;
}
#endif

static esp_err_t lcd_rgb_panel_configure_gpio(esp_rgb_panel_t *panel, const qmsd_lcd_rgb_panel_config_t *panel_config)
{
    int panel_id = panel->panel_id;
    // check validation of GPIO number
    bool valid_gpio = (panel_config->pclk_gpio_num >= 0);
    if (panel_config->de_gpio_num < 0) {
        // Hsync and Vsync are required in HV mode
        valid_gpio = valid_gpio && (panel_config->hsync_gpio_num >= 0) && (panel_config->vsync_gpio_num >= 0);
    }
    for (size_t i = 0; i < panel_config->data_width; i++) {
        valid_gpio = valid_gpio && (panel_config->data_gpio_nums[i] >= 0);
    }
    if (!valid_gpio) {
        return ESP_ERR_INVALID_ARG;
    }
    // connect peripheral signals via GPIO matrix
    for (size_t i = 0; i < panel_config->data_width; i++) {
        gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[panel_config->data_gpio_nums[i]], PIN_FUNC_GPIO);
        gpio_set_direction(panel_config->data_gpio_nums[i], GPIO_MODE_OUTPUT);
        esp_rom_gpio_pad_set_drv(panel_config->data_gpio_nums[i], 0);
        esp_rom_gpio_connect_out_signal(panel_config->data_gpio_nums[i],
                                        lcd_periph_signals.panels[panel_id].data_sigs[i], false, false);
    }
    if (panel_config->hsync_gpio_num >= 0) {
        gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[panel_config->hsync_gpio_num], PIN_FUNC_GPIO);
        gpio_set_direction(panel_config->hsync_gpio_num, GPIO_MODE_OUTPUT);
        esp_rom_gpio_pad_set_drv(panel_config->hsync_gpio_num, 0);
        esp_rom_gpio_connect_out_signal(panel_config->hsync_gpio_num,
                                        lcd_periph_signals.panels[panel_id].hsync_sig, false, false);
    }
    if (panel_config->vsync_gpio_num >= 0) {
        gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[panel_config->vsync_gpio_num], PIN_FUNC_GPIO);
        gpio_set_direction(panel_config->vsync_gpio_num, GPIO_MODE_OUTPUT);
        esp_rom_gpio_pad_set_drv(panel_config->vsync_gpio_num, 0);
        esp_rom_gpio_connect_out_signal(panel_config->vsync_gpio_num,
                                        lcd_periph_signals.panels[panel_id].vsync_sig, false, false);
    }
    gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[panel_config->pclk_gpio_num], PIN_FUNC_GPIO);
    gpio_set_direction(panel_config->pclk_gpio_num, GPIO_MODE_OUTPUT);
    esp_rom_gpio_pad_set_drv(panel_config->pclk_gpio_num, 0);
    esp_rom_gpio_connect_out_signal(panel_config->pclk_gpio_num,
                                    lcd_periph_signals.panels[panel_id].pclk_sig, false, false);
    // DE signal might not be necessary for some RGB LCD
    if (panel_config->de_gpio_num >= 0) {
        gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[panel_config->de_gpio_num], PIN_FUNC_GPIO);
        gpio_set_direction(panel_config->de_gpio_num, GPIO_MODE_OUTPUT);
        esp_rom_gpio_pad_set_drv(panel_config->de_gpio_num, 0);
        esp_rom_gpio_connect_out_signal(panel_config->de_gpio_num,
                                        lcd_periph_signals.panels[panel_id].de_sig, false, false);
    }
    // disp enable GPIO is optional
    if (panel_config->disp_gpio_num >= 0) {
        gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[panel_config->disp_gpio_num], PIN_FUNC_GPIO);
        gpio_set_direction(panel_config->disp_gpio_num, GPIO_MODE_OUTPUT);
        esp_rom_gpio_pad_set_drv(panel_config->disp_gpio_num, 0);
        esp_rom_gpio_connect_out_signal(panel_config->disp_gpio_num, SIG_GPIO_OUT_IDX, false, false);
    }
    return ESP_OK;
}

static IRAM_ATTR bool lcd_rgb_panel_fill_bounce_buffer(esp_rgb_panel_t *panel, uint8_t *buffer)
{
    bool need_yield = false;
    int bytes_per_pixel = panel->bits_per_pixel / 8;
    if (panel->flags.no_fb) {
        if (panel->on_bounce_empty) {
            // We don't have a frame buffer here; we need to call a callback to refill the bounce buffer
            need_yield = panel->on_bounce_empty(&panel->base, buffer, panel->bounce_pos_px, panel->bb_size, panel->user_ctx);
        }
    } else {
        // We do have frame buffer; copy from there.
        // Note: if the cache is diabled, and accessing the PSRAM by DCACHE will crash.
        memcpy(buffer, &panel->fbs[panel->cur_fb_index][panel->bounce_pos_px * bytes_per_pixel], panel->bb_size);
        if (panel->flags.bb_invalidate_cache) {
            // We don't need the bytes we copied from the psram anymore
            // Make sure that if anything happened to have changed (because the line already was in cache) we write the data back.
            Cache_WriteBack_Addr((uint32_t)&panel->fbs[panel->cur_fb_index][panel->bounce_pos_px * bytes_per_pixel], panel->bb_size);
            // Invalidate the data.
            // Note: possible race: perhaps something on the other core can squeeze a write between this and the writeback,
            // in which case that data gets discarded.
            Cache_Invalidate_Addr((uint32_t)&panel->fbs[panel->cur_fb_index][panel->bounce_pos_px * bytes_per_pixel], panel->bb_size);
        }
    }
    panel->bounce_pos_px += panel->bb_size / bytes_per_pixel;
    // If the bounce pos is larger than the frame buffer size, wrap around so the next isr starts pre-loading the next frame.
    if (panel->bounce_pos_px >= panel->fb_size / bytes_per_pixel) {
        panel->bounce_pos_px = 0;
        panel->cur_fb_index = panel->cur_fb_index_hope;
        BaseType_t high_task_awoken = pdFALSE;
        xSemaphoreGiveFromISR(panel->swap_ready, &high_task_awoken);
        need_yield |= high_task_awoken;
    }
    if (!panel->flags.no_fb) {
        // Preload the next bit of buffer from psram
        Cache_Start_DCache_Preload((uint32_t)&panel->fbs[panel->cur_fb_index][panel->bounce_pos_px * bytes_per_pixel],
                                   panel->bb_size, 0);
    }
    return need_yield;
}

// This is called in bounce buffer mode, when one bounce buffer has been fully sent to the LCD peripheral.
static IRAM_ATTR bool lcd_rgb_panel_eof_handler(gdma_channel_handle_t dma_chan, gdma_event_data_t *event_data, void *user_data)
{
    esp_rgb_panel_t *panel = (esp_rgb_panel_t *)user_data;
    dma_descriptor_t *desc = (dma_descriptor_t *)event_data->tx_eof_desc_addr;
    // Figure out which bounce buffer to write to.
    // Note: what we receive is the *last* descriptor of this bounce buffer.
    int bb = (desc == &panel->dma_nodes[panel->num_dma_nodes - 1]) ? 0 : 1;
    return lcd_rgb_panel_fill_bounce_buffer(panel, panel->bounce_buffer[bb]);
}

// If we restart GDMA, many pixels already have been transferred to the LCD peripheral.
// Looks like that has 16 pixels of FIFO plus one holding register.
#define LCD_FIFO_PRESERVE_SIZE_PX (GDMA_LL_L2FIFO_BASE_SIZE + 1)

static esp_err_t lcd_rgb_panel_create_trans_link(esp_rgb_panel_t *panel)
{
    panel->dma_links[0] = &panel->dma_nodes[0];
    panel->dma_links[1] = &panel->dma_nodes[panel->num_dma_nodes];
    // chain DMA descriptors
    for (int i = 0; i < panel->num_dma_nodes * 2; i++) {
        panel->dma_nodes[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_CPU;
        panel->dma_nodes[i].next = &panel->dma_nodes[i + 1];
    }

    if (panel->bb_size) {
        // loop end back to start
        panel->dma_nodes[panel->num_dma_nodes * 2 - 1].next = &panel->dma_nodes[0];
        // mount the bounce buffers to the DMA descriptors
        lcd_com_mount_dma_data(panel->dma_links[0], panel->bounce_buffer[0], panel->bb_size);
        lcd_com_mount_dma_data(panel->dma_links[1], panel->bounce_buffer[1], panel->bb_size);
    } else {
        if (panel->flags.stream_mode) {
            // circle DMA descriptors chain for each frame buffer
            panel->dma_nodes[panel->num_dma_nodes - 1].next = &panel->dma_nodes[0];
            panel->dma_nodes[panel->num_dma_nodes * 2 - 1].next = &panel->dma_nodes[panel->num_dma_nodes];
        } else {
            // one-off DMA descriptors chain
            panel->dma_nodes[panel->num_dma_nodes - 1].next = NULL;
            panel->dma_nodes[panel->num_dma_nodes * 2 - 1].next = NULL;
        }
        // mount the frame buffer to the DMA descriptors
        lcd_com_mount_dma_data(panel->dma_links[0], panel->fbs[0], panel->fb_size);
        if (panel->fbs[1]) {
            lcd_com_mount_dma_data(panel->dma_links[1], panel->fbs[1], panel->fb_size);
        }
    }

#if CONFIG_LCD_RGB_RESTART_IN_VSYNC
    // On restart, the data sent to the LCD peripheral needs to start LCD_FIFO_PRESERVE_SIZE_PX pixels after the FB start
    // so we use a dedicated DMA node to restart the DMA transaction
    memcpy(&panel->dma_restart_node, &panel->dma_nodes[0], sizeof(panel->dma_restart_node));
    int restart_skip_bytes = LCD_FIFO_PRESERVE_SIZE_PX * sizeof(uint16_t);
    uint8_t *p = (uint8_t *)panel->dma_restart_node.buffer;
    panel->dma_restart_node.buffer = &p[restart_skip_bytes];
    panel->dma_restart_node.dw0.length -= restart_skip_bytes;
    panel->dma_restart_node.dw0.size -= restart_skip_bytes;
#endif
    // alloc DMA channel and connect to LCD peripheral
    gdma_channel_alloc_config_t dma_chan_config = {
        .direction = GDMA_CHANNEL_DIRECTION_TX,
    };

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
#if SOC_GDMA_TRIG_PERIPH_LCD0_BUS == SOC_GDMA_BUS_AHB
    ESP_RETURN_ON_ERROR(gdma_new_ahb_channel(&dma_chan_config, &panel->dma_chan), TAG, "alloc DMA channel failed");
#elif SOC_GDMA_TRIG_PERIPH_LCD0_BUS == SOC_GDMA_BUS_AXI
    ESP_RETURN_ON_ERROR(gdma_new_axi_channel(&dma_chan_config, &panel->dma_chan), TAG, "alloc DMA channel failed");
#endif
    gdma_connect(panel->dma_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0));

    // configure DMA transfer parameters
    gdma_transfer_config_t trans_cfg = {
        .max_data_burst_size = 64,
        .access_ext_mem = true, // frame buffer was allocated from external memory
    };
    ESP_RETURN_ON_ERROR(gdma_config_transfer(panel->dma_chan, &trans_cfg), TAG, "config DMA transfer failed");
#else
    // alloc DMA channel and connect to LCD peripheral
    ESP_RETURN_ON_ERROR(gdma_new_channel(&dma_chan_config, &panel->dma_chan), TAG, "alloc DMA channel failed");
    gdma_connect(panel->dma_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0));
    gdma_transfer_ability_t ability = {
        .psram_trans_align = panel->psram_trans_align,
        .sram_trans_align = panel->sram_trans_align,
    };
    gdma_set_transfer_ability(panel->dma_chan, &ability);
#endif

    // we need to refill the bounce buffer in the DMA EOF interrupt, so only register the callback for bounce buffer mode
    if (panel->bb_size) {
        gdma_tx_event_callbacks_t cbs = {
            .on_trans_eof = lcd_rgb_panel_eof_handler,
        };
        gdma_register_tx_event_callbacks(panel->dma_chan, &cbs, panel);
    }

    return ESP_OK;
}

#if CONFIG_LCD_RGB_RESTART_IN_VSYNC
static IRAM_ATTR void lcd_rgb_panel_restart_transmission_in_isr(esp_rgb_panel_t *panel)
{
    if (panel->bb_size) {
        // Catch de-synced frame buffer and reset if needed.
        if (panel->bounce_pos_px > panel->bb_size) {
            panel->bounce_pos_px = 0;
        }
        // Pre-fill bounce buffer 0, if the EOF ISR didn't do that already
        if (panel->bounce_pos_px < panel->bb_size / 2) {
            lcd_rgb_panel_fill_bounce_buffer(panel, panel->bounce_buffer[0]);
        }
    }

    gdma_reset(panel->dma_chan);
    // restart the DMA by a special DMA node
    gdma_start(panel->dma_chan, (intptr_t)&panel->dma_restart_node);

    if (panel->bb_size) {
        // Fill 2nd bounce buffer while 1st is being sent out, if needed.
        if (panel->bounce_pos_px < panel->bb_size) {
            lcd_rgb_panel_fill_bounce_buffer(panel, panel->bounce_buffer[0]);
        }
    }
}
#endif

static void lcd_rgb_panel_start_transmission(esp_rgb_panel_t *rgb_panel)
{
    // reset FIFO of DMA and LCD, incase there remains old frame data
    gdma_reset(rgb_panel->dma_chan);
    lcd_ll_stop(rgb_panel->hal.dev);
    lcd_ll_fifo_reset(rgb_panel->hal.dev);

    // pre-fill bounce buffers if needed
    if (rgb_panel->bb_size) {
        rgb_panel->bounce_pos_px = 0;
        lcd_rgb_panel_fill_bounce_buffer(rgb_panel, rgb_panel->bounce_buffer[0]);
        lcd_rgb_panel_fill_bounce_buffer(rgb_panel, rgb_panel->bounce_buffer[1]);
    }

    // the start of DMA should be prior to the start of LCD engine
    gdma_start(rgb_panel->dma_chan, (intptr_t)rgb_panel->dma_links[rgb_panel->cur_fb_index]);
    // delay 1us is sufficient for DMA to pass data to LCD FIFO
    // in fact, this is only needed when LCD pixel clock is set too high
    esp_rom_delay_us(1);
    // start LCD engine
    lcd_ll_start(rgb_panel->hal.dev);
}

IRAM_ATTR static void lcd_rgb_panel_try_update_pclk(esp_rgb_panel_t *rgb_panel)
{
    portENTER_CRITICAL_ISR(&rgb_panel->spinlock);
    if (unlikely(rgb_panel->flags.need_update_pclk)) {
        rgb_panel->flags.need_update_pclk = false;
        rgb_panel->timings.pclk_hz = qmsd_lcd_hal_cal_pclk_freq(&rgb_panel->hal, rgb_panel->src_clk_hz, rgb_panel->timings.pclk_hz, 0);
    }
    portEXIT_CRITICAL_ISR(&rgb_panel->spinlock);
}

IRAM_ATTR static void lcd_default_isr_handler(void *args)
{
    esp_rgb_panel_t *rgb_panel = (esp_rgb_panel_t *)args;
    bool need_yield = false;

    uint32_t intr_status = lcd_ll_get_interrupt_status(rgb_panel->hal.dev);
    lcd_ll_clear_interrupt_status(rgb_panel->hal.dev, intr_status);
    if (intr_status & LCD_LL_EVENT_VSYNC_END) {
        // call user registered callback
        if (rgb_panel->on_vsync) {
            if (rgb_panel->on_vsync(&rgb_panel->base, NULL, rgb_panel->user_ctx)) {
                need_yield = true;
            }
        }

        if (rgb_panel->flags.avoid_te) {
            BaseType_t high_task_awoken = pdFALSE;
            xSemaphoreGiveFromISR(rgb_panel->flush_ready, &high_task_awoken);
            need_yield |= high_task_awoken;
        }

        // check whether to update the PCLK frequency, it should be safe to update the PCLK frequency in the VSYNC interrupt
        lcd_rgb_panel_try_update_pclk(rgb_panel);

        if (rgb_panel->flags.stream_mode) {
#if CONFIG_LCD_RGB_RESTART_IN_VSYNC
            // reset the GDMA channel every VBlank to stop permanent desyncs from happening.
            // Note that this fix can lead to single-frame desyncs itself, as in: if this interrupt
            // is late enough, the display will shift as the LCD controller already read out the
            // first data bytes, and resetting DMA will re-send those. However, the single-frame
            // desync this leads to is preferable to the permanent desync that could otherwise
            // happen. It's also not super-likely as this interrupt has the entirety of the VBlank
            // time to reset DMA.
            lcd_rgb_panel_restart_transmission_in_isr(rgb_panel);
#endif
        }

    }

    if (need_yield) {
        portYIELD_FROM_ISR();
    }
}

#if ESP_IDF_VERSION_MAJOR == 5
static esp_err_t lcd_rgb_panel_select_clock_src(esp_rgb_panel_t *panel, user_rgb_clock_source_t clk_src)
{
    esp_err_t ret = ESP_OK;
    switch (clk_src) {
    case USER_RGB_CLK_SRC_PLL240M:
        panel->src_clk_hz = 240000000;
        panel->hal.dev->lcd_clock.lcd_clk_sel = 2;
        break;
    case USER_RGB_CLK_SRC_PLL160M:
        panel->src_clk_hz = 160000000;
        panel->hal.dev->lcd_clock.lcd_clk_sel = 3;
        break;
    case USER_RGB_CLK_SRC_XTAL:
        panel->src_clk_hz = esp_clk_xtal_freq();
        panel->hal.dev->lcd_clock.lcd_clk_sel = 1;
        break;
    default:
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, TAG, "unsupported clock source: %d", clk_src);
        break;
    }

    if (clk_src == USER_RGB_CLK_SRC_PLL160M || clk_src == USER_RGB_CLK_SRC_PLL240M) {
#if CONFIG_PM_ENABLE
        ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "rgb_panel", &panel->pm_lock);
        ESP_RETURN_ON_ERROR(ret, TAG, "create ESP_PM_APB_FREQ_MAX lock failed");
        // hold the lock during the whole lifecycle of RGB panel
        esp_pm_lock_acquire(panel->pm_lock);
        ESP_LOGD(TAG, "installed ESP_PM_APB_FREQ_MAX lock and hold the lock during the whole panel lifecycle");
#endif
    }
    return ret;
}

static uint32_t qmsd_lcd_hal_cal_pclk_freq(lcd_hal_context_t *hal, uint32_t src_freq_hz, uint32_t expect_pclk_freq_hz, int lcd_clk_flags)
{
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
    hal_utils_clk_div_t lcd_clk_div = {};
    uint32_t pclk_hz = lcd_hal_cal_pclk_freq(hal, src_freq_hz, expect_pclk_freq_hz, 0, &lcd_clk_div);
    LCD_CLOCK_SRC_ATOMIC() {
        lcd_ll_set_group_clock_coeff(hal->dev, lcd_clk_div.integer, lcd_clk_div.denominator, lcd_clk_div.numerator);
    }
#else
    uint32_t pclk_hz = lcd_hal_cal_pclk_freq(hal, src_freq_hz, expect_pclk_freq_hz, lcd_clk_flags);
#endif
    return pclk_hz;
}

#else
static uint32_t qmsd_lcd_hal_cal_pclk_freq(lcd_hal_context_t *hal, uint32_t src_freq_hz, uint32_t expect_pclk_freq_hz, int lcd_clk_flags)
{
    uint32_t pclk_prescale = src_freq_hz / expect_pclk_freq_hz;
    lcd_ll_set_pixel_clock_prescale(hal->dev, pclk_prescale);
    return src_freq_hz / pclk_prescale;
}

static inline void user_rgb_ll_set_group_clock_src(lcd_cam_dev_t *dev, user_rgb_clock_source_t src, int div_num, int div_a, int div_b) {
    // lcd_clk = module_clock_src / (div_num + div_b / div_a)
    HAL_ASSERT(div_num >= 2);
    HAL_FORCE_MODIFY_U32_REG_FIELD(dev->lcd_clock, lcd_clkm_div_num, div_num);
    dev->lcd_clock.lcd_clkm_div_a = div_a;
    dev->lcd_clock.lcd_clkm_div_b = div_b;
    switch (src) {
        case USER_RGB_CLK_SRC_PLL160M:
            dev->lcd_clock.lcd_clk_sel = 3;
            break;
        case USER_RGB_CLK_SRC_PLL240M:
            dev->lcd_clock.lcd_clk_sel = 2;
            break;
        case USER_RGB_CLK_SRC_XTAL:
            dev->lcd_clock.lcd_clk_sel = 1;
            break;
        default:
            HAL_ASSERT(false && "unsupported clock source");
            break;
    }
}

static esp_err_t lcd_rgb_panel_select_clock_src(esp_rgb_panel_t *panel, user_rgb_clock_source_t clk_src)
{
    esp_err_t ret = ESP_OK;
    user_rgb_ll_set_group_clock_src(panel->hal.dev, clk_src, LCD_PERIPH_CLOCK_PRE_SCALE, 1, 0);
    switch (clk_src) {
        case USER_RGB_CLK_SRC_PLL160M:
            panel->src_clk_hz = 160000000 / LCD_PERIPH_CLOCK_PRE_SCALE;
#if CONFIG_PM_ENABLE
            ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "rgb_panel", &panel->pm_lock);
            ESP_RETURN_ON_ERROR(ret, TAG, "create ESP_PM_APB_FREQ_MAX lock failed");
            // hold the lock during the whole lifecycle of RGB panel
            esp_pm_lock_acquire(panel->pm_lock);
            ESP_LOGD(TAG, "installed ESP_PM_APB_FREQ_MAX lock and hold the lock during the whole panel lifecycle");
#endif
            break;
        case USER_RGB_CLK_SRC_PLL240M:
            panel->src_clk_hz = 240000000 / LCD_PERIPH_CLOCK_PRE_SCALE;
            break;
        case USER_RGB_CLK_SRC_XTAL:
            panel->src_clk_hz = rtc_clk_xtal_freq_get() * 1000000;
            break;
        default:
            ESP_RETURN_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, TAG,  "unsupported clock source: %d", clk_src);
            break;
    }
    return ret;
}
#endif

#endif // SOC_LCD_RGB_SUPPORTED
