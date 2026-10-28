import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import speaker, microphone
from esphome.const import CONF_ID

CODEOWNERS = ["@powange"]
DEPENDENCIES = ["wifi", "network", "speaker"]

vban_receiver_ns = cg.esphome_ns.namespace("vban_receiver")
VBANReceiver = vban_receiver_ns.class_("VBANReceiver", cg.Component)

CONF_SPEAKER = "speaker"
CONF_MICROPHONE = "microphone"
CONF_LISTEN_PORT = "listen_port"
CONF_STREAM_NAME = "stream_name"
CONF_IDLE_TIMEOUT_MS = "idle_timeout_ms"
CONF_PREBUFFER_MS = "prebuffer_ms"

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(VBANReceiver),
    cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
    cv.Optional(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
    cv.Optional(CONF_LISTEN_PORT, default=6980): cv.port,
    cv.Required(CONF_STREAM_NAME): cv.All(cv.string, cv.Length(max=16)),
    cv.Optional(CONF_IDLE_TIMEOUT_MS, default=1500): cv.positive_int,
    cv.Optional(CONF_PREBUFFER_MS, default=200): cv.int_range(min=0, max=1000),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    spk = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(spk))

    if CONF_MICROPHONE in config:
        mic = await cg.get_variable(config[CONF_MICROPHONE])
        cg.add(var.set_microphone(mic))

    cg.add(var.set_listen_port(config[CONF_LISTEN_PORT]))
    cg.add(var.set_stream_name(config[CONF_STREAM_NAME]))
    cg.add(var.set_idle_timeout_ms(config[CONF_IDLE_TIMEOUT_MS]))
    cg.add(var.set_prebuffer_ms(config[CONF_PREBUFFER_MS]))
