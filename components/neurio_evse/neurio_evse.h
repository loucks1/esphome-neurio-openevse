#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/number/number.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/mqtt/mqtt_client.h"

#include <WiFiClient.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <string>

#include "neurio_math.h"
#include "outage_queue.h"

namespace esphome {
namespace neurio_evse {

enum class PersistenceMode {
  IN_MEMORY_FIRST = 0,
  WRITE_FIRST = 1,
};

struct InFlightRecord {
  uint32_t id{0};
  std::string json_str{""};
  uint32_t enqueued_ms{0};
  bool in_flash{false};
};

class NeurioEvseComponent;

class NeurioEvseNumber : public number::Number, public Component {
 public:
  void set_parent(NeurioEvseComponent *parent) { this->parent_ = parent; }
  void setup() override;

 protected:
  void control(float value) override;
  NeurioEvseComponent *parent_{nullptr};
};

class NeurioEvseSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(NeurioEvseComponent *parent) { this->parent_ = parent; }
  void setup() override;

 protected:
  void write_state(bool state) override;
  NeurioEvseComponent *parent_{nullptr};
};

struct ChannelReading {
  int ch{0};
  std::string type{""};
  int32_t p_w{0};
  uint64_t e_imp_ws{0};
  uint64_t e_exp_ws{0};
  int32_t q_var{0};
  float v_v{0.0f};
  bool valid{false};
};

struct ChannelBaseline {
  uint64_t e_imp_ws{0};
  uint64_t e_exp_ws{0};
  bool valid{false};
};

class NeurioEvseComponent : public PollingComponent {
 public:
  NeurioEvseComponent() = default;

  void setup() override;
  void update() override;
  void dump_config() override;

  void set_neurio_host(const std::string &host) { this->neurio_host_ = host; }
  void set_neurio_port(uint16_t port) { this->neurio_port_ = port; }
  void set_mqtt_openevse_topic(const std::string &topic) { this->mqtt_openevse_topic_ = topic; }
  void set_mqtt_hourly_topic(const std::string &topic) { this->mqtt_hourly_topic_ = topic; }
  void set_mqtt_ack_topic(const std::string &topic) { this->mqtt_ack_topic_ = topic; }
  void set_mqtt_queue_status_topic(const std::string &topic) { this->mqtt_queue_status_topic_ = topic; }
  void set_retry_interval_ms(uint32_t ms) { this->retry_interval_ms_ = ms; }
  void set_require_hourly_ack(bool req) { this->require_hourly_ack_ = req; }
  bool get_require_hourly_ack() const { return this->require_hourly_ack_; }
  void set_persistence_mode(PersistenceMode mode) { this->persistence_mode_ = mode; }
  void set_ack_timeout_ms(uint32_t ms) { this->ack_timeout_ms_ = ms; }
  void set_backoff_interval_ms(uint32_t ms) { this->backoff_interval_ms_ = ms; }
  void set_socket_recycle_count(uint32_t count) { this->socket_recycle_count_ = count; }
  void set_time_id(time::RealTimeClock *time_id) { this->time_id_ = time_id; }

  void set_offset_number(NeurioEvseNumber *num) { this->offset_number_ = num; }
  void set_offset_watts(float watts) { this->offset_watts_ = watts; }
  float get_offset_watts() const { return this->offset_watts_; }

  void set_require_ack_switch(NeurioEvseSwitch *sw) { this->require_ack_switch_ = sw; }

  // Sensors
  void set_net_power_sensor(sensor::Sensor *s) { this->net_power_sensor_ = s; }
  void set_generation_power_sensor(sensor::Sensor *s) { this->generation_power_sensor_ = s; }
  void set_consumption_power_sensor(sensor::Sensor *s) { this->consumption_power_sensor_ = s; }
  void set_phase_a_power_sensor(sensor::Sensor *s) { this->phase_a_power_sensor_ = s; }
  void set_phase_b_power_sensor(sensor::Sensor *s) { this->phase_b_power_sensor_ = s; }
  void set_reported_evse_power_sensor(sensor::Sensor *s) { this->reported_evse_power_sensor_ = s; }
  void set_phase_a_voltage_sensor(sensor::Sensor *s) { this->phase_a_voltage_sensor_ = s; }
  void set_phase_b_voltage_sensor(sensor::Sensor *s) { this->phase_b_voltage_sensor_ = s; }
  void set_queue_pending_sensor(sensor::Sensor *s) { this->queue_pending_sensor_ = s; }

  void handle_mqtt_ack(const std::string &payload);

 protected:
  void poll_neurio();
  void trigger_hourly_snapshot(const time::ESPTime &now);
  void check_ack_timeouts();
  void save_hourly_record(uint32_t id, const std::string &json_str);
  void retry_pending_queue();
  void publish_queue_status();
  void load_pending_queue();
  void save_baseline();
  void load_baseline();
  std::string get_file_path(uint32_t id);

  std::string neurio_host_{""};
  uint16_t neurio_port_{80};
  std::string mqtt_openevse_topic_{"openevse/grid/power"};
  std::string mqtt_hourly_topic_{"neurio/hourly/energy"};
  std::string mqtt_ack_topic_{"neurio/hourly/ack"};
  std::string mqtt_queue_status_topic_{"neurio/queue/status"};
  uint32_t retry_interval_ms_{300000}; // 5 minutes
  uint32_t ack_timeout_ms_{30000};      // 30 seconds grace period
  bool require_hourly_ack_{true};
  PersistenceMode persistence_mode_{PersistenceMode::IN_MEMORY_FIRST};

  // Socket Hygiene & Backoff Parameters
  uint32_t backoff_interval_ms_{10000}; // 10 seconds backoff when failing
  uint32_t socket_recycle_count_{60};   // Cleanly close & reopen socket every 60 requests (~2 min)
  uint32_t consecutive_failures_{0};
  const uint32_t MAX_FAILURES_BEFORE_BACKOFF{3};
  uint32_t last_poll_attempt_ms_{0};
  uint32_t successful_requests_count_{0};

  time::RealTimeClock *time_id_{nullptr};
  NeurioEvseNumber *offset_number_{nullptr};
  NeurioEvseSwitch *require_ack_switch_{nullptr};
  float offset_watts_{0.0f};

  // Sensor pointers
  sensor::Sensor *net_power_sensor_{nullptr};
  sensor::Sensor *generation_power_sensor_{nullptr};
  sensor::Sensor *consumption_power_sensor_{nullptr};
  sensor::Sensor *phase_a_power_sensor_{nullptr};
  sensor::Sensor *phase_b_power_sensor_{nullptr};
  sensor::Sensor *reported_evse_power_sensor_{nullptr};
  sensor::Sensor *phase_a_voltage_sensor_{nullptr};
  sensor::Sensor *phase_b_voltage_sensor_{nullptr};
  sensor::Sensor *queue_pending_sensor_{nullptr};

  // Hardware polling client
  HTTPClient http_;
  WiFiClient wifi_client_;

  // Channel data indexed 1 to 5
  ChannelReading channels_[6];
  ChannelBaseline baselines_[6];
  bool baselines_initialized_{false};

  // State management
  int last_hour_{-1};
  uint32_t last_retry_time_{0};
  OutageQueue outage_queue_{120};
  std::vector<InFlightRecord> in_flight_records_;
};

}  // namespace neurio_evse
}  // namespace esphome
