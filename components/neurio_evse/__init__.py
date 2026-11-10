import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sun, time
from esphome.components.esp32 import request_http_client
from esphome.const import CONF_ID, PLATFORM_ESP32

CODEOWNERS = ["@antigravity"]
DEPENDENCIES = ["time", "mqtt"]
AUTO_LOAD = ["sensor", "number", "switch"]

neurio_evse_ns = cg.esphome_ns.namespace("neurio_evse")
NeurioEvseComponent = neurio_evse_ns.class_("NeurioEvseComponent", cg.PollingComponent)
PersistenceMode = neurio_evse_ns.enum("PersistenceMode")
GatedPublishMode = neurio_evse_ns.enum("GatedPublishMode")

PERSISTENCE_MODES = {
    "in_memory_first": PersistenceMode.IN_MEMORY_FIRST,
    "write_first": PersistenceMode.WRITE_FIRST,
}

GATED_MODES = {
    "import_sentinel": GatedPublishMode.IMPORT_SENTINEL,
    "silent": GatedPublishMode.SILENT,
}

CONF_NEURIO_HOST = "neurio_host"
CONF_NEURIO_PORT = "neurio_port"
CONF_MQTT_OPENEVSE_TOPIC = "mqtt_openevse_topic"
CONF_MQTT_OPENEVSE_VRMS_TOPIC = "mqtt_openevse_vrms_topic"
CONF_MQTT_HOURLY_TOPIC = "mqtt_hourly_topic"
CONF_MQTT_ACK_TOPIC = "mqtt_ack_topic"
CONF_RETRY_INTERVAL = "retry_interval"
CONF_TIME_ID = "time_id"
CONF_REQUIRE_HOURLY_ACK = "require_hourly_ack"
CONF_PERSISTENCE_MODE = "persistence_mode"
CONF_ACK_TIMEOUT = "ack_timeout"
CONF_BACKOFF_INTERVAL = "backoff_interval"
CONF_SOCKET_RECYCLE_COUNT = "socket_recycle_count"
CONF_OPENEVSE_WHEN_GATED = "openevse_when_gated"
CONF_OPENEVSE_GATED_POWER = "openevse_gated_power"
CONF_SUN_ID = "sun_id"
CONF_OPENEVSE_MIN_ELEVATION = "openevse_min_elevation"
CONF_POLL_INTERVAL = "poll_interval"
CONF_UPDATE_INTERVAL = "update_interval"

def _validate_config(config):
    if CONF_OPENEVSE_MIN_ELEVATION in config and CONF_SUN_ID not in config:
        raise cv.Invalid("openevse_min_elevation requires sun_id")
    # poll_interval is the name in the device YAML. update_interval is the
    # ESPHome PollingComponent key. Either one sets the poll cadence.
    if CONF_POLL_INTERVAL in config and CONF_UPDATE_INTERVAL in config:
        raise cv.Invalid("Set only one of poll_interval and update_interval")
    if CONF_POLL_INTERVAL in config:
        config[CONF_UPDATE_INTERVAL] = config.pop(CONF_POLL_INTERVAL)
    elif CONF_UPDATE_INTERVAL not in config:
        config[CONF_UPDATE_INTERVAL] = cv.positive_time_period_milliseconds("2s")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(NeurioEvseComponent),
            cv.Required(CONF_NEURIO_HOST): cv.string,
            cv.Optional(CONF_NEURIO_PORT, default=80): cv.port,
            cv.Optional(CONF_MQTT_OPENEVSE_TOPIC, default="openevse/grid/power"): cv.string,
            cv.Optional(CONF_MQTT_OPENEVSE_VRMS_TOPIC, default="emon/emonpi/vrms"): cv.string,
            cv.Optional(CONF_MQTT_HOURLY_TOPIC, default="neurio/hourly/energy"): cv.string,
            cv.Optional(CONF_MQTT_ACK_TOPIC, default="neurio/hourly/ack"): cv.string,
            cv.Optional(CONF_RETRY_INTERVAL, default="5min"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_REQUIRE_HOURLY_ACK, default=True): cv.boolean,
            cv.Optional(CONF_PERSISTENCE_MODE, default="in_memory_first"): cv.enum(
                PERSISTENCE_MODES, lower=True
            ),
            cv.Optional(CONF_ACK_TIMEOUT, default="30s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_BACKOFF_INTERVAL, default="10s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SOCKET_RECYCLE_COUNT, default=60): cv.positive_int,
            cv.Optional(CONF_OPENEVSE_WHEN_GATED, default="import_sentinel"): cv.enum(
                GATED_MODES, lower=True
            ),
            # Positive import large enough that OpenEVSE still sees no export after it
            # subtracts the car's own charge current. See the README.
            cv.Optional(CONF_OPENEVSE_GATED_POWER, default=50000): cv.int_range(
                min=-1000000, max=1000000
            ),
            cv.Optional(CONF_SUN_ID): cv.use_id(sun.Sun),
            cv.Optional(CONF_OPENEVSE_MIN_ELEVATION): cv.float_range(min=-90, max=90),
            cv.Optional(CONF_POLL_INTERVAL): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_UPDATE_INTERVAL): cv.positive_time_period_milliseconds,
            cv.Required(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
    _validate_config,
)


async def to_code(config):
    # esp_http_client is excluded from ESP-IDF builds unless a component asks for it.
    # Works for framework type esp-idf and for Arduino (Arduino-ESP32 is IDF underneath).
    request_http_client()

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_neurio_host(config[CONF_NEURIO_HOST]))
    cg.add(var.set_neurio_port(config[CONF_NEURIO_PORT]))
    cg.add(var.set_mqtt_openevse_topic(config[CONF_MQTT_OPENEVSE_TOPIC]))
    cg.add(var.set_mqtt_openevse_vrms_topic(config[CONF_MQTT_OPENEVSE_VRMS_TOPIC]))
    cg.add(var.set_mqtt_hourly_topic(config[CONF_MQTT_HOURLY_TOPIC]))
    cg.add(var.set_mqtt_ack_topic(config[CONF_MQTT_ACK_TOPIC]))
    cg.add(var.set_retry_interval_ms(config[CONF_RETRY_INTERVAL]))
    cg.add(var.set_require_hourly_ack(config[CONF_REQUIRE_HOURLY_ACK]))
    cg.add(var.set_persistence_mode(config[CONF_PERSISTENCE_MODE]))
    cg.add(var.set_ack_timeout_ms(config[CONF_ACK_TIMEOUT]))
    cg.add(var.set_backoff_interval_ms(config[CONF_BACKOFF_INTERVAL]))
    cg.add(var.set_socket_recycle_count(config[CONF_SOCKET_RECYCLE_COUNT]))
    cg.add(var.set_gated_mode(config[CONF_OPENEVSE_WHEN_GATED]))
    cg.add(var.set_gated_power(config[CONF_OPENEVSE_GATED_POWER]))

    time_comp = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time_id(time_comp))

    if CONF_SUN_ID in config:
        cg.add_define("USE_NEURIO_SUN_GATE")
        sun_var = await cg.get_variable(config[CONF_SUN_ID])
        cg.add(var.set_sun(sun_var))
        cg.add(var.set_min_elevation(config.get(CONF_OPENEVSE_MIN_ELEVATION, 0.0)))
