from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import display, esp32
from esphome.const import CONF_ID, CONF_LAMBDA

DEPENDENCIES = ["esp32"]
CONF_ON_TOUCH = "on_touch"
CONF_VCOM = "vcom"
CONF_PANEL_ROTATION = "panel_rotation"
CONF_DEFAULT_CHARGE_CAP = "default_charge_cap"

# Pinned to a fork of vroland/epdiy at upstream commit bdb85cc (which carries the shared-I2C
# init API — EpdInitConfig / EpdI2cConfig / epd_init_with_config — not yet in any release tag)
# PLUS a one-commit fix: epd_board_v7's power-on busy-waited on PWRGOOD with no timeout and could
# hang the device forever on a supply dip. PR'd upstream (vroland/epdiy); switch REPO/REF back to
# vroland/epdiy once it merges.
EPDIY_REPO = "https://github.com/rktdno/epdiy"
EPDIY_REF = "d92d739371957baebdb2031039abbbecc22ea841"

t5_epaper_ns = cg.esphome_ns.namespace("t5_epaper")
T5Display = t5_epaper_ns.class_("T5Display", display.Display)

# epdiy hardware rotation. Use THIS, not ESPHome's built-in `rotation:` (which would rotate
# in software on top of the panel). `inverted_portrait` puts the USB-C port at the bottom.
ROTATIONS = {
    "landscape": "EPD_ROT_LANDSCAPE",
    "portrait": "EPD_ROT_PORTRAIT",
    "inverted_landscape": "EPD_ROT_INVERTED_LANDSCAPE",
    "inverted_portrait": "EPD_ROT_INVERTED_PORTRAIT",
}

CONFIG_SCHEMA = display.FULL_DISPLAY_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(T5Display),
        cv.Optional(CONF_ON_TOUCH): automation.validate_automation(single=True),
        # VCOM in millivolts (positive). The correct value is printed on your panel's flex
        # cable; 1560 is a sane default for the T5 S3 Pro's ED047TC1.
        cv.Optional(CONF_VCOM, default=1560): cv.int_range(min=1000, max=2500),
        cv.Optional(CONF_PANEL_ROTATION, default="inverted_portrait"): cv.one_of(
            *ROTATIONS, lower=True
        ),
        # Software charge-voltage cap (%). 100 = charge to full; lower it (e.g. 80) for an
        # always-on/charger install to reduce LiPo calendar wear. Also settable at runtime.
        cv.Optional(CONF_DEFAULT_CHARGE_CAP, default=100): cv.int_range(min=60, max=100),
    }
).extend(cv.polling_component_schema("60s"))


async def to_code(config):
    # UPSTREAM epdiy (IDF-current) pulled as a managed component. Its epd_board_v7 IS the
    # T5 S3 Pro board — do NOT use LilyGO's fork (its lcd_driver.c won't build on modern IDF).
    esp32.add_idf_component(name="epdiy", repo=EPDIY_REPO, ref=EPDIY_REF)
    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)
    cg.add(var.set_vcom(config[CONF_VCOM]))
    cg.add(var.set_panel_rotation(cg.RawExpression(ROTATIONS[config[CONF_PANEL_ROTATION]])))
    cg.add(var.set_default_charge_cap(config[CONF_DEFAULT_CHARGE_CAP]))
    # register_display/setup_display_core_ handles pages/rotation/auto_clear but NOT the
    # top-level lambda — the platform must wire it to the writer itself, or the screen stays blank.
    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_LAMBDA], [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))
    if CONF_ON_TOUCH in config:
        await automation.build_automation(
            var.get_touch_trigger(),
            [(cg.int_, "x"), (cg.int_, "y")],
            config[CONF_ON_TOUCH],
        )
