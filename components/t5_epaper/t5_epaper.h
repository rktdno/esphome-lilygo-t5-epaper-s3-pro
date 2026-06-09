#pragma once
#include <cstring>
#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/core/automation.h"
#include "esphome/components/display/display.h"
#include <epdiy.h>
#include <epd_init_config.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"   // esp_restart()

namespace esphome {
namespace t5_epaper {

// ESPHome display + GT911 touch for the LilyGO T5 E-Paper S3 Pro (epd_board_v7 + ED047TC1), portrait
// 540x960. We create the I2C bus ourselves and hand it to epdiy via epd_init_with_config(), so epdiy's
// PCA9555/TPS power devices and our GT911 share one bus (epdiy holds I2C for life otherwise). ESPHome's
// loop()/update() run sequentially on one task, so shared-bus access never overlaps.
class T5Display : public display::Display {
 public:
  void setup() override {
    // 1) our shared I2C bus on the config pins (39/40)
    i2c_master_bus_config_t bc = {};
    bc.i2c_port = I2C_NUM_0;
    bc.sda_io_num = (gpio_num_t) 39;
    bc.scl_io_num = (gpio_num_t) 40;
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bc, &this->bus_) != ESP_OK) {
      ESP_LOGE("t5_epaper", "i2c bus create failed");
      this->mark_failed();
      return;
    }
    // 1b) hardware-reset the GT911 BEFORE epdiy claims GPIO9 (=RST, shared with EPD data D8).
    // The side RST button restarts the ESP but leaves the GT911 in a stuck/held state — it
    // stops answering at BOTH 0x5D and 0x14 even though the I2C bus + EPD still work. A clean
    // RST pulse (INT held low to latch 0x5D) re-boots the controller. Must run here: once
    // epd_init_with_config() binds GPIO9 to the LCD peripheral, reconfiguring it as plain GPIO
    // would detach it from the LCD and break rendering.
    gpio_set_direction((gpio_num_t) 9, GPIO_MODE_OUTPUT);  // RST
    gpio_set_direction((gpio_num_t) 3, GPIO_MODE_OUTPUT);  // INT
    gpio_set_level((gpio_num_t) 3, 0);   // INT low -> GT911 latches slave address 0x5D
    gpio_set_level((gpio_num_t) 9, 0);   // assert RST
    vTaskDelay(pdMS_TO_TICKS(12));
    gpio_set_level((gpio_num_t) 9, 1);   // release RST (address sampled from INT here)
    vTaskDelay(pdMS_TO_TICKS(6));
    gpio_set_direction((gpio_num_t) 3, GPIO_MODE_INPUT);   // release INT — GT911 drives it now
    vTaskDelay(pdMS_TO_TICKS(60));       // wait out GT911 firmware boot
    // 2) battery charger (BQ25896 @0x6B) + fuel gauge (BQ27220 @0x55) on the shared bus
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.scl_speed_hz = 400000;
    dc.device_address = 0x6B;
    i2c_master_bus_add_device(this->bus_, &dc, &this->bq_charger_);
    dc.device_address = 0x55;
    i2c_master_bus_add_device(this->bus_, &dc, &this->bq_gauge_);
    this->apply_charge_cap_();   // enforce the cap (matters most when the panel lives on a charger)
    this->refresh_battery_();
    // (GT911 touch added AFTER epd_init below — its latched address depends on epdiy's GPIO9/D8 setup)
    // 3) hand the bus to epdiy (it adds PCA9555/TPS on it, owns_bus=false)
    static EpdI2cConfig i2ccfg;
    i2ccfg.bus_handle = this->bus_;
    static EpdInitConfig initcfg;
    initcfg.i2c = &i2ccfg;
    ESP_LOGI("t5_epaper", "epd_init_with_config (board v7, shared I2C)...");
    epd_init_with_config(&epd_board_v7, &ED047TC1, EPD_LUT_64K, &initcfg);
    epd_set_vcom(this->vcom_);
    this->hl_ = epd_hl_init(EPD_BUILTIN_WAVEFORM);
    epd_set_rotation(this->rotation_);
    // epd_width()/epd_height() are the panel's native (landscape) dims; swap them for portrait.
    bool portrait = (this->rotation_ == EPD_ROT_PORTRAIT || this->rotation_ == EPD_ROT_INVERTED_PORTRAIT);
    this->width_ = portrait ? epd_height() : epd_width();
    this->height_ = portrait ? epd_width() : epd_height();
    this->fb_ = epd_hl_get_framebuffer(&this->hl_);
    this->fb_size_ = (size_t) epd_width() * epd_height() / 2;
    this->clean_panel_();        // deep clean at boot so we start ghost-free

    // our own read handle on the PCA9555 (0x20) to confirm EPD power-good before each push
    i2c_device_config_t pc = {};
    pc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    pc.scl_speed_hz = 400000;
    pc.device_address = 0x20;
    i2c_master_bus_add_device(this->bus_, &pc, &this->pca_);

    // GT911 touch: RST shares epdiy's D8 line + INT floats, so the latched address (0x5D or 0x14)
    // varies across reboots (e.g. the RST button). Probe both and use whichever answers.
    for (uint8_t addr : {(uint8_t) 0x5D, (uint8_t) 0x14}) {
      i2c_device_config_t td = {};
      td.dev_addr_length = I2C_ADDR_BIT_LEN_7;
      td.device_address = addr;
      td.scl_speed_hz = 400000;
      i2c_master_dev_handle_t dev = nullptr;
      if (i2c_master_bus_add_device(this->bus_, &td, &dev) != ESP_OK)
        continue;
      uint8_t st, reg[2] = {0x81, 0x4E};
      if (i2c_master_transmit_receive(dev, reg, 2, &st, 1, 50) == ESP_OK) {
        this->gt911_ = dev;
        ESP_LOGI("t5_epaper", "GT911 found at 0x%02X", addr);
        break;
      }
      i2c_master_bus_rm_device(dev);
    }
    if (this->gt911_ == nullptr)
      ESP_LOGW("t5_epaper", "GT911 not found at 0x5D or 0x14");
    // Self-heal: an independent task reboots us if the main loop ever stalls (a blocked EPD power
    // op, a wedged I2C bus, etc.) so the panel recovers on its own instead of hanging. Paused
    // during OTA (which legitimately blocks the loop) — wire ota: on_begin -> pause_watchdog().
    this->last_loop_ms_ = millis();
    xTaskCreate(&T5Display::watchdog_task_, "t5_wdt", 2560, this, 1, nullptr);
  }

  void update() override {
    // Don't power the e-paper on a sagging battery: if the supply dips, the TPS65185 may never
    // reach power-good and epdiy's power-on busy-waits on it forever (epd_board_v7.c:231). Skip
    // the refresh until the supply recovers (e.g. back on the charger). Always refresh while on
    // external power (supply is solid even if % is low). The stall-watchdog covers anything else.
    if (!this->on_charger_ && this->batt_mv_ > 0 && this->batt_mv_ < 3500) {
      ESP_LOGW("t5_epaper", "battery %d mV + off charger - skipping EPD refresh (avoid power-up stall)",
               this->batt_mv_);
      return;
    }
    // Every 20th refresh, force a full-frame repaint to de-ghost + resync. We do this ONLY via
    // epd_hl_set_all_white() (resets the diff back-buffer to white -> next push redraws everything;
    // GC16's waveform de-ghosts on charger). We DELIBERATELY do NOT call the low-level epd_clear():
    // epd_clear bypasses the diff engine, whitening the physical panel while epdiy still believes the
    // old content is shown -> if the resync didn't take, the diff no-ops forever and the panel is STUCK
    // WHITE (root cause of the recurring white-out). Driving the panel ONLY through epd_hl_update_screen
    // keeps the diff and the physical panel in sync: a failed repaint degrades to STALE CONTENT, never blank.
    this->refresh_count_++;
    if (this->refresh_count_ % 20 == 0)
      epd_hl_set_all_white(&this->hl_);
    std::memset(this->fb_, 0xFF, this->fb_size_);
    this->do_update_();
    // Black-box recorder: if the render pipeline ever produces a fully-white framebuffer, pushing it
    // would lock the screen white. Count it (exposed to HA) and skip the push so blank content can
    // never overwrite good content on the panel.
    bool blank = true;
    for (size_t i = 0; i < this->fb_size_; i++)
      if (this->fb_[i] != 0xFF) { blank = false; break; }
    if (blank) {
      this->blank_frame_count_++;
      ESP_LOGW("t5_epaper", "do_update produced a BLANK frame (#%d, blank total=%u) - skipping push, keeping content",
               this->refresh_count_, (unsigned) this->blank_frame_count_);
      return;
    }
    epd_poweron();
    // Only push if the EPD rail actually reached power-good. Pushing while the rail is down would
    // advance epdiy's diff back-buffer WITHOUT physically drawing -> the screen desyncs and then
    // only pixels that later CHANGE get painted (the "blank except one toggling element" bug).
    // Skipping keeps the diff in sync; the full frame repaints on the next good cycle (self-healing).
    if (this->epd_powergood_()) {
      // On battery a full GC16 refresh (16-level greyscale = many waveform frames = high sustained
      // current) can sag the marginal rail mid-waveform: the draw doesn't complete, but epdiy still
      // marks the back-buffer "drawn" -> diff desync -> stuck white. Use the light, fast, low-current
      // MODE_DU off-charger; keep crisp GC16 while on external power (solid rail). DU is 1-bit B/W, so
      // greyscale UIs lose shading off-charger but stay legible and never strand the panel.
      enum EpdDrawMode mode = this->on_charger_ ? MODE_GC16 : MODE_DU;
      enum EpdDrawError err = epd_hl_update_screen(&this->hl_, mode, 25);
      if (err != EPD_DRAW_SUCCESS)
        ESP_LOGW("t5_epaper", "update err=%d (mode=%s)", (int) err, mode == MODE_GC16 ? "GC16" : "DU");
    } else {
      this->pg_fail_count_++;
      ESP_LOGW("t5_epaper", "EPD power-good NOT asserted (#%u) - skipped push to keep diff in sync",
               (unsigned) this->pg_fail_count_);
    }
    epd_poweroff();
  }

  void loop() override {
    uint32_t now = millis();
    this->last_loop_ms_ = now;   // heartbeat for the stall watchdog
    if (now - this->last_batt_ > 15000) {   // refresh battery readings for HA + the low-batt guard
      this->last_batt_ = now;               // (cap is applied once at setup / on change, NOT here:
      this->refresh_battery_();             //  rewriting the charger every 15s makes charging cycle)
    }
    if (this->gt911_ == nullptr || now - this->last_poll_ < 40)
      return;
    this->last_poll_ = now;
    uint8_t status;
    if (!this->gt911_read_(0x814E, &status, 1))
      return;
    bool touching = false;
    if (status & 0x80) {  // touch data ready
      uint8_t n = status & 0x0F;
      if (n >= 1 && n <= 5) {
        uint8_t pt[8];
        if (this->gt911_read_(0x8150, pt, 8)) {
          int x = pt[0] | (pt[1] << 8);          // already in display coords (540x960, top-left)
          int y = pt[2] | (pt[3] << 8);
          touching = true;
          if (!this->was_touching_) {             // rising edge = a new tap
            // Debounce ACTIONS, not just finger-bounce: e-paper takes ~1s to visibly redraw, so an
            // impatient user taps again — each tap stacks an EPD power-up surge AND a ~1s HA service
            // call. On battery that pile-up stalls the loop (looks frozen) or browns out (reboot).
            // Accept one tap, then ignore repeats for the settle window. One action, one refresh.
            if (now - this->last_action_ms_ >= 1000) {
              this->last_action_ms_ = now;
              ESP_LOGI("t5_epaper.touch", "TAP x=%d y=%d", x, y);
              this->touch_trigger_.trigger(x, y);
            } else {
              ESP_LOGD("t5_epaper.touch", "tap ignored (%ums since last action - settling)",
                       (unsigned) (now - this->last_action_ms_));
            }
          }
        }
      }
      this->gt911_write1_(0x814E, 0x00);  // clear the buffer-ready flag
    }
    this->was_touching_ = touching;
  }

  void draw_pixel_at(int x, int y, Color color) override {
    uint16_t lum = (uint16_t) ((color.r * 54 + color.g * 183 + color.b * 19) >> 8);
    epd_draw_pixel(x, y, (uint8_t) (255 - lum), this->fb_);
  }

  Trigger<int, int> *get_touch_trigger() { return &this->touch_trigger_; }

  // De-ghost + full repaint. Wire to an HA button/service, or to a binary_sensor for a physical button.
  // E-paper ghosting only clears by flashing every pixel through the waveform, which the diff engine
  // won't do on its own. This does it SAFELY: (1) render the current view FIRST and ABORT if it's blank,
  // so we never flash the panel white with nothing to put back; (2) drive the de-ghost through the diff
  // engine (epd_hl_set_all_white -> push), NEVER the low-level epd_clear() which desyncs the diff and can
  // strand the panel white. Two passes: pass 1 whitens every non-white pixel, pass 2 redraws the content.
  void deep_clean() {
    this->refresh_battery_();   // refresh charger state NOW so a clean right after docking picks the right path
    this->fb_ = epd_hl_get_framebuffer(&this->hl_);
    std::memset(this->fb_, 0xFF, this->fb_size_);
    this->do_update_();
    bool blank = true;
    for (size_t i = 0; i < this->fb_size_; i++)
      if (this->fb_[i] != 0xFF) { blank = false; break; }
    if (blank) {
      this->blank_frame_count_++;
      ESP_LOGW("t5_epaper", "deep_clean aborted - render is blank, not flashing white");
      return;
    }
    enum EpdDrawMode mode = this->on_charger_ ? MODE_GC16 : MODE_DU;
    // PASS 1 — drive the whole panel white (de-ghost every non-white pixel) and sync back_fb to white
    epd_hl_set_all_white(&this->hl_);
    epd_poweron();
    if (this->epd_powergood_()) epd_hl_update_screen(&this->hl_, mode, 25);
    epd_poweroff();
    // PASS 2 — pass 1 wiped the FRONT fb to white, so redraw the content; diff(content vs white) = full repaint
    this->fb_ = epd_hl_get_framebuffer(&this->hl_);
    std::memset(this->fb_, 0xFF, this->fb_size_);
    this->do_update_();
    epd_poweron();
    if (this->epd_powergood_()) epd_hl_update_screen(&this->hl_, mode, 25);
    epd_poweroff();
    ESP_LOGI("t5_epaper", "deep clean done (%s, 2-pass)", this->on_charger_ ? "GC16" : "DU");
  }

  // watchdog control — OTA legitimately blocks the loop, so pause around it
  void pause_watchdog() { this->wdt_paused_ = true; }
  void resume_watchdog() { this->wdt_paused_ = false; }

  // battery / charge-cap API (for an HA number + a status screen)
  void set_charge_cap_pct(int pct) {
    this->charge_cap_pct_ = pct < 60 ? 60 : (pct > 100 ? 100 : pct);
    this->apply_charge_cap_();
  }
  int battery_soc() { return this->batt_soc_; }       // % (-1 if unknown)
  float battery_volts() { return this->batt_mv_ / 1000.0f; }
  bool battery_charging() { return this->batt_charging_; }
  int charge_cap_pct() { return this->charge_cap_pct_; }
  float charge_cap_volts() {                          // the actual charge-voltage the cap targets
    int mv = 4208 - (100 - this->charge_cap_pct_) * 10;
    if (mv < 3840) mv = 3840;
    if (mv > 4208) mv = 4208;
    return mv / 1000.0f;
  }
  bool on_charger() { return this->on_charger_; }     // external power present (solid rail; drives the header bolt)
  int blank_frames() { return (int) this->blank_frame_count_; }   // render-pipeline-produced-blank counter (for HA)

  // config setters (called from to_code)
  void set_vcom(int mv) { this->vcom_ = mv; }
  void set_panel_rotation(EpdRotation r) { this->rotation_ = r; }
  void set_default_charge_cap(int pct) { this->charge_cap_pct_ = pct; }

  int get_width_internal() override { return this->width_; }
  int get_height_internal() override { return this->height_; }
  display::DisplayType get_display_type() override { return display::DISPLAY_TYPE_GRAYSCALE; }
  void dump_config() override {
    ESP_LOGCONFIG("t5_epaper", "LilyGO T5 E-Paper S3 Pro (540x960 grayscale, epdiy v7, GT911 touch)");
    ESP_LOGCONFIG("t5_epaper", "  GT911 touch: %s", this->gt911_ ? "found" : "NOT found");
    ESP_LOGCONFIG("t5_epaper", "  charge cap: %d%%", this->charge_cap_pct_);
  }

 protected:
  // BOOT-ONLY deep flush. epd_clear() flashes the whole panel through the raw waveform (clears ghosting
  // left by a previous firmware), then we sync the diff back-buffer to white to match. Safe ONLY at boot,
  // before any content is on screen: epd_clear() bypasses the diff engine, so calling it at runtime would
  // whiten the panel while epdiy still believes the old content is shown -> the diff no-ops -> stuck white.
  // For a runtime de-ghost use deep_clean() (diff-safe). Do NOT call this from update() or a button.
  void clean_panel_() {
    epd_poweron();
    epd_clear();
    epd_clear();
    epd_poweroff();
    epd_hl_set_all_white(&this->hl_);
  }

  bool gt911_read_(uint16_t reg, uint8_t *buf, size_t len) {
    uint8_t r[2] = {(uint8_t) (reg >> 8), (uint8_t) (reg & 0xFF)};
    return i2c_master_transmit_receive(this->gt911_, r, 2, buf, len, 50) == ESP_OK;
  }
  void gt911_write1_(uint16_t reg, uint8_t val) {
    uint8_t w[3] = {(uint8_t) (reg >> 8), (uint8_t) (reg & 0xFF), val};
    i2c_master_transmit(this->gt911_, w, 3, 50);
  }

  // EPD power-good = PCA9555 port-1 bit6 (PC16). Fail-open (return true) if we can't read it,
  // so an I2C hiccup never permanently blocks rendering.
  bool epd_powergood_() {
    if (this->pca_ == nullptr)
      return true;
    uint8_t reg = 0x01, val = 0;   // PCA9555 input port 1
    if (i2c_master_transmit_receive(this->pca_, &reg, 1, &val, 1, 50) != ESP_OK)
      return true;
    return (val & 0x40) != 0;
  }

  // Reboots the device if loop() hasn't run in 60s (longer than any legit blocking op incl. OTA,
  // which is also explicitly paused). Runs on its own task so a blocked main loop can't disable it.
  static void watchdog_task_(void *arg) {
    auto *self = static_cast<T5Display *>(arg);
    for (;;) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      if (self->wdt_paused_)
        continue;
      uint32_t last = self->last_loop_ms_;
      if (last != 0 && (millis() - last) > 60000) {
        ESP_LOGE("t5_epaper", "main loop stalled %u ms - rebooting to self-heal", (unsigned) (millis() - last));
        esp_restart();
      }
    }
  }

  // ---- BQ25896 charger / BQ27220 gauge (8-bit regs; gauge words are little-endian) ----
  static bool r8_(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *v) {
    return d && i2c_master_transmit_receive(d, &reg, 1, v, 1, 50) == ESP_OK;
  }
  static bool w8_(i2c_master_dev_handle_t d, uint8_t reg, uint8_t v) {
    uint8_t w[2] = {reg, v};
    return d && i2c_master_transmit(d, w, 2, 50) == ESP_OK;
  }
  static bool r16_(i2c_master_dev_handle_t d, uint8_t reg, uint16_t *v) {
    uint8_t b[2];
    if (!d || i2c_master_transmit_receive(d, &reg, 1, b, 2, 50) != ESP_OK)
      return false;
    *v = b[0] | (b[1] << 8);
    return true;
  }
  void apply_charge_cap_() {
    if (this->bq_charger_ == nullptr)
      return;
    int mv = 4208 - (100 - this->charge_cap_pct_) * 10;   // ~10mV per %, 100%->4208, 80%->4008
    if (mv < 3840) mv = 3840;
    if (mv > 4208) mv = 4208;
    uint8_t bits = (uint8_t) ((mv - 3840) / 16);          // BQ25896 REG06[7:2], 16mV/step
    uint8_t r6;
    if (r8_(this->bq_charger_, 0x06, &r6))
      w8_(this->bq_charger_, 0x06, (uint8_t) ((r6 & 0x03) | (bits << 2)));
    // Disable the I2C watchdog (REG07[5:4]=00) so VREG holds with NO periodic writes. Verify + retry:
    // a single flaky read here leaves the watchdog at its 40s default, which expires and RESETS VREG to
    // 4.208V -> charging resumes -> the bolt flickers on at a capped-and-full battery (charge cycling).
    bool wdt_off = false;
    for (int i = 0; i < 4 && !wdt_off; i++) {
      uint8_t r7;
      if (r8_(this->bq_charger_, 0x07, &r7)) {
        w8_(this->bq_charger_, 0x07, (uint8_t) (r7 & ~0x30));
        uint8_t chk;
        if (r8_(this->bq_charger_, 0x07, &chk) && (chk & 0x30) == 0)
          wdt_off = true;
      }
    }
    if (!wdt_off)
      ESP_LOGW("t5_epaper", "BQ25896 watchdog disable NOT confirmed - VREG may reset (charge cycling)");
  }
  void refresh_battery_() {
    uint16_t v;
    if (r16_(this->bq_gauge_, 0x2C, &v)) this->batt_soc_ = (int) v;   // StateOfCharge %
    if (r16_(this->bq_gauge_, 0x08, &v)) this->batt_mv_ = (int) v;    // Voltage mV
    uint8_t s;
    if (r8_(this->bq_charger_, 0x0B, &s)) {                            // REG0B
      uint8_t cs = (s >> 3) & 0x03;                                    // CHRG_STAT[4:3]
      this->batt_charging_ = (cs == 1 || cs == 2);                     // actively charging (drives the bolt)
      this->on_charger_ = ((s >> 2) & 0x01) != 0;                      // PG_STAT[2]: external power present =
    }                                                                  // solid rail -> safe for crisp GC16
  }

  // config (set from YAML; defaults match the T5 S3 Pro in portrait)
  int vcom_{1560};
  EpdRotation rotation_{EPD_ROT_INVERTED_PORTRAIT};
  int width_{540};
  int height_{960};

  EpdiyHighlevelState hl_;
  uint8_t *fb_{nullptr};
  size_t fb_size_{0};
  i2c_master_bus_handle_t bus_{nullptr};
  i2c_master_dev_handle_t gt911_{nullptr};
  uint32_t last_poll_{0};
  bool was_touching_{false};
  int refresh_count_{0};
  Trigger<int, int> touch_trigger_;
  volatile uint32_t last_loop_ms_{0};   // stall-watchdog heartbeat
  volatile bool wdt_paused_{false};
  // charger/gauge
  i2c_master_dev_handle_t bq_charger_{nullptr};
  i2c_master_dev_handle_t bq_gauge_{nullptr};
  i2c_master_dev_handle_t pca_{nullptr};   // PCA9555 0x20, read PWRGOOD before each EPD push
  uint32_t pg_fail_count_{0};
  uint32_t blank_frame_count_{0};   // times do_update() yielded an all-white frame (would lock the panel white)
  int charge_cap_pct_{100};     // default: charge to full. Lower it (e.g. 80) for always-on/charger installs.
  uint32_t last_action_ms_{0};  // tap-action debounce (stops rapid taps stacking refreshes + HA calls)
  int batt_soc_{-1};
  int batt_mv_{0};
  bool batt_charging_{false};   // actively charging (CHRG_STAT 1/2) — drives the charging-bolt glyph
  bool on_charger_{true};       // external power present (REG0B PG_STAT) — solid rail => crisp GC16, else DU
  uint32_t last_batt_{0};
};

}  // namespace t5_epaper
}  // namespace esphome
