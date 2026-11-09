import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    UNIT_VOLT,
    UNIT_WATT,
    UNIT_EMPTY,
)
from . import NeurioEvseComponent

CONF_NEURIO_EVSE_ID = "neurio_evse_id"
CONF_NET_POWER = "net_power"
CONF_GENERATION_POWER = "generation_power"
CONF_CONSUMPTION_POWER = "consumption_power"
CONF_PHASE_A_POWER = "phase_a_power"
CONF_PHASE_B_POWER = "phase_b_power"
CONF_REPORTED_EVSE_POWER = "reported_evse_power"
CONF_PHASE_A_VOLTAGE = "phase_a_voltage"
CONF_PHASE_B_VOLTAGE = "phase_b_voltage"
CONF_QUEUE_PENDING_COUNT = "queue_pending_count"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_NEURIO_EVSE_ID): cv.use_id(NeurioEvseComponent),
        cv.Optional(CONF_NET_POWER): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_GENERATION_POWER): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_CONSUMPTION_POWER): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_PHASE_A_POWER): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_PHASE_B_POWER): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_REPORTED_EVSE_POWER): sensor.sensor_schema(
            unit_of_measurement=UNIT_WATT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_POWER,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_PHASE_A_VOLTAGE): sensor.sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_PHASE_B_VOLTAGE): sensor.sensor_schema(
            unit_of_measurement=UNIT_VOLT,
            accuracy_decimals=1,
            device_class=DEVICE_CLASS_VOLTAGE,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_QUEUE_PENDING_COUNT): sensor.sensor_schema(
            unit_of_measurement=UNIT_EMPTY,
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
        ),
    }
)

async def to_code(config):
    hub = await cg.get_variable(config[CONF_NEURIO_EVSE_ID])

    sensor_map = [
        (CONF_NET_POWER, hub.set_net_power_sensor),
        (CONF_GENERATION_POWER, hub.set_generation_power_sensor),
        (CONF_CONSUMPTION_POWER, hub.set_consumption_power_sensor),
        (CONF_PHASE_A_POWER, hub.set_phase_a_power_sensor),
        (CONF_PHASE_B_POWER, hub.set_phase_b_power_sensor),
        (CONF_REPORTED_EVSE_POWER, hub.set_reported_evse_power_sensor),
        (CONF_PHASE_A_VOLTAGE, hub.set_phase_a_voltage_sensor),
        (CONF_PHASE_B_VOLTAGE, hub.set_phase_b_voltage_sensor),
        (CONF_QUEUE_PENDING_COUNT, hub.set_queue_pending_sensor),
    ]

    for key, setter in sensor_map:
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(setter(s))
