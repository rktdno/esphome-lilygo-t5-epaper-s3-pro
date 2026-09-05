# Shared T5 E-Paper S3 Pro component

This is the single maintained hardware driver. Keep it generic: no household entities,
network details, private dashboard code or credentials belong here. Consumer applications
pin a public commit and own their UI, command scheduling, Bluetooth and home-key action.
GitHub is the contribution remote; Forgejo is a private read-only pull mirror.

Preserve VCOM, hardware rotation, default charge cap, on_touch, on_home_button,
clean_screen and deep_clean APIs. The latter retains three GC16 black/white cycles on the
charger; ordinary cleaning uses the checked DU white/content path. Preserve the front
buffer during all passes, and only synchronise back_fb after successful white-out.
A failed draw or power-off invalidates diff state and uses bounded retries. Never add a
runtime epd_clear or treat epd_hl_set_all_white as a back-buffer reset.

Use the 1 KiB LUT and retain checked power transitions plus the line-queue memory fix in
the epdiy pin. All I2C access remains sequential on the ESPHome loop; do not move display
work to a background task without explicit bus ownership. GT911 reset must precede LCD
setup because GPIO9 is shared. Touch must match hardware rotation.

Before publishing hardware changes:
- Run `python3 tests/test_driver.py --epdiy-source /path/to/pinned/epdiy` (C++ sanitizers).
- Run `python3 tools/compile_example.py` (local component, dummy WiFi values).
- Compile the private consuming firmware without copying it into this repository.
- Review the public diff for private data. Publish, then pin the consumer to that exact commit.
- Verify normal display/cleaning and runtime memory on the consuming device. Host tests
  cannot prove optical waveform quality. Do not operate household appliances for coverage.
