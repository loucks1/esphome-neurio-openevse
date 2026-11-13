# ESPHome Neurio to OpenEVSE Bridge

An ESPHome external component for ESP32 that polls a Neurio energy monitor on the LAN (`http://<ip>/current-sample`) and exposes the readings to Home Assistant as native ESPHome API entities. MQTT is used only where a broker is actually needed: net power plus an offset for OpenEVSE solar divert, and an hourly energy queue (with ACKs) that survives broker outages.

It builds on **ESP-IDF** (`framework: type: esp-idf`) and on the **Arduino** framework. ESP-IDF is the one to use on a board that also runs `bluetooth_proxy`. Tested configuration: ESPHome **2026.10.x**, `board: esp32dev`.

---

## Features

1. **Local polling with socket hygiene**
   * HTTP GET of `/current-sample` through ESP-IDF `esp_http_client` (800 ms timeout, TCP keep-alive off). Neurio answers HTTP/1.0 with no `Content-Length` and closes the socket; that body is accepted when the top-level JSON object is complete. A short `Content-Length` response is still a failure. `esp_http_client` logs an `Incomplete chunked data received` error for that normal close, so the component silences the `HTTP_CLIENT` log tag for the duration of each request and reports the outcome itself (debug for the expected close-delimited success, warning for real failures) instead of spamming the log every poll. Runtime tag control is what makes that possible, and ESPHome's esp32 codegen turns it off by default — so the component re-enables it in sdkconfig (`CONFIG_LOG_DYNAMIC_LEVEL_CONTROL` with the linked-list tag-level implementation), at the cost of a small tag-level cache in RAM.
   * On any timeout or bad response the TCP connection is closed immediately, so Neurio is less likely to stick sockets in `CLOSE_WAIT`.
   * After 3 consecutive failures, polling backs off to `backoff_interval` (default 10 s).
   * The TCP connection is recycled every `socket_recycle_count` successes (default 60).
   * Parses channels 1–5 (phase A, phase B, net, generation, consumption), including `eImp_Ws` / `eExp_Ws` as `uint64_t`.
   * Solar production — the `generation_energy` sensor and the hourly `generation` fields — is the GENERATION channel register that has actually accumulated energy: the larger of `eImp_Ws` and `eExp_Ws`. A tie uses import. CT orientation decides which register moves. On the monitor this was measured on, `eImp_Ws` is about 417.7 billion Ws (~116,000 kWh of lifetime solar) and `eExp_Ws` is about 145 million Ws (~40.39 kWh). Reading export alone publishes that residual and an hourly solar delta of about zero. NET import and export stay on channel 3's own registers. Consumption stays on channel 5 `eImp_Ws`. The phase channels stay on their import counters.
   * `poll_interval` is the normal ESPHome poll interval (default **2 s**). **5 s** is a good fit for OpenEVSE, which expects grid updates about every 5–10 s, and it leaves more of the 80 MHz core for Wi-Fi, MQTT, and an active BLE proxy.

2. **OpenEVSE feed and offset**
   * Reported watts are channel 3 net plus the offset: \(P_{reported} = P_{net} + P_{offset}\).
   * The topic is `mqtt_openevse_topic` (default `openevse/grid/power`; a common divert topic is `emon/emonpi/power1`). Payload is a decimal integer, QoS 0.
   * The measured line voltage (phase A + phase B, one decimal place) is published to `mqtt_openevse_vrms_topic` (default `emon/emonpi/vrms`) on every successful poll, so OpenEVSE's divert math divides grid watts by the real voltage instead of an assumed default. It is not passed through the feed gate. Set the topic to `""` to disable it.
   * The offset is a Home Assistant number entity (`EVSE Grid Power Offset`, −10000…10000 W, step 50). The value is stored in flash and restored on boot.
   * Live values reach Home Assistant through the ESPHome API entities, not MQTT. There is no raw MQTT feed.

3. **Hourly queue in NVS**
   * At each UTC hour boundary the component snapshots the hardware watt-second counters and publishes `neurio/hourly/energy` (QoS 1). The ESPHome timezone is not the bucket: `id` is that UTC hour's Unix epoch, and `timestamp` is the same instant with a `Z` suffix. DST fall-back therefore does not merge two real hours, and spring-forward does not skip one.
   * Records are written to NVS as the hour turns — before the first publish, and before the baseline counters advance — and are retried every `retry_interval` (default 5 min) until ACKed. The persisted baseline only advances once every record it summarises is in NVS or has been ACKed, so a reboot cannot lose an hour whose baseline was already committed. If the NVS write itself fails, the record is held in RAM and the write is retried starting at `ack_timeout` (default 30 s) and every 60 s after that, with the persisted baseline held back until the record lands. A queued record whose stored blob no longer matches the current layout (a firmware version change) is not dropped silently: it is re-published from its stored bytes when they can be interpreted under the current layout, and only discarded — with an error log counting the lost hour — when they cannot.
   * Capacity is 336 records (14 days, matching the recorder's settle window for late-arriving cloud data). When the queue is full, the oldest record is dropped and logged at error level. ACK topic `neurio/hourly/ack` accepts a bare id or `{"id": <id>}`.
   * Persistence uses the ESPHome NVS partition (namespace `neurio`), not LittleFS. ESPHome’s esp-idf 4 MB table gives that partition 448 KB, which is shared with Wi-Fi, BLE bonds, and preferences. A record is 144 bytes, so a full 336-record queue is about 47 KB of payload, plus per-entry NVS overhead. No extra flash partition is required, and an OTA from an existing esp-idf ESPHome image does not need a new partition table.

4. **Require-ACK switch**
   * `Require Hourly ACK` can be turned off from Home Assistant to publish hourly records without queueing them.

---

## OpenEVSE feed gate

Home Assistant should own the gate. The component exposes a switch, **OpenEVSE feed enabled**, with `restore_mode: RESTORE_DEFAULT_ON` (change to `RESTORE_DEFAULT_OFF` if a reboot should stay quiet until Home Assistant turns it on). It is the only gate — the component has no on-device gate of its own, so any policy (such as sun elevation) lives in a Home Assistant automation driving this switch, like the example below.

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

`openevse_when_gated: silent` publishes nothing instead. Use that only if another publisher owns the topic, or you accept that Eco mode will hold the last value. Do not point the sentinel at a topic that emoncms (or anything else) treats as a real CT.

The reported-power sensor follows what was actually published (the sentinel, or `unknown` in silent mode). Net, generation, and the other Neurio sensors keep updating.

---

## Heap

The poll path does not allocate a JSON document. The previous Arduino build parsed every sample with a 4096-byte `DynamicJsonDocument` and copied the body into an Arduino `String`.

| What | When | Approximate size |
| --- | --- | --- |
| HTTP body buffer | BSS for the life of the process | 3072 bytes, one instance |
| `esp_http_client` RX/TX buffers | heap, once in `setup` | 1024 + 256 bytes, plus the client object (about 2 KB) |
| Hourly record | 144-byte NVS blob from creation, plus a RAM copy until ACK | one record in flight is typical |

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
│   ├── minimal.yaml                    # smallest useful entity set (see below)
│   ├── rf433sniffer-esp-idf.yaml       # esp32dev + ble proxy + gate
│   └── esp32-arduino.yaml              # Arduino framework, no BLE proxy
└── components/neurio_evse/
```

---

## Home Assistant entities are opt-in

Home Assistant only receives the entities you declare in the device YAML. Every sensor key is optional; an entity you leave out is never created, and leaving one out changes nothing else — polling, the OpenEVSE feed, and the hourly queue all run the same either way. `examples/minimal.yaml` shows the smallest useful set (net power, reported EVSE power, and the import/export energy counters); the full examples declare everything.

Sensor keys you can declare under `sensor: - platform: neurio_evse`:

| Key | Unit / class |
| --- | --- |
| `net_power`, `generation_power`, `consumption_power`, `phase_a_power`, `phase_b_power`, `reported_evse_power` | W, measurement |
| `phase_a_voltage`, `phase_b_voltage` | V, measurement |
| `queue_pending_count` | count of hourly records waiting for an ACK |
| `net_import_energy`, `net_export_energy` | kWh, `total_increasing`. NET channel `eImp_Ws` and `eExp_Ws` |
| `generation_energy` | kWh, `total_increasing`. Larger of the GENERATION channel's `eImp_Ws` and `eExp_Ws` (import on a tie) |
| `consumption_energy` | kWh, `total_increasing`. CONSUMPTION channel `eImp_Ws` |

The controls work the same way: the `switch:` and `number:` platforms are separate blocks you can omit entirely if you do not want Home Assistant controls (the hub then just follows its `neurio_evse:` config, e.g. `require_hourly_ack:`). If you do declare the switch platform, the `require_hourly_ack` switch is required within it and `openevse_feed` is optional; the number platform declares the single `grid_offset` number. You can also disable any entity from the Home Assistant UI without reflashing.

Channels are not configurable: the component expects the Neurio wired as channel 1 = phase A, 2 = phase B, 3 = net, 4 = generation, 5 = consumption, and the OpenEVSE feed and hourly math rely on that layout.

---

## MQTT topics

MQTT carries only the OpenEVSE feed and the hourly energy/ACK flow. Live Neurio values are **not** published over MQTT: Home Assistant receives them as ESPHome API entities (the sensors, switches, and number defined in the device YAML). The hourly backlog count is likewise an ESPHome sensor, not a topic.

### OpenEVSE (`mqtt_openevse_topic`, default `openevse/grid/power`)

QoS 0. Payload is an integer watt string such as `-2256` or `1400`. Positive is import, negative is export. While the feed gate is closed in `import_sentinel` mode the payload is `openevse_gated_power` instead of net+offset.

### OpenEVSE voltage (`mqtt_openevse_vrms_topic`, default `emon/emonpi/vrms`)

QoS 0. Payload is the line-to-line voltage with one decimal place, e.g. `241.7` — the sum of the phase A and phase B voltages, published on every successful poll when both phases report a plausible voltage. OpenEVSE divides grid watts by this voltage to get current in its divert math. Not gated by the feed switch. Set `mqtt_openevse_vrms_topic: ""` to disable it.

### Hourly energy (`mqtt_hourly_topic`, default `neurio/hourly/energy`)

QoS 1. JSON with `id`, `timestamp`, `retries`, and `net` / `generation` / `consumption` / `phase_a` / `phase_b` kWh fields (3 decimal places). Deltas are the increase in the watt-second counters since the previous snapshot. `id` is the UTC hour (Unix epoch floored to 3600 s). `timestamp` is that same instant as UTC (`YYYY-MM-DDTHH:MM:SSZ`), formatted from `id` when the record is published. A record already stored in NVS keeps its 144-byte layout and its id; the next publish replaces a local wall-clock string that was saved with a `Z` suffix, and the retry rewrite stores that corrected string back with the bumped `retries`. `generation.export_kwh` and `generation.delta_export_kwh` are the larger of the GENERATION channel's `eImp_Ws` and `eExp_Ws` (import on a tie) and that register's increase. The key names are historical and are not raw `eExp_Ws`. `net.import_kwh` and `net.export_kwh` remain channel 3's import and export counters. A record already stored in NVS was snapshotted from `eExp_Ws` and still publishes that residual when it is retried.

### ACK (`mqtt_ack_topic`, default `neurio/hourly/ack`)

A decimal id, or `{"id": 1730980800}`.

---

## Build

ESPHome 2026.10 or newer:

```bash
esphome compile examples/rf433sniffer-esp-idf.yaml
esphome compile examples/esp32-arduino.yaml
esphome compile examples/minimal.yaml
```

`examples/rf433sniffer-esp-idf.yaml` matches the rf433sniffer device shape: `esp32dev`, esp-idf, 80 MHz, `bluetooth_proxy` active, `api`, `web_server`, and the feed switch. Replace the placeholder Wi-Fi and MQTT password. Do not commit real secrets.

Arduino still compiles (`examples/esp32-arduino.yaml`) using the same `esp_http_client` and NVS code. `bluetooth_proxy` on ESP32 is an esp-idf feature, so that example does not enable it.

---

## Configuration

| Key | Default | Role |
| --- | --- | --- |
| `neurio_host` | required | Neurio IP or hostname |
| `neurio_port` | 80 | |
| `poll_interval` | 2 s | Alias of `update_interval`. Try 5 s with BLE proxy |
| `mqtt_openevse_topic` | `openevse/grid/power` | Divert feed |
| `mqtt_openevse_vrms_topic` | `emon/emonpi/vrms` | Line voltage for OpenEVSE; `""` disables |
| `mqtt_hourly_topic` | `neurio/hourly/energy` | Hourly energy records |
| `mqtt_ack_topic` | `neurio/hourly/ack` | Hourly record ACKs |
| `openevse_when_gated` | `import_sentinel` | or `silent` |
| `openevse_gated_power` | 50000 | Watts published while gated |
| `require_hourly_ack` | true | |

---

## Tests

```bash
python tests/run_tests.py
```

Host C++ tests cover the energy math, the GENERATION production-register choice, offset, ACK parser, the 336-record queue, Neurio JSON parser, hourly JSON, UTC timestamps (a non-UTC zone and both DST transitions), the vrms payload, and the feed-gate decision. They do not flash a device.
---

## Cutover notes

* Disable the Home Assistant automation that also publishes `emon/emonpi/power1` before the ESP starts, or the two writers will fight.
* Same warning for `emon/emonpi/vrms`: if a real emonPi (or anything else) already publishes it, disable one of them — or set `mqtt_openevse_vrms_topic: ""` — before the ESP starts.
* Seed **EVSE Grid Power Offset** to the value the old helper used (often −500). It is restored from flash after the first set.
* Turn on an automation for **OpenEVSE feed enabled** before or immediately after flashing if `restore_mode` is `RESTORE_DEFAULT_ON`. Until that switch is off, the device publishes real net+offset, including at night.
* Only one HTTP client should hit Neurio. Remove any Home Assistant REST poll of the Neurio; Home Assistant gets the values from this device's ESPHome API entities.
* Breaking change: the `neurio/raw` and `neurio/queue/status` MQTT topics and the `mqtt_raw_topic` / `mqtt_queue_status_topic` options are gone. Consumers of the raw feed (notably neurio-recorder's MQTT mode, which used it for live samples) keep the hourly records over MQTT but need another arrangement for live samples — do not simply run a second poller against the Neurio alongside this bridge.
* Hourly ACKs are optional. neurio-recorder ACKs them after durable storage; turn **Require Hourly ACK** off if nothing is ACKing and you do not want the NVS queue to fill.
