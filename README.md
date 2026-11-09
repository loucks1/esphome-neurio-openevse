# ESPHome Neurio to OpenEVSE Bridge

An outage-resilient custom ESPHome component for ESP32 that bridges a local Neurio energy monitor with an OpenEVSE charger over MQTT, complete with dynamic offset control and a LittleFS store-and-forward queue for hourly energy reporting.

---

## Features

1. **High-Frequency Local Polling (1s – 2s) with Socket Hygiene:**
   * Uses persistent HTTP `Connection: keep-alive` to stream `/current-sample` directly from Neurio over LAN.
   * **Aggressive Socket Teardown:** Immediately resets and terminates the TCP socket upon any timeout or error, preventing Neurio's embedded OS from trapping sockets in `CLOSE_WAIT` / `TIME_WAIT` (which causes the 20-minute freeze).
   * **Adaptive Backoff:** If Neurio fails 3 consecutive times, automatically backs off to a relaxed polling rate (`backoff_interval: 10s`) to let Neurio sweep its socket buffers before resuming 1s–2s polling.
   * **Socket Recycling:** Cleanly cycles the TCP connection every 60 requests (`socket_recycle_count: 60`, ~2 min) to prevent stale keep-alive desynchronization.
   * Parses Phase A (Ch 1), Phase B (Ch 2), Net (Ch 3), Generation (Ch 4), and Consumption (Ch 5).
   * Uses `uint64_t` for `eImp_Ws` and `eExp_Ws` counters to prevent 32-bit integer overflow.

2. **OpenEVSE Real-Time Ingestion & Dynamic Offset:**
   * Uses Channel 3 (`Net`) real-time power ($P_{\text{net}}$).
   * Applies configurable offset: $P_{\text{reported}} = P_{\text{net}} + P_{\text{offset}}$.
   * Reports directly to `openevse/grid/power` at QoS 0.
   * Offset is controlled natively in Home Assistant via the `EVSE Grid Power Offset` slider entity.

3. **Store-and-Forward Hourly Queue (LittleFS):**
   * Snaps hardware energy counters at `MM:00:00` (top of every hour).
   * Computes exact hourly deltas: $\Delta\text{kWh} = \frac{\Delta\text{Ws}}{3,600,000}$.
   * Stores full channel breakdowns into ESP32 Flash via LittleFS (`/neurio_q/rec_<id>.json`).
   * Retains up to **120 hours (5 days)** of records across reboots and network outages.

4. **MQTT Resilience, Retries & Backlog Telemetry:**
   * Records are retried every **5 minutes** while unacknowledged.
   * Backlog status is published to `neurio/queue/status` (`{"pending_records": N, "oldest_id": ...}`).
   * Acknowledging an hourly record deletes it permanently from flash storage.

5. **Configurable Persistence Modes:**
   * **`in_memory_first` (Default):** The hourly record is held in RAM when published. If an ACK arrives within the grace period (`ack_timeout`, default 30s), the record is cleared from RAM—**flash is never touched during normal operation!** If no ACK arrives or MQTT is disconnected, the record is flushed to LittleFS for the 5-minute retry cycle.
   * **`write_first` (Write-Ahead):** Writes every hourly record to LittleFS flash immediately before sending over MQTT, ensuring zero data loss if power cuts out immediately after rollover.

6. **Testing Mode (Toggleable ACK Requirement):**
   * Configurable via YAML: `require_hourly_ack: false`.
   * **Live Home Assistant Switch:** Exposes a toggle switch entity (`Require Hourly ACK`) so you can disable ACK waiting on the fly during testing without reflashing. When disabled, hourly records are published directly to MQTT without enqueueing or 5-minute retries.

---

## Directory Structure

```
esphome-neurio-openevse/
├── esp32-neurio-openevse.yaml
└── components/
    └── neurio_evse/
        ├── __init__.py
        ├── sensor.py
        ├── number.py
        ├── neurio_evse.h
        └── neurio_evse.cpp
```

---

## MQTT Topics & Contracts

### 1. OpenEVSE Real-Time Feed
* **Topic:** `openevse/grid/power`
* **QoS:** `0`
* **Payload:** Stringified integer (Watts), e.g. `-2256` (exporting 2.2 kW) or `1400` (importing 1.4 kW).

### 2. Raw Neurio Channels Feed (Home Assistant Compatible)
* **Topic:** `neurio/raw`
* **QoS:** `0`
* **Payload:** Untouched 1:1 raw JSON string as returned by Neurio `/current-sample` on every poll (includes `sensorId`, `timestamp`, and the complete `channels` array with all 5 channels, `p_W`, `eImp_Ws`, `eExp_Ws`, and `v_V`).
* **Home Assistant Template Sensor Integration:**
  ```yaml
  # In Home Assistant configuration.yaml
  mqtt:
    sensor:
      - name: "Neurio Raw"
        state_topic: "neurio/raw"
        json_attributes_topic: "neurio/raw"
        value_template: "{{ value_json.timestamp }}"
  ```

### 3. Hourly Energy Report
* **Topic:** `neurio/hourly/energy`
* **QoS:** `1`
* **Payload:**
```json
{
  "id": 1730980800,
  "timestamp": "2026-11-07T12:00:00Z",
  "retries": 0,
  "net": {
    "import_kwh": 123733.774,
    "export_kwh": 65625.888,
    "delta_import_kwh": 1.450,
    "delta_export_kwh": 2.256,
    "delta_net_kwh": -0.806
  },
  "generation": {
    "export_kwh": 40.379,
    "delta_export_kwh": 2.910
  },
  "consumption": {
    "import_kwh": 173966.709,
    "delta_import_kwh": 0.654
  },
  "phase_a": {
    "delta_import_kwh": 0.568,
    "voltage_v": 123.7
  },
  "phase_b": {
    "delta_import_kwh": 0.086,
    "voltage_v": 124.0
  }
}
```

### 3. Acknowledgment Contract
To acknowledge receipt and instruct the ESP32 to delete the hourly record from flash:
* **Topic:** `neurio/hourly/ack`
* **Payload:** Either raw integer ID string (e.g. `1730980800`) or JSON (`{"id": 1730980800}`).

### 4. Backlog Status Telemetry
Published whenever there are unacknowledged records pending (and once when cleared):
* **Topic:** `neurio/queue/status`
* **Payload:**
```json
{
  "pending_records": 3,
  "oldest_id": 1730970000
}
```

---

## Flashing & Deployment

### Using ESPHome CLI:
```bash
cd C:\Users\jason\.gemini\antigravity\scratch\esphome-neurio-openevse
esphome compile esp32-neurio-openevse.yaml
esphome run esp32-neurio-openevse.yaml
```

### Using Home Assistant ESPHome Add-on:
1. Copy the `esphome-neurio-openevse` folder or its contents into your Home Assistant `/config/esphome/` directory.
2. Edit `esp32-neurio-openevse.yaml` with your Wi-Fi credentials, Neurio IP, and MQTT broker IP.
3. Click **Install** in the ESPHome dashboard.

---

## Automated Test Suite

A comprehensive test suite is included under `tests/`:

1. **Native C++ Unit Tests (`test_unit.cpp`):**
   * Energy math & 64-bit Watt-second conversion precision.
   * OpenEVSE dynamic offset calculation and boundary handling.
   * ACK parsing (supporting both integer strings and JSON objects).
   * Outage store-and-forward queue mechanics (120-record capacity, FIFO eviction, and ACK resolution).

2. **Python End-to-End Simulation (`test_simulation.py`):**
   * Parses the exact 5-channel Neurio payload.
   * Simulates top-of-the-hour latching and delta calculation.
   * Emulates multi-hour network outages, 5-minute retry increments, and downstream ACK handling.

### Running the Tests:
```bash
python tests/run_tests.py
```
