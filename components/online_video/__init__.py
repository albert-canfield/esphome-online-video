"""Show an MJPEG camera stream as an image, read and decoded off the main loop."""

from esphome import automation
import esphome.codegen as cg
from esphome.components import image
from esphome.components.image import CONF_OPAQUE, add_metadata
import esphome.config_validation as cv
from esphome.const import (
    CONF_BUFFER_SIZE,
    CONF_ID,
    CONF_ON_ERROR,
    CONF_RESIZE,
    CONF_TRIGGER_ID,
    CONF_URL,
)
from esphome.core import CORE
from esphome.types import ConfigType

AUTO_LOAD = ["image"]
CODEOWNERS = ["@albert-canfield"]
DEPENDENCIES = ["display", "network"]
DOMAIN = "online_video"
MULTI_CONF = True

CONF_AUTOSTART = "autostart"
CONF_HEADERS = "headers"
CONF_MAX_FPS = "max_fps"
CONF_ON_FRAME = "on_frame"

MAX_HEADERS = 4

online_video_ns = cg.esphome_ns.namespace("online_video")
OnlineVideo = online_video_ns.class_("OnlineVideo", cg.Component, image.Image_)
StartAction = online_video_ns.class_(
    "StartAction", automation.Action, cg.Parented.template(OnlineVideo)
)
StopAction = online_video_ns.class_(
    "StopAction", automation.Action, cg.Parented.template(OnlineVideo)
)
IsRunningCondition = online_video_ns.class_(
    "IsRunningCondition", automation.Condition, cg.Parented.template(OnlineVideo)
)
FrameTrigger = online_video_ns.class_("FrameTrigger", automation.Trigger.template())
ErrorTrigger = online_video_ns.class_("ErrorTrigger", automation.Trigger.template())


def validate_url(value: str) -> str:
    value = cv.url(value)
    if not value.lower().startswith("http://"):
        raise cv.Invalid(
            "Only http:// URLs are supported. Serve the stream from your local network, "
            "for example with go2rtc."
        )
    return value


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(OnlineVideo),
            cv.Required(CONF_URL): validate_url,
            cv.Required(CONF_RESIZE): cv.dimensions,
            cv.Optional(CONF_MAX_FPS, default=5): cv.int_range(min=1, max=30),
            cv.Optional(CONF_BUFFER_SIZE, default="100kB"): cv.All(
                cv.validate_bytes, cv.int_range(min=8192, max=2_000_000)
            ),
            cv.Optional(CONF_AUTOSTART, default=False): cv.boolean,
            cv.Optional(CONF_HEADERS): cv.All(
                cv.Schema({cv.string_strict: cv.string_strict}),
                cv.Length(max=MAX_HEADERS),
            ),
            cv.Optional(CONF_ON_FRAME): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(FrameTrigger)}
            ),
            cv.Optional(CONF_ON_ERROR): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ErrorTrigger)}
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on(["esp32", "host"]),
)

VIDEO_ACTION_SCHEMA = automation.maybe_simple_id(
    {cv.GenerateID(): cv.use_id(OnlineVideo)}
)


@automation.register_action(
    "online_video.start", StartAction, VIDEO_ACTION_SCHEMA, synchronous=True
)
@automation.register_action(
    "online_video.stop", StopAction, VIDEO_ACTION_SCHEMA, synchronous=True
)
async def video_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


@automation.register_condition(
    "online_video.is_running", IsRunningCondition, VIDEO_ACTION_SCHEMA
)
async def is_running_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


async def to_code(config: ConfigType) -> None:
    width, height = config[CONF_RESIZE]
    # LVGL reads this to know which image formats to enable.
    add_metadata(config[CONF_ID], width, height, "RGB565", CONF_OPAQUE)

    var = cg.new_Pvariable(
        config[CONF_ID],
        config[CONF_URL],
        width,
        height,
        config[CONF_BUFFER_SIZE],
        config[CONF_MAX_FPS],
    )
    await cg.register_component(var, config)
    if config[CONF_AUTOSTART]:
        cg.add(var.set_autostart(True))
    for name, value in config.get(CONF_HEADERS, {}).items():
        cg.add(var.add_header(name, value))

    for conf in config.get(CONF_ON_FRAME, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
    for conf in config.get(CONF_ON_ERROR, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    # Same decoder and version as runtime_image, so both can live in one build.
    cg.add_library("JPEGDEC", "1.8.4", "https://github.com/bitbank2/JPEGDEC#1.8.4")
    if CORE.is_host:
        cg.add_build_flag("-D__LINUX__")
    if CORE.is_esp32:
        from esphome.components.esp32 import add_idf_component

        # JPEGDEC's ESP32-S3 SIMD code needs the esp-dsp headers.
        add_idf_component(name="espressif/esp-dsp", ref="1.8.2")
