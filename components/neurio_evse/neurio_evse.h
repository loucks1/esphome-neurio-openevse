#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/number/number.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/mqtt/mqtt_client.h"

#include <cstdint>
#include <string>
#include <vector>

#include "neurio_logic.h"
#include "outage_queue.h"

namespace esphome {
namespace neurio_evse {

class NeurioEvseComponent;

class NeurioEvseNumber : public number::Number, public Component {
 public:
  void set_parent(NeurioEvseComponent *parent) { this->parent_ = parent; }
  void setup() override;

 protected:
  void control(float value) override;
  NeurioEvseComponent *parent_{nullptr};
  ESPPreferenceObject pref_{};
};

class NeurioEvseSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(NeurioEvseComponent *parent) { this->parent_ = parent; }
  void setup() override;

 protected:
  void write_state(bool state) override;
  NeurioEvseComponent *parent_{nullptr};
};

// Home Assistant switch. restore_mode is applied by Switch::get_initial_state_with_restore_mode.
class NeurioEvseFeedSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(NeurioEvseComponent *parent) { this->parent_ = parent; }
  void setup() override;

 protected:
  void write_state(bool state) override;
  NeurioEvseComponent *parent_{nullptr};
};

// Unscoped so ESPHome codegen can emit neurio_evse::IN_MEMORY_FIRST.
enum PersistenceMode {
  IN_MEMORY_FIRST = 0,
  WRITE_FIRST = 1,
};

struct InFlightRecord {
  HourlyRecord record{};
  uint32_t enqueued_ms{0};
  uint32_t last_flush_attempt_ms{0};
  bool in_flash{false};
};

class NeurioEvseComponent : public PollingComponent {
 public:
  NeurioEvseComponent() = default;

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;

  void set_neurio_host(const std::string &host) { this->neurio_host_ = host; }
  void set_neurio_port(uint16_t port) { this->neurio_port_ = port; }
  void set_mqtt_openevse_topic(const std::string &topic) { this->mqtt_openevse_topic_ = topic; }
  void set_mqtt_openevse_vrms_topic(const std::string &topic) { this->mqtt_openevse_vrms_topic_ = topic; }
  void set_mqtt_hourly_topic(const std::string &topic) { this->mqtt_hourly_topic_ = topic; }
  void set_mqtt_ack_topic(const std::string &topic) { this->mqtt_ack_topic_ = topic; }
  void set_retry_interval_ms(uint32_t ms) { this->retry_interval_ms_ = ms; }
  void set_require_hourly_ack(bool req) { this->require_hourly_ack_ = req; }
  bool get_require_hourly_ack() const { return this->require_hourly_ack_; }
  void set_persistence_mode(PersistenceMode mode) { this->persistence_mode_ = mode; }
  void set_ack_timeout_ms(uint32_t ms) { this->ack_timeout_ms_ = ms; }
  void set_backoff_interval_ms(uint32_t ms) { this->backoff_interval_ms_ = ms; }
  void set_socket_recycle_count(uint32_t count) { this->socket_recycle_count_ = count; }
  void set_time_id(time::RealTimeClock *time_id) { this->time_id_ = time_id; }
  void set_gated_mode(GatedPublishMode mode) { this->gated_mode_ = mode; }
  void set_gated_power(int32_t watts) { this->gated_power_w_ = watts; }

  void set_offset_watts(float watts) { this->offset_watts_ = watts; }
  float get_offset_watts() const { return this->offset_watts_; }

  void set_require_ack_switch(NeurioEvseSwitch *sw) { this->require_ack_switch_ = sw; }
  void set_feed_switch(NeurioEvseFeedSwitch *sw) {
    this->feed_switch_ = sw;
    this->feed_switch_configured_ = true;
  }
  void set_feed_switch_on(bool on) { this->feed_switch_on_ = on; }

  void set_net_power_sensor(sensor::Sensor *s) { this->net_power_sensor_ = s; }
  void set_generation_power_sensor(sensor::Sensor *s) { this->generation_power_sensor_ = s; }
  void set_consumption_power_sensor(sensor::Sensor *s) { this->consumption_power_sensor_ = s; }
  void set_phase_a_power_sensor(sensor::Sensor *s) { this->phase_a_power_sensor_ = s; }
  void set_phase_b_power_sensor(sensor::Sensor *s) { this->phase_b_power_sensor_ = s; }
  void set_reported_evse_power_sensor(sensor::Sensor *s) { this->reported_evse_power_sensor_ = s; }
  void set_phase_a_voltage_sensor(sensor::Sensor *s) { this->phase_a_voltage_sensor_ = s; }
  void set_phase_b_voltage_sensor(sensor::Sensor *s) { this->phase_b_voltage_sensor_ = s; }
  void set_queue_pending_sensor(sensor::Sensor *s) { this->queue_pending_sensor_ = s; }

  void set_net_import_energy_sensor(sensor::Sensor *s) { this->net_import_energy_sensor_ = s; }
  void set_net_export_energy_sensor(sensor::Sensor *s) { this->net_export_energy_sensor_ = s; }
  void set_generation_energy_sensor(sensor::Sensor *s) { this->generation_energy_sensor_ = s; }
  void set_consumption_energy_sensor(sensor::Sensor *s) { this->consumption_energy_sensor_ = s; }

  void handle_mqtt_ack(const std::string &payload);
  // Called from the MQTT task. Copies into a fixed ring; loop() drains it.
  void enqueue_mqtt_ack(const char *payload, size_t len);

 protected:
  enum class PollResult { SKIPPED, FAILED, OK };

  bool init_http_();
  void close_http_();
  PollResult poll_neurio();
  void publish_openevse_(bool fresh_sample);
  FeedDecision current_feed_decision_() const;
  void trigger_hourly_snapshot(ESPTime utc);
  void check_ack_timeouts();
  bool save_hourly_record(const HourlyRecord &rec);
  void retry_pending_queue();
  void update_queue_pending_sensor();
  void load_pending_queue();
  void save_baseline();
  void load_baseline();
  void drain_acks_();
  bool nvs_write_record_(const HourlyRecord &rec);
  bool nvs_read_record_(uint32_t id, HourlyRecord *rec);
  void nvs_erase_record_(uint32_t id);
  void publish_bytes_(const char *topic, const char *payload, size_t len, uint8_t qos);
  void publish_watts_(int32_t watts);

  std::string neurio_host_{};
  uint16_t neurio_port_{80};
  char url_[160]{};
  std::string mqtt_openevse_topic_{"openevse/grid/power"};
  std::string mqtt_openevse_vrms_topic_{"emon/emonpi/vrms"};
  std::string mqtt_hourly_topic_{"neurio/hourly/energy"};
  std::string mqtt_ack_topic_{"neurio/hourly/ack"};
  uint32_t retry_interval_ms_{300000};
  uint32_t ack_timeout_ms_{30000};
  bool require_hourly_ack_{true};
  PersistenceMode persistence_mode_{PersistenceMode::IN_MEMORY_FIRST};

  uint32_t backoff_interval_ms_{10000};
  uint32_t socket_recycle_count_{60};
  uint32_t consecutive_failures_{0};
  static constexpr uint32_t MAX_FAILURES_BEFORE_BACKOFF{3};
  uint32_t last_poll_attempt_ms_{0};
  uint32_t successful_requests_count_{0};

  time::RealTimeClock *time_id_{nullptr};
  NeurioEvseSwitch *require_ack_switch_{nullptr};
  NeurioEvseFeedSwitch *feed_switch_{nullptr};
  float offset_watts_{0.0f};

  bool feed_switch_configured_{false};
  bool feed_switch_on_{true};
  GatedPublishMode gated_mode_{GatedPublishMode::IMPORT_SENTINEL};
  int32_t gated_power_w_{50000};
  FeedPublish last_feed_publish_{FeedPublish::LIVE};
  const char *last_feed_reason_{""};
  bool feed_decision_valid_{false};

  sensor::Sensor *net_power_sensor_{nullptr};
  sensor::Sensor *generation_power_sensor_{nullptr};
  sensor::Sensor *consumption_power_sensor_{nullptr};
  sensor::Sensor *phase_a_power_sensor_{nullptr};
  sensor::Sensor *phase_b_power_sensor_{nullptr};
  sensor::Sensor *reported_evse_power_sensor_{nullptr};
  sensor::Sensor *phase_a_voltage_sensor_{nullptr};
  sensor::Sensor *phase_b_voltage_sensor_{nullptr};
  sensor::Sensor *queue_pending_sensor_{nullptr};
  sensor::Sensor *net_import_energy_sensor_{nullptr};
  sensor::Sensor *net_export_energy_sensor_{nullptr};
  sensor::Sensor *generation_energy_sensor_{nullptr};
  sensor::Sensor *consumption_energy_sensor_{nullptr};

  // esp_http_client_handle_t, owned for the life of the process. Typed as void
  // so this header does not include esp_http_client.h.
  void *http_{nullptr};
  bool http_ready_{false};

  ParsedChannel channels_[6]{};
  struct Baseline {
    uint64_t e_imp_ws{0};
    uint64_t e_exp_ws{0};
    bool valid{false};
  };
  Baseline baselines_[6]{};
  bool baselines_initialized_{false};
  bool have_fresh_net_{false};
  int32_t fresh_reported_w_{0};

  // UTC hour id of the last accepted sample. 0 until the clock is valid.
  // Advanced only by note_utc_sample(); not a local hour-of-day.
  uint32_t last_utc_hour_id_{0};
  uint32_t last_retry_time_{0};
  OutageQueue outage_queue_{120};
  std::vector<InFlightRecord> in_flight_records_;
  bool nvs_ok_{false};

  static constexpr int kAckSlots = 4;
  static constexpr size_t kAckBytes = 48;
  char ack_q_[kAckSlots][kAckBytes]{};
  volatile uint8_t ack_w_{0};
  volatile uint8_t ack_r_{0};
};

}  // namespace neurio_evse
}  // namespace esphome
