#pragma once

#include <driver/gpio.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_types.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include <functional>
#include <vector>

// Minimal LVGL display wrapper matching the espp::Display interface used by the project.
class CustomDisplay {
public:
  lv_display_t *lv_disp{nullptr};

  void force_refresh() {
    if (lv_disp) {
      lv_obj_invalidate(lv_display_get_screen_active(lv_disp));
    }
  }
};

// Custom BSP for ESP32-S3 Dev Module with ILI9341 8-bit parallel display.
// Implements the same interface as espp::EspBox so that box-emu.hpp can use it
// as a drop-in replacement via: using Bsp = CustomBsp;
class CustomBsp {
public:
  // ---- Types expected by box-emu.hpp ----------------------------------------
  using Pixel = lv_color16_t;         // 16-bit RGB565
  using DisplayDriver = CustomDisplay;

  struct TouchpadData {
    uint16_t x{0};
    uint16_t y{0};
    bool touched{false};
    uint8_t num_touch_points{0};
    bool btn_state{false}; // no physical touch button on custom hardware
  };

  TouchpadData touchpad_data() const { return {}; }
  using button_callback_t = std::function<void(bool)>;
  using touch_callback_t  = std::function<void(TouchpadData)>;

  // ---- Singleton -------------------------------------------------------------
  static CustomBsp &get() {
    static CustomBsp instance;
    return instance;
  }
  CustomBsp(const CustomBsp &)            = delete;
  CustomBsp &operator=(const CustomBsp &) = delete;
  CustomBsp(CustomBsp &&)                 = delete;
  CustomBsp &operator=(CustomBsp &&)      = delete;

  // ---- Display geometry -----------------------------------------------------
  static constexpr size_t lcd_width()  { return 320; }
  static constexpr size_t lcd_height() { return 240; }

  // ---- Hardware init --------------------------------------------------------
  // No audio codec on this hardware — return true (success/no-op).
  bool initialize_sound() { return true; }
  bool initialize_lcd();
  bool initialize_display(size_t pixel_buffer_size);
  // No touchscreen on this hardware.
  bool initialize_touch() { return true; }

  // ---- No mute button -------------------------------------------------------
  static constexpr gpio_num_t get_mute_pin() { return GPIO_NUM_NC; }

  // ---- Pixel buffers used by emulator video task ----------------------------
  Pixel *vram0() { return vram0_; }
  Pixel *vram1() { return vram1_; }
  uint8_t *frame_buffer0() { return frame_buffer0_; }
  uint8_t *frame_buffer1() { return frame_buffer1_; }

  // ---- Direct LCD write (bypasses LVGL, used by emulator video task) --------
  void write_lcd_frame(int x, int y, int width, int height, uint8_t *data);

  // ---- Audio stubs (no codec) -----------------------------------------------
  void     mute(bool)                          {}
  bool     is_muted()                    const { return false; }
  void     volume(float v)                     { volume_ = v; }
  float    volume()                      const { return volume_; }
  void     audio_sample_rate(int r)            { sample_rate_ = r; }
  uint32_t audio_sample_rate()           const { return sample_rate_; }
  void     play_audio(const uint8_t *, size_t) {}
  void     play_audio(const std::vector<uint8_t> &) {}

  // ---- Backlight (stub — could drive a PWM pin later) -----------------------
  void  brightness(float v) { brightness_ = v; }
  float brightness()  const { return brightness_; }

  // ---- LVGL display handle --------------------------------------------------
  DisplayDriver *display() { return &display_wrapper_; }

private:
  CustomBsp() = default;

  static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
  static void lvgl_tick_cb(void *arg);

  // ---- LCD panel handles ----------------------------------------------------
  esp_lcd_panel_handle_t    panel_{nullptr};
  esp_lcd_panel_io_handle_t io_{nullptr};
  esp_lcd_i80_bus_handle_t  bus_{nullptr};

  // ---- Pixel buffers --------------------------------------------------------
  Pixel *vram0_{nullptr};
  Pixel *vram1_{nullptr};
  // frame_buffer0/1: emulator-specific staging (palette-indexed or RGB565).
  // Sized for the largest emulator frame: lcd_width()*lcd_height()*2 bytes.
  uint8_t *frame_buffer0_{nullptr};
  uint8_t *frame_buffer1_{nullptr};

  // ---- LVGL display wrapper -------------------------------------------------
  CustomDisplay display_wrapper_;

  // ---- Audio state (stubs) --------------------------------------------------
  float    volume_{50.0f};
  uint32_t sample_rate_{16000};
  float    brightness_{1.0f};

  // ---- ILI9341 GPIO assignments ---------------------------------------------
  static constexpr gpio_num_t LCD_DC  = GPIO_NUM_7;
  static constexpr gpio_num_t LCD_CS  = GPIO_NUM_6;
  static constexpr gpio_num_t LCD_WR  = GPIO_NUM_1;
  static constexpr gpio_num_t LCD_RD  = GPIO_NUM_2;
  static constexpr gpio_num_t LCD_RST = GPIO_NUM_5;
  static constexpr int LCD_D0 = 21;
  static constexpr int LCD_D1 = 46;
  static constexpr int LCD_D2 = 18;
  static constexpr int LCD_D3 = 17;
  static constexpr int LCD_D4 = 19;
  static constexpr int LCD_D5 = 20;
  static constexpr int LCD_D6 = 3;
  static constexpr int LCD_D7 = 14;
};
