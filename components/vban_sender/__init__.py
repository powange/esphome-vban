import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import microphone
from esphome.const import CONF_ID, CONF_MICROPHONE

CODEOWNERS = ["@powange"]
DEPENDENCIES = ["wifi", "network", "microphone"]
AUTO_LOAD = []

vban_sender_ns = cg.esphome_ns.namespace("vban_sender")
VBANSender = vban_sender_ns.class_("VBANSender", cg.Component)

CONF_TARGET_IP = "target_ip"
CONF_TARGET_PORT = "target_port"
CONF_TARGETS = "targets"
CONF_IP = "ip"
CONF_PORT = "port"
CONF_STREAM_NAME = "stream_name"
CONF_GAIN = "gain"

vban_stream_name = cv.All(cv.string, cv.Length(max=16))

TARGET_SCHEMA = cv.Schema({
    cv.Required(CONF_IP): cv.string,
    cv.Optional(CONF_PORT, default=6980): cv.port,
})


def _validate_target_port(config):
    if CONF_TARGETS in config and CONF_TARGET_PORT in config:
        raise cv.Invalid(
            f"'{CONF_TARGET_PORT}' cannot be used with '{CONF_TARGETS}'; "
            f"set 'port' on each target instead"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema({
        cv.GenerateID(): cv.declare_id(VBANSender),
        cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        cv.Optional(CONF_TARGET_IP): cv.string,
        cv.Optional(CONF_TARGET_PORT): cv.port,
        cv.Optional(CONF_TARGETS): cv.All(cv.ensure_list(TARGET_SCHEMA), cv.Length(min=1)),
        cv.Optional(CONF_STREAM_NAME, default="AtomEcho"): vban_stream_name,
        cv.Optional(CONF_GAIN, default=1.0): cv.float_range(min=0.0),
    }).extend(cv.COMPONENT_SCHEMA),
    cv.has_exactly_one_key(CONF_TARGET_IP, CONF_TARGETS),
    _validate_target_port,
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    mic = await cg.get_variable(config[CONF_MICROPHONE])
    cg.add(var.set_microphone(mic))

    if CONF_TARGETS in config:
        for target in config[CONF_TARGETS]:
            cg.add(var.add_target(target[CONF_IP], target[CONF_PORT]))
    else:
        cg.add(var.add_target(config[CONF_TARGET_IP], config.get(CONF_TARGET_PORT, 6980)))
    cg.add(var.set_stream_name(config[CONF_STREAM_NAME]))
    cg.add(var.set_gain(config[CONF_GAIN]))
