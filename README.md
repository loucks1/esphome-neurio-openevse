# ESPHome Neurio to OpenEVSE Bridge

An ESPHome external component for ESP32 that polls a Neurio energy monitor on the LAN (`http://<ip>/current-sample`) and publishes over MQTT: the raw JSON for Home Assistant, net power plus an offset for OpenEVSE solar divert, and an hourly energy queue that survives broker outages.

It builds on **ESP-IDF** (`framework: type: esp-idf`) and on the **Arduino** framework. ESP-IDF is the one to use on a board that also runs `bluetooth_proxy`. Tested configuration: ESPHome **2026.10.x**, `board: esp32dev`.

---

## Features

1. **Local polling with socket hygiene**
   * HTTP GET of `/current-sample` through ESP-IDF `esp_http_client` (keep-alive, 800 ms timeout).
   * On any timeout or bad response the TCP connection is closed immediately, so Neurio is less likely to stick sockets in `CLOSE_WAIT`.
   * After 3 consecutive failures, polling backs off to `backoff_interval` (default 10 s).
   * The TCP connection is recycled every `socket_recycle_count` successes (default 60).
   * Parses channels 1–5 (phase A, phase B, net, generation, consumption), including `eImp_Ws` / `eExp_Ws` as `uint64_t`.
   * `poll_interval` is the normal ESPHome poll interval (default **2 s**). **5 s** is a good fit for OpenEVSE, which expects grid updates about every 5–10 s, and it leaves more of the 80 MHz core for Wi-Fi, MQTT, and an active BLE proxy.

2. **OpenEVSE feed, offset, and a sun gate**
   * Reported watts are channel 3 net plus the offset: \(P_{reported} = P_{net} + P_{offset}\).
   * The topic is `mqtt_openevse_topic` (default `openevse/grid/power`; a common divert topic is `emon/emonpi/power1`). Payload is a decimal integer, QoS 0.
   * The offset is a Home Assistant number entity (`EVSE Grid Power Offset`, −10000…10000 W, step 50). The value is stored in flash and restored on boot.
   * **`neurio/raw` is never gated.** Home Assistant always gets the untouched Neurio body.

3. **Hourly queue in NVS**
   * At the top of the hour the component snapshots the hardware watt-second counters and publishes `neurio/hourly/energy` (QoS 1).
   * Records wait in RAM for `ack_timeout` (default 30 s). If no ACK arrives they are written to NVS and retried every `retry_interval` (default 5 min). `write_first` stores them before the first publish.
   * Capacity is 120 records. ACK topic `neurio/hourly/ack` accepts a bare id or `{"id": <id>}`.
   * Persistence uses the ESPHome NVS partition (namespace `neurio`), not LittleFS. ESPHome’s esp-idf 4 MB table gives that partition 448 KB, which is shared with Wi-Fi, BLE bonds, and preferences. A record is 144 bytes, so 120 records are about 17 KB. No extra flash partition is required, and an OTA from an existing esp-idf ESPHome image does not need a new partition table.

4. **Require-ACK switch**
   * `Require Hourly ACK` can be turned off from Home Assistant to publish hourly records without queueing them.

---

## OpenEVSE feed gate

Home Assistant should own the gate. The component exposes a switch, **OpenEVSE feed enabled**, with `restore_mode: RESTORE_DEFAULT_ON` (change to `RESTORE_DEFAULT_OFF` if a reboot should stay quiet until Home Assistant turns it on).

```yaml
switch:
  - platform: neurio_evse
    neurio_evse_id: neurio_hub
    openevse_feed:
      name: "OpenEVSE feed enabled"
      restore_mode: RESTORE_DEFAULT_ON
```

Example automation (sun elevation strictly above 0, matching a `numeric_state` `above: 0` trigger):

```yaml
automation:
  - alias: OpenEVSE feed follows the sun
    triggers:
      - trigger: numeric_state
        entity_id: sun.sun
        attribute: elevation
        above: 0
      - trigger: numeric_state
        entity_id: sun.sun
        attribute: elevation
        below: 0
      - trigger: homeassistant
        event: start
    actions:
      - if:
          - condition: numeric_state
            entity_id: sun.sun
            attribute: elevation
            above: 0
        then:
          - action: switch.turn_on
            target:
              entity_id: switch.rf433sniffer_openevse_feed_enabled
        else:
          - action: switch.turn_off
            target:
              entity_id: switch.rf433sniffer_openevse_feed_enabled
```

The entity id follows the ESPHome name. Confirm it in Home Assistant after the first boot.

### What OpenEVSE does when samples stop

Current OpenEVSE ESP32 firmware (`openevse_esp32_firmware` `src/divert.cpp`, master as of 2026-11-09) has **no stale-data timeout**. `DivertTask::update_state()` runs only when a new MQTT grid or solar message arrives. `_last_update` is stored and never compared to a deadline. The divert claim (charge current, or disabled) stays where the last sample left it. Stopping the publisher **latches** Eco mode on the last export value, including overnight.

The divert math also subtracts the car’s own charge current from the grid reading:

```text
Igrid = grid_ie_watts / voltage - ev_amps
excess when Igrid < 0
```

So `0` (or a small positive number) published while the car is already charging still looks like export. A single “stop” sample is not enough either: smoothing and `divert_min_charge_time` only advance when new samples arrive.

**Default while the gate is closed:** every poll publishes `openevse_gated_power` (default **50000** W, a large import) to the OpenEVSE topic only. 50000 W stays positive after subtracting a 48 A single-phase charge (~12 kW) or a 32 A three-phase charge (~23 kW). Raise it if the charger is larger. The repeated samples let the smoothing filter decay and let the minimum charge time expire; the contactor may stay closed until that timer finishes. That is OpenEVSE’s behavior, not something this device can skip.

`openevse_when_gated: silent` publishes nothing instead. Use that only if another publisher owns the topic, or you accept that Eco mode will hold the last value. Do not point the sentinel at a topic that emoncms (or anything else) treats as a real CT. `neurio/raw` is unchanged in both modes.

The reported-power sensor follows what was actually published (the sentinel, or `unknown` in silent mode). Net, generation, and the other Neurio sensors keep updating.

### Optional on-device sun gate

If the ESP has `sun:` configured, set `sun_id` and optionally `openevse_min_elevation` (default 0). The feed is live only when the switch is on **and** elevation is **strictly greater** than that minimum. If SNTP has not synced yet, elevation is ignored and the switch alone decides, so a reboot does not force the sentinel for the whole clock-sync window. The switch still wins when it is off: local sun cannot force the feed on.

```yaml
sun:
  id: sun_site
  latitude: 0.0   # replace
  longitude: 0.0  # replace

neurio_evse:
  sun_id: sun_site
  openevse_min_elevation: 0.0
```

---

## Heap

The poll path does not allocate a JSON document. The previous Arduino build parsed every sample with a 4096-byte `DynamicJsonDocument` and copied the body into an Arduino `String`.

| What | When | Approximate size |
| --- | --- | --- |
| HTTP body buffer | BSS for the life of the process | 3072 bytes, one instance |
| `esp_http_client` RX/TX buffers | heap, once in `setup` | 1024 + 256 bytes, plus the client object (about 2 KB) |
| Hourly record | RAM until ACK, then a 144-byte NVS blob | one record in flight is typical |
| MQTT publish of `neurio/raw` | copy inside the MQTT client for that send | size of the Neurio body, usually under 1.5 KB |

`setup` logs free heap once. Every 60 successful polls a debug line logs it again. The HTTP GET still blocks the main loop for the LAN round-trip, up to the 800 ms timeout; a 5 s interval reduces how often that happens next to BLE scanning.

An ESPHome 2026.10.2 compile of `examples/rf433sniffer-esp-idf.yaml` (esp-idf 5.5.6, `bluetooth_proxy` active, API, web server, MQTT, sun, this component) reported:

| | Used | Total |
| --- | --- | --- |
| DRAM | 76364 bytes (61.3%) | 124580 bytes, 48216 left for heap |
| IRAM | 107555 bytes (82.1%) | 131072 bytes |
| Flash | 1356527 bytes (73.9%) | 1835008 byte app partition |

That DRAM remainder is the pool Wi-Fi, BLE, and the HTTP client allocate from. It is not the free-heap number after those stacks start. The component does not add a 4 KB JSON allocation on each poll. IRAM on this image is already tight because of the BLE proxy; further IRAM-resident features may not fit. The same sources built as Arduino (no BLE proxy) used 53084 bytes of DRAM (29.4%) and 975075 bytes of flash (53.1%).

---

## Directory structure

```
esphome-neurio-openevse/
├── esp32-neurio-openevse.yaml          # esp-idf example
├── examples/
│   ├── rf433sniffer-esp-idf.yaml       # esp32dev + ble proxy + gate + sun
│   └── esp32-arduino.yaml              # Arduino framework, no BLE proxy
└── components/neurio_evse/
```

---

## MQTT topics

### OpenEVSE (`mqtt_openevse_topic`, default `openevse/grid/power`)

QoS 0. Payload is an integer watt string such as `-2256` or `1400`. Positive is import, negative is export. While the feed gate is closed in `import_sentinel` mode the payload is `openevse_gated_power` instead of net+offset.

### Raw Neurio (`neurio/raw`)

QoS 0. Exact `/current-sample` body, including `channels`. Not gated.

```yaml
mqtt:
  sensor:
    - name: "Neurio Raw"
      state_topic: "neurio/raw"
      json_attributes_topic: "neurio/raw"
      value_template: "{{ value_json.timestamp }}"
```

### Hourly energy (`neurio/hourly/energy`)

QoS 1. JSON with `id`, `timestamp`, `retries`, and `net` / `generation` / `consumption` / `phase_a` / `phase_b` kWh fields (3 decimal places). Deltas are computed from the watt-second counters.

### ACK (`neurio/hourly/ack`)

A decimal id, or `{"id": 1730980800}`.

### Queue status (`neurio/queue/status`)

```json
{"pending_records": 3, "oldest_id": 1730970000}
```

---

## Build

ESPHome 2026.10 or newer:

```bash
esphome compile examples/rf433sniffer-esp-idf.yaml
esphome compile examples/esp32-arduino.yaml
```

`examples/rf433sniffer-esp-idf.yaml` matches the rf433sniffer device shape: `esp32dev`, esp-idf, 80 MHz, `bluetooth_proxy` active, `api`, `web_server`, the feed switch, and a placeholder `sun:`. Replace the placeholder Wi-Fi, MQTT password, and coordinates. Do not commit real secrets.

Arduino still compiles (`examples/esp32-arduino.yaml`) using the same `esp_http_client` and NVS code. `bluetooth_proxy` on ESP32 is an esp-idf feature, so that example does not enable it.

---

## Configuration

| Key | Default | Role |
| --- | --- | --- |
| `neurio_host` | required | Neurio IP or hostname |
| `neurio_port` | 80 | |
| `poll_interval` | 2 s | Alias of `update_interval`. Try 5 s with BLE proxy |
| `mqtt_openevse_topic` | `openevse/grid/power` | Divert feed |
| `mqtt_raw_topic` | `neurio/raw` | Never gated |
| `openevse_when_gated` | `import_sentinel` | or `silent` |
| `openevse_gated_power` | 50000 | Watts published while gated |
| `sun_id` | unset | Optional local elevation gate |
| `openevse_min_elevation` | 0 with `sun_id` | Live only when elevation is greater |
| `persistence_mode` | `in_memory_first` | or `write_first` |
| `require_hourly_ack` | true | |

---

## Tests

```bash
python tests/run_tests.py
```

Host C++ tests cover the energy math, offset, ACK parser, 120-record queue, Neurio JSON parser, hourly JSON, and the feed-gate decision. Python tests cover the payload shape and the queue state machine. They do not flash a device.

---

## Cutover notes

* Disable the Home Assistant automation that also publishes `emon/emonpi/power1` before the ESP starts, or the two writers will fight.
* Seed **EVSE Grid Power Offset** to the value the old helper used (often −500). It is restored from flash after the first set.
* Turn on an automation for **OpenEVSE feed enabled** before or immediately after flashing if `restore_mode` is `RESTORE_DEFAULT_ON`. Until that switch is off, the device publishes real net+offset, including at night.
* Only one HTTP client should hit Neurio. Remove the Home Assistant REST poll when the ESP is publishing `neurio/raw`.
* Hourly ACKs are optional. Nothing in Home Assistant acks them today; turn **Require Hourly ACK** off if you do not want the NVS queue to fill.
