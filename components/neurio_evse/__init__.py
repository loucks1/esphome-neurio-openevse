import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import time
from esphome.const import CONF_ID

CODEOWNERS = ["@antigravity"]
DEPENDENCIES = ["time", "mqtt"]
AUTO_LOAD = ["sensor", "number", "switch"]

neurio_evse_ns = cg.esphome_ns.namespace("neurio_evse")
NeurioEvseComponent = neurio_evse_ns.class_("NeurioEvseComponent", cg.PollingComponent)
PersistenceMode = neurio_evse_ns.enum("PersistenceMode")

PERSISTENCE_MODES = {
    "in_memory_first": PersistenceMode.IN_MEMORY_FIRST,
    "write_first": PersistenceMode.WRITE_FIRST,
}

CONF_NEURIO_HOST = "neurio_host"
CONF_NEURIO_PORT = "neurio_port"
CONF_MQTT_OPENEVSE_TOPIC = "mqtt_openevse_topic"
CONF_MQTT_HOURLY_TOPIC = "mqtt_hourly_topic"
CONF_MQTT_ACK_TOPIC = "mqtt_ack_topic"
CONF_MQTT_QUEUE_STATUS_TOPIC = "mqtt_queue_status_topic"
CONF_RETRY_INTERVAL = "retry_interval"
CONF_TIME_ID = "time_id"
CONF_REQUIRE_HOURLY_ACK = "require_hourly_ack"
CONF_PERSISTENCE_MODE = "persistence_mode"
CONF_ACK_TIMEOUT = "ack_timeout"
CONF_BACKOFF_INTERVAL = "backoff_interval"
CONF_SOCKET_RECYCLE_COUNT = "socket_recycle_count"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(NeurioEvseComponent),
            cv.Required(CONF_NEURIO_HOST): cv.string,
            cv.Optional(CONF_NEURIO_PORT, default=80): cv.port,
            cv.Optional(CONF_MQTT_OPENEVSE_TOPIC, default="openevse/grid/power"): cv.string,
            cv.Optional(CONF_MQTT_HOURLY_TOPIC, default="neurio/hourly/energy"): cv.string,
            cv.Optional(CONF_MQTT_ACK_TOPIC, default="neurio/hourly/ack"): cv.string,
            cv.Optional(CONF_MQTT_QUEUE_STATUS_TOPIC, default="neurio/queue/status"): cv.string,
            cv.Optional(CONF_RETRY_INTERVAL, default="5min"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_REQUIRE_HOURLY_ACK, default=True): cv.boolean,
            cv.Optional(CONF_PERSISTENCE_MODE, default="in_memory_first"): cv.enum(PERSISTENCE_MODES, lower=True),
            cv.Optional(CONF_ACK_TIMEOUT, default="30s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_BACKOFF_INTERVAL, default="10s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SOCKET_RECYCLE_COUNT, default=60): cv.positive_int,
            cv.Required(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
        }
    )
    .extend(cv.polling_component_schema("2s"))
)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_neurio_host(config[CONF_NEURIO_HOST]))
    cg.add(var.set_neurio_port(config[CONF_NEURIO_PORT]))
    cg.add(var.set_mqtt_openevse_topic(config[CONF_MQTT_OPENEVSE_TOPIC]))
    cg.add(var.set_mqtt_hourly_topic(config[CONF_MQTT_HOURLY_TOPIC]))
    cg.add(var.set_mqtt_ack_topic(config[CONF_MQTT_ACK_TOPIC]))
    cg.add(var.set_mqtt_queue_status_topic(config[CONF_MQTT_QUEUE_STATUS_TOPIC]))
    cg.add(var.set_retry_interval_ms(config[CONF_RETRY_INTERVAL]))
    cg.add(var.set_require_hourly_ack(config[CONF_REQUIRE_HOURLY_ACK]))
    cg.add(var.set_persistence_mode(config[CONF_PERSISTENCE_MODE]))
    cg.add(var.set_ack_timeout_ms(config[CONF_ACK_TIMEOUT]))
    cg.add(var.set_backoff_interval_ms(config[CONF_BACKOFF_INTERVAL]))
    cg.add(var.set_socket_recycle_count(config[CONF_SOCKET_RECYCLE_COUNT]))

    time_comp = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time_id(time_comp))
