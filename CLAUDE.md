# esp-box-emu

Multi-system emulator (NES, GB/GBC, SMS, Game Gear, Genesis, MSX, Doom) for ESP32-S3,
built with ESP-IDF 5.5+. The UI is LVGL 9.2+; emulator cores are bundled as local components.

## Build

```bash
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

First build downloads managed components automatically via the IDF component manager.
If components are stale or missing, run `idf.py update-dependencies` first.

To exit `idf.py monitor`: `Ctrl+T` then `Ctrl+X`.

Key sdkconfig.defaults settings required for managed espp I2C component:
```
CONFIG_ESPP_I2C_USE_LEGACY_API=y
CONFIG_ESPP_I2C_LEGACY_API_DISABLE_DEPRECATION_WARNINGS=y
```

## Hardware targets

### Custom hardware (active target)
ESP32-S3 Dev Module · OPI PSRAM · 16 MB flash

| Peripheral | Interface | Pins |
|---|---|---|
| ILI9341 display | 8-bit parallel (i80) | DC=7, CS=6, WR=1, RD=2, RST=5, D0–D7=21,46,18,17,19,20,3,14 |
| CardKB keyboard | I2C (I2C_NUM_0) | SDA=8, SCL=9, addr=0x5F |
| NES Mini Classic clone | I2C (I2C_NUM_0) | SDA=8, SCL=9, addr=0x52 |
| SD card | SPI (SPI3_HOST) | SCK=12, MISO=13, MOSI=11, CS=10 |

BSP: `components/custom-bsp/` — `CustomBsp` class.
No audio codec, touchscreen, haptics, or battery monitor on this target.

### Original ESP32-S3-BOX / ESP32-S3-BOX-3 (reference)
Uses `espp::EspBox` BSP (removed from active build). ST7789 SPI display, ES8311 audio,
TT21100/GT911 touch, MCP23x17 (V0) or AW9523 (V1) GPIO expander for input.

## Key source files

| File | Purpose |
|---|---|
| `components/custom-bsp/` | Custom BSP: ILI9341 parallel driver + LVGL init |
| `components/box-emu/include/box-emu.hpp` | Hardware abstraction class; `using Bsp = CustomBsp` |
| `components/box-emu/src/box-emu.cpp` | Init functions: display, SD, gamepad, video, haptics |
| `main/main.cpp` | App entry point: init sequence → GUI loop → emulator run |
| `components/gui/` | LVGL-based ROM selection menu (Squareline Studio) |
| `components/menu/` | In-game pause menu |
| `partitions.csv` | Flash layout: NVS 24 KB, factory app 6 MB |
| `sdkconfig.defaults` | Build config: 240 MHz, 16 MB flash QIO 120 MHz, OPI PSRAM 120 MHz |

## Emulator components

| System | Component | Enable flag | File extensions |
|---|---|---|---|
| NES | `components/nes/` (Nofrendo) | `ENABLE_NES` | `.nes` |
| Game Boy / GBC | `components/gbc/` (GNUBoy) | `ENABLE_GBC` | `.gb` `.gbc` |
| Sega Master System / Game Gear | `components/sms/` (SMS Plus) | `ENABLE_SMS` | `.sms` `.gg` |
| Sega Genesis | `components/genesis/` (Gwenesis) | `ENABLE_GENESIS` | `.gen` `.md` |
| MSX | `components/msx/` (fMSX) | `ENABLE_MSX` | `.rom` |
| Doom | `components/doom/` (PrBoom) | `ENABLE_DOOM` | `.wad` |

Enable/disable emulators in the root `CMakeLists.txt` by commenting/uncommenting the `set(*_COMPONENTS ...)` lines.

## Emulator notes

### Genesis (Gwenesis) — `components/genesis/src/genesis.cpp`

- `lfo_pm_table` is declared `extern int32_t*` in `genesis.cpp` but `ym2612.c` is compiled with
  `#define GW_TARGET 1`, which makes it a `uint8_t*` of size `128*8*16 = 16384` bytes. Allocate
  exactly `128*8*16` bytes (not `128*8*32 * sizeof(int32_t)` = 128 KB). Allocating 128 KB can
  silently fail on SPIRAM-constrained boots and causes a `StoreProhibited` crash in `YM2612Init`.
- Both `M68K_RAM` and `lfo_pm_table` have NULL-checked allocations with `ESP_LOGE` fallback; both
  are nulled after `free()` in `deinit_genesis()`.

### MSX (fMSX) — `components/msx/src/msx.cpp`

- `-skip N` sets `UPeriod = 100 - N`; `RefreshScreen()` / `PutImage()` fires only when
  `UCount >= 100`. With `-skip 50`, `frame_complete` is set every 2 VBlanks → `run_msx_rom()`
  paces 2 frames as 1 → 2× game speed. Use `-skip 0` (UPeriod = 100, one signal per VBlank).

## SD card layout

```
/sdcard/
  nes/        ← .nes ROMs
  gbc/        ← .gb / .gbc ROMs
  sms/        ← .sms / .gg ROMs
  genesis/    ← .gen / .md ROMs
  msx/
    bios/     ← MSX BIOS files (MSX.ROM, MSX2.ROM, MSX2EXT.ROM, DISK.ROM, MSXDOS2.ROM)
    *.rom     ← MSX game ROMs
  doom/       ← .wad files
  boxart/     ← JPEG cover art, resized to exactly 100×165 px
  saves/      ← save states (auto-created)
  metadata.csv
```

`metadata.csv` format: `rom_path, boxart_path, display_name`
- Paths are relative to SD card root
- No commas allowed in rom or boxart filenames (CSV delimiter)
- Emulator detected purely from file extension
- Boxart panel is 100×165 px; images tile if smaller, clip if larger. Use ImageMagick: `convert input.jpg -resize 100x165! output.jpg`

## BSP interface (`CustomBsp` / `espp::EspBox`)

`box-emu.hpp` uses `using Bsp = CustomBsp`. Any replacement BSP must provide:

- `static Bsp& get()` — singleton
- `static constexpr size_t lcd_width() / lcd_height()`
- `static constexpr gpio_num_t get_mute_pin()` — return `GPIO_NUM_NC` if absent
- `bool initialize_sound() / initialize_lcd() / initialize_display(size_t) / initialize_touch()`
- `Pixel* vram0() / vram1()` — double-buffered pixel memory (SPIRAM)
- `void write_lcd_frame(int x, int y, int w, int h, uint8_t* data)`
- `DisplayDriver* display()` — must have `void force_refresh()`
- Audio stubs: `mute/is_muted/volume/audio_sample_rate/play_audio`
- `void brightness(float) / float brightness()`

## Input — CardKB key mapping

Edit `CardKbInput::key_to_pins()` in `box-emu.hpp` to remap keys.

| Key(s) | Button |
|---|---|
| ↑ / W | D-pad Up |
| ↓ / S | D-pad Down |
| ← / A | D-pad Left |
| → / D | D-pad Right |
| Z / Space | A |
| X / Backspace | B |
| Q | X |
| E | Y |
| Enter | Start |
| Tab | Select |

CardKB sends one key at a time — simultaneous inputs are not possible.

I2C is configured at 100 kHz (reduced from 400 kHz for reliability). On boot, absent peripherals
(DRV2605 at 0x5A, MAX1704x at 0x36) are probed and fail gracefully — their NACK failures can
leave the ESP32 I2C hardware in a stuck state. `initialize_gamepad()` calls `deinit()/init()`
on `external_i2c_` before creating `CardKbInput` to recover the bus.

Battery (`initialize_battery`) and haptics (`initialize_haptics`) are skipped entirely for
CARDKB hardware — guarded by `has_battery_and_haptics = (version != CARDKB)` in `main.cpp`.
This avoids all DRV2605/MAX1704x I2C traffic, which was causing bus contention and CardKB timeouts.

## Input — NES Mini Classic clone (Nunchuck)

`NunchuckInput` class in `box-emu.hpp`. Auto-detected at I2C addr 0x52 on the same bus as CardKB (SDA=8, SCL=9).

Init sequence sent once in constructor (order matters):
```
{0xF0, 0x55}
{0xFB, 0x00}
{0xFE, 0x03}  ← required for clone variant
```

Read sequence per poll: write `0x00`, wait 200 µs, read 8 bytes. Buttons are active low in bytes 6 and 7:

| Button | Byte | Mask |
|---|---|---|
| Up | 7 | 0x01 |
| Down | 6 | 0x40 |
| Left | 7 | 0x02 |
| Right | 6 | 0x80 |
| A | 7 | 0x10 |
| B | 7 | 0x40 |
| Start | 6 | 0x04 |
| Select | 6 | 0x10 |

Detection order in `detect()`: CardKB (0x5F) → Nunchuck (0x52) → V1 (AW9523) → V0 (MCP23x17). Only one input device is active at a time — whichever is detected first wins. Plug in the desired controller before boot.

## Video pipeline

The emulator video task runs on **core 1** at priority 20. It double-buffers into
`vram0`/`vram1` (SPIRAM) and calls `write_lcd_frame()` directly, bypassing LVGL.
LVGL runs on **core 0** at priority 5. The GUI is paused (`gui.pause()`) before
emulator startup to avoid concurrent LCD access.

`lvgl_flush_cb` calls `lv_display_flush_ready()` synchronously after `esp_lcd_panel_draw_bitmap`
(safe with double-buffered draw buffers — LVGL switches to the other buffer immediately).
Do NOT add a separate task calling `lv_task_handler()`; the Gui's `HighResolutionTimer` is
the sole driver of LVGL rendering. A second caller causes concurrent LCD writes and a black screen.

Frame scaling modes: `ORIGINAL`, `FIT`, `FILL` — toggled via the pause menu.
Row batch size: 30 rows per `write_lcd_frame()` call (`num_rows_in_framebuffer`).

## Display tuning

- Pixel clock: `pclk_hz` in `custom-bsp.cpp` — currently 20 MHz. Increasing beyond 20 MHz has no effect: breadboard wiring capacitance (~100 pF/node) limits the effective ILI9341 write throughput to ~10 MHz regardless of the requested clock (ILI9341 write cycle spec is 66 ns min ≈ 15 MHz max). At ~10 MHz effective rate a full 320×240 frame takes ~15 ms = 90% of the 16.7 ms display refresh, causing the ~7/8-screen tearing artifact.
- ILI9341 is initialized in landscape via `esp_lcd_panel_swap_xy(true)` + `mirror(false, false)`.
- RD pin (GPIO 2) is driven HIGH statically — display is write-only.
- LVGL draw buffers and emulator VRAM are all allocated in SPIRAM.
- LVGL draw buffers are full-screen (`lcd_width() × lcd_height()` pixels each) using `LV_DISPLAY_RENDER_MODE_FULL`. Full-screen rendering flushes the entire frame in one `draw_bitmap` call, eliminating partial-tile flickering stripes. **Critical**: `lv_draw_buf_init()` height must match the allocation — LVGL 9.5 validates `stride * h <= data_size` and returns `LV_RESULT_INVALID` (silent black screen) if mismatched.
- Color format: `LV_COLOR_FORMAT_RGB565` (little-endian native). Hardware byte-swap to big-endian for ILI9341 is done via `io_cfg.flags.swap_color_bytes = 1` in the i80 panel IO config. This applies uniformly to both LVGL flushes and emulator `write_lcd_frame()` calls. `panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR`.
- `make_color()` uses `lv_color_to_u16()` (little-endian). The i80 `swap_color_bytes` flag corrects byte order for all pixel data paths — do not add per-path byte swapping.
- **Known issue — screen tearing**: ILI9341 has no TE (Tearing Effect) pin wired. The display scans out GRAM at ~60 Hz while the CPU writes new frames. With `swap_xy=true` the physical scan direction maps to the logical X axis, so the tear appears as **vertical** bars rather than horizontal — most visible in the black left/right borders where the artifact is not masked by moving game content. Increasing `pclk_hz` reduces severity (faster writes = smaller overlap with scan cycle). The ILI9341 TE pin is not exposed on the Arduino shield connector, so hardware sync is not an option with this shield.

## ESP-IDF patches

The following local patches are applied to the ESP-IDF installation at `/home/chneeb/Source/esp-idf/`:

### `components/esp_lcd/i80/esp_lcd_panel_io_i80.c` — GDMA `check_owner = false`

**File**: `lcd_i80_init_dma_link()`, the `gdma_link_list_config_t` initializer (~line 634).

**Change**: `check_owner = true` → `check_owner = false`

**Why**: The i80 driver calls `gdma_link_mount_buffers(bus->dma_link, 0, ...)` from the `trans_done` ISR.
At that point the DMA is fully idle; no descriptor is in use. However, `auto_write_back` (hardware ownership
reset after each transfer) fires simultaneously with the `trans_done` interrupt. Due to a race between the
ISR and the writeback propagating through cache/memory, `gdma_link_mount_buffers` reads stale
`GDMA_LLI_OWNER_DMA` bits on all nodes and returns `ESP_ERR_INVALID_ARG` (logged as
`E gdma-link: lli full start=0 need=N avail=0`). The ISR does not check the return value, so
`lcd_start_transaction` then DMA's the stale descriptor from the previous transfer — corrupting the output.
With `check_owner=false` the count is always the full pool, eliminating the race. Safe here because the
i80 trans-done ISR is the only mount site and DMA is always stopped before it runs.

## Memory

- 4 MB SPIRAM reserved for ROM data (`romdata_` in `box-emu.cpp`)
- SPIRAM malloc threshold: 0 bytes (all allocations ≥ 0 bytes prefer SPIRAM)
- 256 KB internal RAM reserved for DMA (`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`)
- LVGL heap: 50 KB (`CONFIG_LV_MEM_SIZE_KILOBYTES`)
