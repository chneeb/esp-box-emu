#include "custom-bsp.hpp"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_ili9341.h>
#include <driver/gpio.h>
#include <freertos/task.h>

static const char *TAG = "custom-bsp";

// ---------------------------------------------------------------------------
// LVGL tick — called every 1 ms by an esp_timer
// ---------------------------------------------------------------------------
void CustomBsp::lvgl_tick_cb(void * /*arg*/) {
  lv_tick_inc(1);
}

// ---------------------------------------------------------------------------
// LVGL flush callback — called by LVGL when it wants to push pixels
// ---------------------------------------------------------------------------
void CustomBsp::lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  auto *self = static_cast<CustomBsp *>(lv_display_get_user_data(disp));
  esp_lcd_panel_draw_bitmap(self->panel_,
                            area->x1, area->y1,
                            area->x2 + 1, area->y2 + 1,
                            px_map);
  // With double-buffered LVGL draw buffers, LVGL switches to the other buffer
  // immediately on flush_ready, so it is safe to signal here without waiting
  // for DMA completion.
  lv_display_flush_ready(disp);
}

// ---------------------------------------------------------------------------
// initialize_lcd — configure i80 bus + ILI9341 panel
// ---------------------------------------------------------------------------
bool CustomBsp::initialize_lcd() {
  ESP_LOGI(TAG, "Initializing ILI9341 8-bit parallel LCD");

  // RD is write-only in our use case; pull it HIGH permanently.
  gpio_config_t rd_cfg = {};
  rd_cfg.pin_bit_mask   = (1ULL << LCD_RD);
  rd_cfg.mode           = GPIO_MODE_OUTPUT;
  rd_cfg.pull_up_en     = GPIO_PULLUP_DISABLE;
  rd_cfg.pull_down_en   = GPIO_PULLDOWN_DISABLE;
  rd_cfg.intr_type      = GPIO_INTR_DISABLE;
  gpio_config(&rd_cfg);
  gpio_set_level(LCD_RD, 1);

  // i80 parallel bus
  esp_lcd_i80_bus_config_t bus_cfg = {};
  bus_cfg.dc_gpio_num          = LCD_DC;
  bus_cfg.wr_gpio_num          = LCD_WR;
  bus_cfg.clk_src              = LCD_CLK_SRC_DEFAULT;
  bus_cfg.data_gpio_nums[0]    = LCD_D0;
  bus_cfg.data_gpio_nums[1]    = LCD_D1;
  bus_cfg.data_gpio_nums[2]    = LCD_D2;
  bus_cfg.data_gpio_nums[3]    = LCD_D3;
  bus_cfg.data_gpio_nums[4]    = LCD_D4;
  bus_cfg.data_gpio_nums[5]    = LCD_D5;
  bus_cfg.data_gpio_nums[6]    = LCD_D6;
  bus_cfg.data_gpio_nums[7]    = LCD_D7;
  bus_cfg.bus_width            = 8;
  // Max transfer: full screen at 16 bpp + small overhead
  bus_cfg.max_transfer_bytes   = lcd_width() * lcd_height() * sizeof(Pixel) + 8;

  if (esp_lcd_new_i80_bus(&bus_cfg, &bus_) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create i80 bus");
    return false;
  }

  // Panel IO (controls CS, clock speed, command/data framing)
  esp_lcd_panel_io_i80_config_t io_cfg = {};
  io_cfg.cs_gpio_num            = LCD_CS;
  io_cfg.pclk_hz                = 20 * 1000 * 1000; // 20 MHz — breadboard wiring limits effective throughput to ~10 MHz regardless of higher settings
  io_cfg.trans_queue_depth      = 1; // depth=1: each draw_bitmap blocks until DMA completes, preventing buffer overwrite
  io_cfg.lcd_cmd_bits           = 8;
  io_cfg.lcd_param_bits         = 8;
  io_cfg.flags.swap_color_bytes = 1; // lv_color_to_u16/make_color produce LE; swap to BE for ILI9341
  io_cfg.dc_levels.dc_idle_level  = 0;
  io_cfg.dc_levels.dc_cmd_level   = 0;
  io_cfg.dc_levels.dc_dummy_level = 0;
  io_cfg.dc_levels.dc_data_level  = 1;

  if (esp_lcd_new_panel_io_i80(bus_, &io_cfg, &io_) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create panel IO");
    return false;
  }

  // ILI9341 panel device
  esp_lcd_panel_dev_config_t panel_cfg = {};
  panel_cfg.reset_gpio_num   = LCD_RST;
  panel_cfg.rgb_ele_order    = LCD_RGB_ELEMENT_ORDER_BGR;
  panel_cfg.data_endian      = LCD_RGB_DATA_ENDIAN_BIG; // after swap_color_bytes, data arrives big-endian
  panel_cfg.bits_per_pixel   = 16;

  if (esp_lcd_new_panel_ili9341(io_, &panel_cfg, &panel_) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to create ILI9341 panel");
    return false;
  }

  esp_lcd_panel_reset(panel_);
  vTaskDelay(pdMS_TO_TICKS(120)); // ILI9341 requires 120 ms after RST deasserts
  esp_lcd_panel_init(panel_);
  esp_lcd_panel_swap_xy(panel_, true);   // landscape: swap X and Y axes
  esp_lcd_panel_mirror(panel_, false, false); // no flip needed for this wiring
  esp_lcd_panel_invert_color(panel_, false);
  esp_lcd_panel_disp_on_off(panel_, true);

  ESP_LOGI(TAG, "ILI9341 initialized OK");
  return true;
}

// ---------------------------------------------------------------------------
// initialize_display — allocate VRAM + start LVGL
// ---------------------------------------------------------------------------
bool CustomBsp::initialize_display(size_t pixel_buffer_size) {
  ESP_LOGI(TAG, "Initializing LVGL (pixel_buffer_size=%zu pixels)", pixel_buffer_size);

  if (!panel_) {
    ESP_LOGE(TAG, "Call initialize_lcd() before initialize_display()");
    return false;
  }

  // Emulator VRAM buffers (double-buffered, allocated in SPIRAM)
  vram0_ = static_cast<Pixel *>(
      heap_caps_malloc(pixel_buffer_size * sizeof(Pixel), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  vram1_ = static_cast<Pixel *>(
      heap_caps_malloc(pixel_buffer_size * sizeof(Pixel), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!vram0_ || !vram1_) {
    ESP_LOGE(TAG, "Failed to allocate emulator VRAM buffers");
    return false;
  }

  // Frame buffers used by emulator cores (palette-indexed staging + hw draw surface).
  // Sized for the largest emulator frame: lcd_width * lcd_height * 2 bytes.
  static constexpr size_t frame_buf_bytes = lcd_width() * lcd_height() * 2;
  frame_buffer0_ = static_cast<uint8_t *>(
      heap_caps_malloc(frame_buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  frame_buffer1_ = static_cast<uint8_t *>(
      heap_caps_malloc(frame_buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!frame_buffer0_ || !frame_buffer1_) {
    ESP_LOGE(TAG, "Failed to allocate emulator frame buffers");
    return false;
  }

  // LVGL draw buffers — full-screen size for LV_DISPLAY_RENDER_MODE_FULL.
  // Full-screen rendering flushes the entire frame in one draw_bitmap call,
  // eliminating partial-update tile boundaries that cause flickering stripes.
  static constexpr size_t lvgl_buf_pixels = lcd_width() * lcd_height();
  void *lvgl_buf1 = heap_caps_malloc(lvgl_buf_pixels * sizeof(Pixel), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  void *lvgl_buf2 = heap_caps_malloc(lvgl_buf_pixels * sizeof(Pixel), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!lvgl_buf1 || !lvgl_buf2) {
    ESP_LOGE(TAG, "Failed to allocate LVGL draw buffers");
    return false;
  }

  // Clear GRAM to black before LVGL starts. Uses vram0_ (persistent, no free)
  // so there is no use-after-free. This also primes the i80 DMA path; without
  // at least one draw_bitmap call here the DMA does not deliver LVGL flushes.
  {
    const size_t rows_per_batch = pixel_buffer_size / lcd_width();
    memset(vram0_, 0, pixel_buffer_size * sizeof(Pixel));
    for (int y = 0; y < (int)lcd_height(); y += (int)rows_per_batch) {
      int rows = std::min((int)rows_per_batch, (int)lcd_height() - y);
      esp_lcd_panel_draw_bitmap(panel_, 0, y, lcd_width(), y + rows, vram0_);
    }
  }

  lv_init();

  lv_display_t *disp = lv_display_create(lcd_width(), lcd_height());
  if (!disp) {
    ESP_LOGE(TAG, "lv_display_create failed");
    return false;
  }

  // LVGL 9.x: draw buffers must be wrapped in lv_draw_buf_t.
  // Use plain RGB565 (little-endian); the i80 swap_color_bytes=1 flag swaps to
  // big-endian before transmission, which is what ILI9341 expects.
  static lv_draw_buf_t draw_buf1, draw_buf2;
  uint32_t buf_bytes = lvgl_buf_pixels * sizeof(Pixel);
  lv_draw_buf_init(&draw_buf1, lcd_width(), lcd_height(), LV_COLOR_FORMAT_RGB565,
                   LV_STRIDE_AUTO, lvgl_buf1, buf_bytes);
  lv_draw_buf_init(&draw_buf2, lcd_width(), lcd_height(), LV_COLOR_FORMAT_RGB565,
                   LV_STRIDE_AUTO, lvgl_buf2, buf_bytes);

  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  lv_display_set_draw_buffers(disp, &draw_buf1, &draw_buf2);
  lv_display_set_render_mode(disp, LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(disp, lvgl_flush_cb);
  lv_display_set_user_data(disp, this);

  display_wrapper_.lv_disp = disp;

  // 1 ms tick timer — the Gui class drives lv_task_handler() itself
  esp_timer_handle_t tick_timer;
  const esp_timer_create_args_t tick_args = {
      .callback         = lvgl_tick_cb,
      .arg              = nullptr,
      .dispatch_method  = ESP_TIMER_TASK,
      .name             = "lvgl_tick",
      .skip_unhandled_events = true,
  };
  esp_timer_create(&tick_args, &tick_timer);
  esp_timer_start_periodic(tick_timer, 1000 /* µs = 1 ms */);

  ESP_LOGI(TAG, "LVGL initialized OK");
  return true;
}

// ---------------------------------------------------------------------------
// write_lcd_frame — direct panel write, bypasses LVGL (used by emulator)
// ---------------------------------------------------------------------------
void CustomBsp::write_lcd_frame(int x, int y, int width, int height, uint8_t *data) {
  esp_lcd_panel_draw_bitmap(panel_, x, y, x + width, y + height, data);
}
