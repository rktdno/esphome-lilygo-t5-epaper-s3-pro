# Changelog

All notable changes to this component are documented here. This project adheres to
[Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.1.0] — first release

The missing ESPHome component for the **LilyGO T5 E-Paper S3 Pro** (ED047TC1, 540×960,
ESP32-S3) — display, touch, and battery, on one shared I²C bus.

### Added
- `t5_epaper` ESPHome `display` platform, driving the panel via upstream
  [epdiy](https://github.com/vroland/epdiy) (`epd_board_v7`), pinned to a known-good commit.
- **GT911 capacitive touch** exposed as an `on_touch:` automation trigger (`x`/`y` in
  display coordinates), sharing epdiy's I²C bus via `epd_init_with_config`.
- **Battery support** — SOC / voltage / charging state (BQ27220 gauge + BQ25896 charger),
  plus a software charge-voltage cap (`set_charge_cap_pct()`).
- **`deep_clean()`** — a diff-safe, two-pass full-panel refresh to clear e-paper ghosting, for a
  button or an HA service. Renders the current view first and aborts if it's blank (so it never
  flashes the panel white with nothing to put back), then de-ghosts through the diff engine.
- Configurable **`vcom`**, **`panel_rotation`** (width/height auto-derived), and
  **`default_charge_cap`**.
- **`dashboard_import`** so a flashed device offers one-click **Adopt** in the ESPHome dashboard.
- Workaround for the side **RST button leaving the GT911 stuck**: an explicit GT911 hardware
  reset pulse before epdiy claims the shared GPIO9/D8 line.
- **Stall watchdog** — an independent task reboots the device if the main loop stops ticking for
  60 s (a blocked EPD power op, a wedged I²C bus, …), so it self-heals instead of hanging until a
  manual reset. Auto-paused during OTA via `pause_watchdog()` / `resume_watchdog()`.
- **Low-battery refresh guard** — skips the e-paper refresh while on a sagging battery (off external
  power, < 3.5 V) so it never triggers the power-up power-good failure below.
- **Root-cause fix for the power-on hang** — epdiy's `epd_board_v7` busy-waited on the PCA9555
  `PWRGOOD` bit with no timeout and could hang the device forever on a supply dip. Pinned epdiy is
  a fork carrying the bounded-wait fix ([vroland/epdiy#481](https://github.com/vroland/epdiy/pull/481));
  will revert to upstream once merged.
- **Power-good gate on the screen push** — only call `epd_hl_update_screen` after confirming the EPD
  rail is power-good (read from the PCA9555). A push while the rail is down would advance epdiy's diff
  back-buffer without physically drawing, desyncing it so only later-changing pixels paint (screen goes
  blank but for one toggling element). Skipping keeps the diff in sync; the frame repaints next good cycle.
- **Root-cause fix for the recurring "stuck white screen."** The panel is driven **only** through
  epdiy's diff engine (`epd_hl_update_screen`); the low-level `epd_clear()` is never called at runtime.
  `epd_clear()` whitens the physical panel while epdiy still believes the old content is shown — if the
  diff buffer then fails to resync, every later push no-ops and the screen is stuck white forever. The
  periodic de-ghost now uses `epd_hl_set_all_white()` (resets the diff back-buffer, so the next push
  legitimately repaints everything), and a failed refresh degrades to *stale content*, never blank.
  `epd_clear()` is now reserved for a one-time boot flush, where there is no content to strand.
- **`MODE_DU` off-charger, `MODE_GC16` on-charger.** A full 16-level GC16 refresh draws high sustained
  current; on a marginal battery rail it can sag mid-waveform, leaving the draw incomplete while epdiy
  marks the back-buffer "drawn" → diff desync → stuck white. Off external power the panel uses the light,
  fast 1-bit `MODE_DU` (loses greyscale shading but stays legible and never strands); crisp GC16 returns
  on the charger. On-charger state is read live from the BQ25896 (REG0B `PG_STAT`).
- **Blank-frame guard** — if the render pipeline ever produces an all-white framebuffer, the push is
  skipped (good content is kept on the panel) and a counter is bumped (`blank_frames()`, exposable to HA)
  so a future white-out is diagnosable as render-side vs diff/push-side.
- **Tap-action debounce** — e-paper takes ~1 s to visibly redraw, so impatient repeat taps would stack
  EPD power-up surges and ~1 s HA service calls, stalling the loop or browning out on battery. One tap is
  accepted, repeats inside a 1 s settle window are ignored: one action, one refresh.
- **Charge cap applied once** (at setup / on slider change), never on a timer — continuously rewriting
  the BQ25896 made charging cycle on/off. The charger's I²C watchdog is disabled (with read-back verify +
  retry) so a one-time write holds without the watchdog resetting VREG back to full.
- Standalone `example.yaml` and a full setup recipe in the README.
