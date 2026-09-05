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
    // Apply the charge cap ONCE here (and again only when the HA slider changes). The cap logic is
    // fine; the bug was calling it every 15s — that continuous rewrite fought the charge cycle. We
    // disable the charger's I2C watchdog so the voltage cap holds with NO periodic writes.
    this->apply_charge_cap_();
    this->refresh_battery_();
    // (GT911 touch added AFTER epd_init below — its latched address depends on epdiy's GPIO9/D8 setup)
    // 3) hand the bus to epdiy (it adds PCA9555/TPS on it, owns_bus=false)
    static EpdI2cConfig i2ccfg;
    i2ccfg.bus_handle = this->bus_;
    static EpdInitConfig initcfg;
    initcfg.i2c = &i2ccfg;
    ESP_LOGI("t5_epaper", "epd_init_with_config (board v7, shared I2C)...");
    // S3 LCD difference rendering uses the same vector path with a 1 KiB LUT.
    // Avoid reserving an unused extra 63 KiB of scarce internal RAM.
    epd_init_with_config(&epd_board_v7, &ED047TC1, EPD_LUT_1K, &initcfg);
    epd_set_vcom(this->vcom_);
    this->hl_ = epd_hl_init(EPD_BUILTIN_WAVEFORM);
    epd_set_rotation(this->rotation_);
    this->width_ = epd_rotated_display_width();
    this->height_ = epd_rotated_display_height();
    this->fb_ = epd_hl_get_framebuffer(&this->hl_);
    this->fb_size_ = (size_t) epd_width() * epd_height() / 2;
    // Boot-only deep clean. A power failure must not block touch/HA startup or
    // issue a waveform; the normal checked update will retry with a white pass.
    if (epd_poweron_checked() == ESP_OK) {
      epd_clear();
      epd_clear();
    } else {
      ESP_LOGW("t5_epaper", "Boot clear deferred: display power-on failed");
    }
    if (epd_poweroff_checked() != ESP_OK)
      ESP_LOGW("t5_epaper", "Boot display power-off failed");
    epd_hl_set_all_white(&this->hl_);

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
    // Self-heal: an independent task that reboots us if the main loop ever stalls (a blocked EPD
    // power op, a wedged I2C bus, etc.) — so the panel recovers on its own instead of hanging
    // until someone presses RST. Paused during OTA (which legitimately blocks the loop).
    this->last_loop_ms_ = millis();
    xTaskCreate(&T5Display::watchdog_task_, "t5_wdt", 2560, this, 1, nullptr);
  }

  void update() override {
    if (this->is_failed() || this->fb_ == nullptr) return;
    if (this->retry_pending_ && static_cast<int32_t>(millis() - this->retry_due_) < 0) return;
    if (!this->on_charger_ && (this->batt_mv_ <= 0 || this->batt_mv_ < 3500)) {
      ESP_LOGW("t5_epaper", "battery too low or unknown - deferring display update/clean");
      return;
    }
    ++this->refresh_count_;
    std::memset(this->fb_, 0xFF, this->fb_size_);
    this->do_update_();
    bool blank = true;
    for (size_t i = 0; i < this->fb_size_; ++i)
      if (this->fb_[i] != 0xFF) { blank = false; break; }
    if (blank) {
      ++this->blank_frame_count_;
      ESP_LOGW("t5_epaper", "BLANK render - preserving screen, update deferred");
      return;
    }
    const bool clean = this->clean_requested_ || !this->last_push_succeeded_ ||
                       this->pushes_since_clean_ >= 19;
    if (!clean && std::memcmp(this->fb_, this->hl_.back_fb, this->fb_size_) == 0) {
      ++this->unchanged_frames_;
      return;
    }
    const uint32_t started = millis();
    const auto power_error = epd_poweron_checked();
    if (power_error != ESP_OK || !this->epd_powergood_()) {
      ESP_LOGW("t5_epaper", "Display power-on/readiness failed (%d)", (int) power_error);
      ++this->pg_fail_count_;
      if (epd_poweroff_checked() != ESP_OK)
        ESP_LOGW("t5_epaper", "Display power cleanup failed");
      this->draw_failed_();
      return;
    }
    bool ok = true;
    if (clean) {
      // Establish white without relying on the old back buffer. Keep the
      // validated content intact in front_fb throughout every cleaning pass.
      ok = this->solid_pass_(0xF0, MODE_DU);
      if (ok && this->deep_clean_requested_ && this->on_charger_) {
        // Preserve the public deep_clean API: three black/white GC16 cycles
        // on external power, with every pass checked and no extra framebuffer.
        for (int i = 0; i < 3 && ok; ++i)
          ok = this->solid_pass_(0x0F, MODE_GC16) && this->solid_pass_(0xF0, MODE_GC16);
      }
      if (ok) std::memset(this->hl_.back_fb, 0xFF, this->fb_size_);
    }
    if (ok) {
      auto mode = this->on_charger_ ? MODE_GC16 : MODE_DU;
      auto err = epd_hl_update_screen(&this->hl_, mode, 25);
      ok = err == EPD_DRAW_SUCCESS && this->epd_powergood_();
    }
    // Always switch off, even after a failed draw. Do not mark the transaction
    // successful when shutdown failed; invalidate diff state and retry later.
    if (epd_poweroff_checked() != ESP_OK) {
      ESP_LOGW("t5_epaper", "Display power-off failed");
      ok = false;
    }
    if (!ok) {
      this->draw_failed_();
      return;
    }
    this->last_push_succeeded_ = true;
    this->clean_requested_ = false;
    this->deep_clean_requested_ = false;
    this->retry_pending_ = false;
    this->retry_attempts_ = 0;
    if (clean) { ++this->clean_count_; this->pushes_since_clean_ = 0; }
    else ++this->pushes_since_clean_;
    ESP_LOGI("t5_epaper", "epd push ok (#%d, %s, %ums, pg_fail=%u)", this->refresh_count_,
             clean ? "clean+content" : (this->on_charger_ ? "GC16" : "DU"),
             (unsigned)(millis() - started), (unsigned)this->pg_fail_count_);
  }

  void loop() override {
    uint32_t now = millis();
    this->last_loop_ms_ = now;   // heartbeat for the stall watchdog
    if (now - this->last_batt_ > 15000) {   // refresh battery readings for HA + the low-batt guard
      this->last_batt_ = now;               // (charge cap is applied once at boot, not here — the
      this->refresh_battery_();             //  every-15s rewrite was what made charging cycle)
    }
    if (this->retry_pending_ && !this->wdt_paused_ &&
        static_cast<int32_t>(now - this->retry_due_) >= 0) {
      this->retry_pending_ = false;
      this->update();
      now = millis();
      this->last_loop_ms_ = now;
    }
    if (this->gt911_ == nullptr || now - this->last_poll_ < 40)
      return;
    this->last_poll_ = now;
    uint8_t status;
    if (!this->gt911_read_(0x814E, &status, 1))
      return;
    bool touching = false;
    if (status & 0x80) {  // data ready (touch and/or capacitive key)
      uint8_t n = status & 0x0F;
      // Home-key behaviour belongs to the consuming configuration. Preserve
      // the public trigger and allow the kitchen to choose its lighter clean.
      if ((status & 0x10) && (now - this->last_home_btn_ms_) >= this->home_button_debounce_ms_) {
        this->last_home_btn_ms_ = now;
        this->home_button_trigger_.trigger();
        this->last_loop_ms_ = millis();
      }
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
              if (this->transform_touch_(x, y)) this->touch_trigger_.trigger(x, y);
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
  Trigger<> *get_home_button_trigger() { return &this->home_button_trigger_; }

  // Configurable board settings retained from the public component.
  void set_home_button_debounce(uint32_t ms) { this->home_button_debounce_ms_ = ms; }
  void set_vcom(int mv) { this->vcom_ = mv; }
  void set_panel_rotation(EpdRotation rotation) { this->rotation_ = rotation; }
  void set_default_charge_cap(int pct) { this->charge_cap_pct_ = pct; }

  // Stronger manual clean; low/unknown battery, blank renders and failed power
  // still go through the same guards and bounded recovery as ordinary updates.
  void deep_clean() {
    this->deep_clean_requested_ = true;
    this->clean_screen();
  }

  // Explicit clean is retained until a safe, successful white+content transaction.
  void clean_screen() {
    this->clean_requested_ = true;
    this->refresh_battery_();
    this->update();
  }

  // watchdog control — OTA legitimately blocks the loop, so pause around it
  void pause_watchdog() { this->wdt_paused_ = true; }
  void resume_watchdog() { this->wdt_paused_ = false; }

  // battery / charge-cap API (for the HA number + status screen)
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
  bool on_charger() { return this->on_charger_; }     // external power present (drives the header bolt)
  int unchanged_frames() const { return (int)this->unchanged_frames_; }
  int draw_failures() const { return (int)this->draw_failures_; }
  int clean_count() const { return (int)this->clean_count_; }
  int blank_frames() { return (int) this->blank_frame_count_; }   // render-pipeline-produced-blank counter

  int get_width_internal() override { return this->width_; }
  int get_height_internal() override { return this->height_; }
  display::DisplayType get_display_type() override { return display::DISPLAY_TYPE_GRAYSCALE; }
  void dump_config() override {
    ESP_LOGCONFIG("t5_epaper", "LilyGO T5 E-Paper S3 Pro (%dx%d, epdiy v7, GT911 touch)", this->width_, this->height_);
    ESP_LOGCONFIG("t5_epaper", "  GT911: %s, charge cap: %d%%", this->gt911_ ? "found" : "not found", this->charge_cap_pct_);
  }

 protected:
  bool solid_pass_(uint8_t transition, EpdDrawMode mode) {
    std::memset(this->hl_.difference_fb, transition, this->fb_size_ * 2);
    auto err = epd_draw_base(epd_full_screen(), this->hl_.difference_fb, epd_full_screen(),
        (EpdDrawMode)(mode | MODE_PACKING_1PPB_DIFFERENCE), 25, nullptr, nullptr, this->hl_.waveform);
    this->last_loop_ms_ = millis();
    return err == EPD_DRAW_SUCCESS && this->epd_powergood_();
  }
  bool transform_touch_(int &x, int &y) const {
    // GT911 coordinates match inverted portrait. Convert to the configured
    // epdiy rotation, using the inverse of epdiy's framebuffer pixel transform.
    const int native_w = epd_width(), native_h = epd_height();
    if (x < 0 || x >= native_h || y < 0 || y >= native_w) return false;
    const int tx = x, ty = y;
    switch (this->rotation_) {
      case EPD_ROT_LANDSCAPE: x = ty; y = native_h - 1 - tx; break;
      case EPD_ROT_PORTRAIT: x = native_h - 1 - tx; y = native_w - 1 - ty; break;
      case EPD_ROT_INVERTED_LANDSCAPE: x = native_w - 1 - ty; y = tx; break;
      case EPD_ROT_INVERTED_PORTRAIT: break;
    }
    return true;
  }
  void draw_failed_() {
    // epdiy may have advanced back_fb even on an error. Never reuse it as truth.
    this->last_push_succeeded_ = false;
    ++this->draw_failures_;
    this->retry_pending_ = false;
    if (this->retry_attempts_ < 3) {
      ++this->retry_attempts_;
      this->retry_pending_ = true;
      this->retry_due_ = millis() + 5000;
    }
    ESP_LOGW("t5_epaper", "draw uncertain; recovery deferred (failures=%u, retries=%u)",
             (unsigned)this->draw_failures_, (unsigned)this->retry_attempts_);
  }
  bool gt911_read_(uint16_t reg, uint8_t *buf, size_t len) {
    uint8_t r[2] = {(uint8_t) (reg >> 8), (uint8_t) (reg & 0xFF)};
    return i2c_master_transmit_receive(this->gt911_, r, 2, buf, len, 50) == ESP_OK;
  }
  void gt911_write1_(uint16_t reg, uint8_t val) {
    uint8_t w[3] = {(uint8_t) (reg >> 8), (uint8_t) (reg & 0xFF), val};
    i2c_master_transmit(this->gt911_, w, 3, 50);
  }

  // PCA9555 port-1 bit6 (PC16). Unknown power must prevent drawing; retries recover it.
  bool epd_powergood_() {
    if (this->pca_ == nullptr)
      return false;
    uint8_t reg = 0x01, val = 0;   // PCA9555 input port 1
    if (i2c_master_transmit_receive(this->pca_, &reg, 1, &val, 1, 50) != ESP_OK)
      return false;
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

  int vcom_{1560};
  EpdRotation rotation_{EPD_ROT_INVERTED_PORTRAIT};
  int width_{540}, height_{960};

  EpdiyHighlevelState hl_;
  uint8_t *fb_{nullptr};
  size_t fb_size_{0};
  i2c_master_bus_handle_t bus_{nullptr};
  i2c_master_dev_handle_t gt911_{nullptr};
  uint32_t last_poll_{0};
  uint32_t last_action_ms_{0};   // tap-action debounce (stops rapid taps stacking refreshes + HA calls)
  bool was_touching_{false};
  int refresh_count_{0};
  Trigger<int, int> touch_trigger_;
  Trigger<> home_button_trigger_;
  uint32_t last_home_btn_ms_{0}, home_button_debounce_ms_{1000};
  volatile uint32_t last_loop_ms_{0};   // stall-watchdog heartbeat
  volatile bool wdt_paused_{false};
  // charger/gauge
  i2c_master_dev_handle_t bq_charger_{nullptr};
  i2c_master_dev_handle_t bq_gauge_{nullptr};
  i2c_master_dev_handle_t pca_{nullptr};   // PCA9555 0x20, read PWRGOOD before each EPD push
  bool last_push_succeeded_{false};
  bool clean_requested_{false}, deep_clean_requested_{false}, retry_pending_{false};
  uint32_t retry_due_{0}, retry_attempts_{0}, pushes_since_clean_{0};
  uint32_t draw_failures_{0}, clean_count_{0};
  uint32_t unchanged_frames_{0};
  uint32_t pg_fail_count_{0};
  uint32_t blank_frame_count_{0};   // times do_update() yielded an all-white frame (would lock the panel white)
  int charge_cap_pct_{100};     // Charge to full unless explicitly configured otherwise.
  int batt_soc_{-1};
  int batt_mv_{0};
  bool batt_charging_{false};   // actively charging (CHRG_STAT 1/2) — drives the charging-bolt glyph
  bool on_charger_{true};       // external power present (REG0B PG_STAT) — solid rail => crisp GC16, else DU
  uint32_t last_batt_{0};
};

}  // namespace t5_epaper
}  // namespace esphome
