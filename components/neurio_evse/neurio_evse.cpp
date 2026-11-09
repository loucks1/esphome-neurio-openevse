#include "neurio_evse.h"
#include "esphome/core/log.h"
#include <algorithm>

namespace esphome {
namespace neurio_evse {

static const char *const TAG = "neurio_evse";

void NeurioEvseNumber::setup() {
  this->publish_state(0.0f);
}

void NeurioEvseNumber::control(float value) {
  this->publish_state(value);
  if (this->parent_ != nullptr) {
    this->parent_->set_offset_watts(value);
  }
}

void NeurioEvseSwitch::setup() {
  bool initial_state = (this->parent_ != nullptr) ? this->parent_->get_require_hourly_ack() : true;
  this->publish_state(initial_state);
}

void NeurioEvseSwitch::write_state(bool state) {
  this->publish_state(state);
  if (this->parent_ != nullptr) {
    this->parent_->set_require_hourly_ack(state);
    ESP_LOGI(TAG, "Require Hourly ACK toggled to: %s", state ? "ENABLED" : "DISABLED (Testing Mode)");
  }
}

void NeurioEvseComponent::setup() {
  ESP_LOGI(TAG, "Initializing Neurio EVSE component...");

  // Initialize LittleFS for outage queue persistence
  if (!LittleFS.begin(true)) {
    ESP_LOGE(TAG, "LittleFS mount failed! Flash store-and-forward will be unavailable.");
  } else {
    ESP_LOGI(TAG, "LittleFS mounted successfully.");
    if (!LittleFS.exists("/neurio_q")) {
      LittleFS.mkdir("/neurio_q");
    }
    this->load_pending_queue();
    this->load_baseline();
  }

  // Subscribe to MQTT ACK topic
  if (mqtt::global_mqtt_client != nullptr) {
    mqtt::global_mqtt_client->subscribe(
        this->mqtt_ack_topic_,
        [this](const std::string &topic, const std::string &payload) {
          this->handle_mqtt_ack(payload);
        },
        1);
  } else {
    ESP_LOGW(TAG, "Global MQTT client is null; ACK subscription deferred.");
  }

  // Configure HTTP Keep-Alive and strict timeout
  this->http_.setReuse(true);
  this->http_.setTimeout(800);

  this->last_retry_time_ = millis();
  this->publish_queue_status();
}

void NeurioEvseComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Neurio EVSE Component:");
  ESP_LOGCONFIG(TAG, "  Neurio Host: %s:%u", this->neurio_host_.c_str(), this->neurio_port_);
  ESP_LOGCONFIG(TAG, "  OpenEVSE Topic: %s", this->mqtt_openevse_topic_.c_str());
  ESP_LOGCONFIG(TAG, "  Hourly Topic: %s", this->mqtt_hourly_topic_.c_str());
  ESP_LOGCONFIG(TAG, "  ACK Topic: %s", this->mqtt_ack_topic_.c_str());
  ESP_LOGCONFIG(TAG, "  Queue Status Topic: %s", this->mqtt_queue_status_topic_.c_str());
  ESP_LOGCONFIG(TAG, "  Retry Interval: %u ms", this->retry_interval_ms_);
  ESP_LOGCONFIG(TAG, "  ACK Timeout (Flush Grace Period): %u ms", this->ack_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Adaptive Backoff Interval: %u ms", this->backoff_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Socket Recycle Count: %u requests", this->socket_recycle_count_);
  ESP_LOGCONFIG(TAG, "  Persistence Mode: %s", (this->persistence_mode_ == PersistenceMode::IN_MEMORY_FIRST) ? "in_memory_first" : "write_first");
  ESP_LOGCONFIG(TAG, "  Require Hourly ACK: %s", YESNO(this->require_hourly_ack_));
  ESP_LOGCONFIG(TAG, "  Persisted Outage Records in Flash: %u", this->outage_queue_.size());
}

void NeurioEvseComponent::update() {
  uint32_t now_ms = millis();

  // 1. Adaptive Backoff: If Neurio is unresponsive, avoid hammering its socket table
  if (this->consecutive_failures_ >= this->MAX_FAILURES_BEFORE_BACKOFF) {
    if (now_ms - this->last_poll_attempt_ms_ < this->backoff_interval_ms_) {
      // In backoff window: skip this tick to give Neurio time to sweep stale sockets
      return;
    }
  }

  this->last_poll_attempt_ms_ = now_ms;
  this->poll_neurio();

  // 2. Check for in-memory ACK timeouts (flushing to LittleFS if timeout exceeded)
  this->check_ack_timeouts();

  // 3. 5-minute retry check for unacknowledged hourly records (only if ACK is required)
  if (this->require_hourly_ack_) {
    if (!this->outage_queue_.empty() && (now_ms - this->last_retry_time_ >= this->retry_interval_ms_)) {
      this->last_retry_time_ = now_ms;
      this->retry_pending_queue();
    }
  }
}

void NeurioEvseComponent::check_ack_timeouts() {
  if (this->in_flight_records_.empty() || !this->require_hourly_ack_) {
    return;
  }

  uint32_t now_ms = millis();
  for (auto &rec : this->in_flight_records_) {
    if (!rec.in_flash && (now_ms - rec.enqueued_ms >= this->ack_timeout_ms_)) {
      ESP_LOGW(TAG, "ACK timeout expired for record %u (%u ms). Flushing to LittleFS flash.", rec.id, this->ack_timeout_ms_);
      this->save_hourly_record(rec.id, rec.json_str);
      rec.in_flash = true;
      this->publish_queue_status();
    }
  }
}

void NeurioEvseComponent::poll_neurio() {
  if (this->neurio_host_.empty()) {
    return;
  }

  std::string url = "http://" + this->neurio_host_ + ":" + std::to_string(this->neurio_port_) + "/current-sample";
  this->http_.begin(this->wifi_client_, url.c_str());

  int http_code = this->http_.GET();
  if (http_code != HTTP_CODE_OK) {
    this->consecutive_failures_++;
    this->successful_requests_count_ = 0;

    // Aggressive Teardown: Forcefully kill TCP connection so Neurio immediately closes socket
    this->http_.end();
    this->wifi_client_.stop();

    if (this->consecutive_failures_ == this->MAX_FAILURES_BEFORE_BACKOFF) {
      ESP_LOGW(TAG, "Neurio unresponsive (%d consecutive failures). Backing off to %u ms polling rate to prevent socket lockup.",
               this->consecutive_failures_, this->backoff_interval_ms_);
    } else if (this->consecutive_failures_ < this->MAX_FAILURES_BEFORE_BACKOFF) {
      ESP_LOGW(TAG, "Neurio HTTP GET failed (%d): %s", http_code, this->http_.errorToString(http_code).c_str());
    }
    return;
  }

  String payload = this->http_.getString();
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    this->consecutive_failures_++;
    this->successful_requests_count_ = 0;
    this->http_.end();
    this->wifi_client_.stop();
    ESP_LOGW(TAG, "Failed to parse Neurio JSON: %s", err.c_str());
    return;
  }

  // Successful sample received!
  if (this->consecutive_failures_ >= this->MAX_FAILURES_BEFORE_BACKOFF) {
    ESP_LOGI(TAG, "Neurio communication restored! Resuming normal 1s-2s polling rate.");
  }
  this->consecutive_failures_ = 0;
  this->successful_requests_count_++;

  // Socket Recycling: Cleanly close connection every N requests to keep Neurio socket table clean
  if (this->successful_requests_count_ >= this->socket_recycle_count_) {
    this->successful_requests_count_ = 0;
    this->http_.end();
    this->wifi_client_.stop();
    ESP_LOGD(TAG, "Recycled TCP socket (reached %u requests) to maintain clean connection state.", this->socket_recycle_count_);
  }

  JsonArray channels = doc["channels"];
  for (JsonObject ch_obj : channels) {
    int ch = ch_obj["ch"] | 0;
    if (ch >= 1 && ch <= 5) {
      this->channels_[ch].ch = ch;
      this->channels_[ch].type = ch_obj["type"].as<std::string>();
      this->channels_[ch].p_w = ch_obj.containsKey("p_W") ? ch_obj["p_W"].as<int32_t>() : ch_obj["P"].as<int32_t>();
      this->channels_[ch].e_imp_ws = ch_obj["eImp_Ws"].as<uint64_t>();
      this->channels_[ch].e_exp_ws = ch_obj["eExp_Ws"].as<uint64_t>();
      this->channels_[ch].q_var = ch_obj.containsKey("q_VAR") ? ch_obj["q_VAR"].as<int32_t>() : ch_obj["Q"].as<int32_t>();
      this->channels_[ch].v_v = ch_obj.containsKey("v_V") ? ch_obj["v_V"].as<float>() : ch_obj["V"].as<float>();
      this->channels_[ch].valid = true;
    }
  }

  // Initialize baselines on very first reading if not already loaded from flash
  if (!this->baselines_initialized_) {
    bool all_valid = true;
    for (int i = 1; i <= 5; i++) {
      if (!this->channels_[i].valid) {
        all_valid = false;
        break;
      }
    }
    if (all_valid) {
      for (int i = 1; i <= 5; i++) {
        this->baselines_[i].e_imp_ws = this->channels_[i].e_imp_ws;
        this->baselines_[i].e_exp_ws = this->channels_[i].e_exp_ws;
        this->baselines_[i].valid = true;
      }
      this->baselines_initialized_ = true;
      this->save_baseline();
      ESP_LOGI(TAG, "Baselines initialized from live Neurio sample.");
    }
  }

  // Real-time OpenEVSE dispatch using Channel 3 (Net)
  if (this->channels_[3].valid) {
    int32_t net_p = this->channels_[3].p_w;
    int32_t reported_p = calculate_reported_power(net_p, this->offset_watts_);

    if (this->net_power_sensor_ != nullptr) {
      this->net_power_sensor_->publish_state(net_p);
    }
    if (this->reported_evse_power_sensor_ != nullptr) {
      this->reported_evse_power_sensor_->publish_state(reported_p);
    }

    if (mqtt::global_mqtt_client != nullptr && mqtt::global_mqtt_client->is_connected()) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%d", reported_p);
      mqtt::global_mqtt_client->publish(this->mqtt_openevse_topic_, buf, 0, false);
    }
  }

  // Publish other sensor states
  if (this->channels_[1].valid && this->phase_a_power_sensor_ != nullptr) {
    this->phase_a_power_sensor_->publish_state(this->channels_[1].p_w);
  }
  if (this->channels_[1].valid && this->phase_a_voltage_sensor_ != nullptr) {
    this->phase_a_voltage_sensor_->publish_state(this->channels_[1].v_v);
  }
  if (this->channels_[2].valid && this->phase_b_power_sensor_ != nullptr) {
    this->phase_b_power_sensor_->publish_state(this->channels_[2].p_w);
  }
  if (this->channels_[2].valid && this->phase_b_voltage_sensor_ != nullptr) {
    this->phase_b_voltage_sensor_->publish_state(this->channels_[2].v_v);
  }
  if (this->channels_[4].valid && this->generation_power_sensor_ != nullptr) {
    this->generation_power_sensor_->publish_state(this->channels_[4].p_w);
  }
  if (this->channels_[5].valid && this->consumption_power_sensor_ != nullptr) {
    this->consumption_power_sensor_->publish_state(this->channels_[5].p_w);
  }

  // Check top of hour rollover
  if (this->time_id_ != nullptr) {
    auto now = this->time_id_->now();
    if (now.is_valid()) {
      if (this->last_hour_ != -1 && now.hour != this->last_hour_) {
        this->trigger_hourly_snapshot(now);
      }
      this->last_hour_ = now.hour;
    }
  }
}

void NeurioEvseComponent::trigger_hourly_snapshot(const time::ESPTime &now) {
  if (!this->baselines_initialized_ || !this->channels_[3].valid) {
    ESP_LOGW(TAG, "Cannot trigger hourly snapshot: channel data not ready.");
    return;
  }

  uint32_t hourly_epoch = (static_cast<uint32_t>(now.timestamp) / 3600) * 3600;
  char time_buf[32];
  now.strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:00:00Z");

  double net_delta_imp = calculate_delta_kwh(this->channels_[3].e_imp_ws, this->baselines_[3].e_imp_ws);
  double net_delta_exp = calculate_delta_kwh(this->channels_[3].e_exp_ws, this->baselines_[3].e_exp_ws);
  double net_delta = net_delta_imp - net_delta_exp;

  double gen_delta_exp = calculate_delta_kwh(this->channels_[4].e_exp_ws, this->baselines_[4].e_exp_ws);
  double cons_delta_imp = calculate_delta_kwh(this->channels_[5].e_imp_ws, this->baselines_[5].e_imp_ws);
  double p1_delta_imp = calculate_delta_kwh(this->channels_[1].e_imp_ws, this->baselines_[1].e_imp_ws);
  double p2_delta_imp = calculate_delta_kwh(this->channels_[2].e_imp_ws, this->baselines_[2].e_imp_ws);

  DynamicJsonDocument doc(1024);
  doc["id"] = hourly_epoch;
  doc["timestamp"] = time_buf;
  doc["retries"] = 0;

  JsonObject net_obj = doc.createNestedObject("net");
  net_obj["import_kwh"] = ws_to_kwh(this->channels_[3].e_imp_ws);
  net_obj["export_kwh"] = ws_to_kwh(this->channels_[3].e_exp_ws);
  net_obj["delta_import_kwh"] = net_delta_imp;
  net_obj["delta_export_kwh"] = net_delta_exp;
  net_obj["delta_net_kwh"] = net_delta;

  JsonObject gen_obj = doc.createNestedObject("generation");
  gen_obj["export_kwh"] = ws_to_kwh(this->channels_[4].e_exp_ws);
  gen_obj["delta_export_kwh"] = gen_delta_exp;

  JsonObject cons_obj = doc.createNestedObject("consumption");
  cons_obj["import_kwh"] = ws_to_kwh(this->channels_[5].e_imp_ws);
  cons_obj["delta_import_kwh"] = cons_delta_imp;

  JsonObject p1_obj = doc.createNestedObject("phase_a");
  p1_obj["delta_import_kwh"] = p1_delta_imp;
  p1_obj["voltage_v"] = this->channels_[1].v_v;

  JsonObject p2_obj = doc.createNestedObject("phase_b");
  p2_obj["delta_import_kwh"] = p2_delta_imp;
  p2_obj["voltage_v"] = this->channels_[2].v_v;

  std::string json_str;
  serializeJson(doc, json_str);

  // Update baselines to current counters and save to flash
  for (int i = 1; i <= 5; i++) {
    this->baselines_[i].e_imp_ws = this->channels_[i].e_imp_ws;
    this->baselines_[i].e_exp_ws = this->channels_[i].e_exp_ws;
  }
  this->save_baseline();

  // Persistence Handling
  bool mqtt_online = (mqtt::global_mqtt_client != nullptr && mqtt::global_mqtt_client->is_connected());

  if (this->require_hourly_ack_) {
    if (this->persistence_mode_ == PersistenceMode::WRITE_FIRST || !mqtt_online) {
      // Immediate flash persistence
      this->save_hourly_record(hourly_epoch, json_str);
      this->in_flight_records_.push_back({hourly_epoch, json_str, millis(), true});
      ESP_LOGI(TAG, "Hourly record %u written to LittleFS flash.", hourly_epoch);
    } else {
      // In-Memory First: Hold in RAM, only write if ACK times out
      this->in_flight_records_.push_back({hourly_epoch, json_str, millis(), false});
      ESP_LOGI(TAG, "Hourly record %u stored in RAM (awaiting ACK within %u ms before writing to flash).", hourly_epoch, this->ack_timeout_ms_);
    }
  } else {
    ESP_LOGI(TAG, "Testing Mode: require_hourly_ack is disabled; record %u published without persistence.", hourly_epoch);
  }

  // Publish via MQTT
  if (mqtt_online) {
    mqtt::global_mqtt_client->publish(this->mqtt_hourly_topic_, json_str, 1, false);
    ESP_LOGI(TAG, "Published hourly energy record %u to %s", hourly_epoch, this->mqtt_hourly_topic_.c_str());
  } else {
    ESP_LOGW(TAG, "MQTT not connected; record %u held in outage queue.", hourly_epoch);
  }

  this->publish_queue_status();
}

void NeurioEvseComponent::save_hourly_record(uint32_t id, const std::string &json_str) {
  uint32_t evicted_id = this->outage_queue_.push_record(id);
  if (evicted_id > 0) {
    LittleFS.remove(this->get_file_path(evicted_id).c_str());
    ESP_LOGW(TAG, "Queue capacity reached (120 hrs). Dropped oldest record %u", evicted_id);
  }

  std::string path = this->get_file_path(id);
  File f = LittleFS.open(path.c_str(), "w");
  if (f) {
    f.print(json_str.c_str());
    f.close();
    ESP_LOGI(TAG, "Queued hourly record %u to LittleFS (%s)", id, path.c_str());
  } else {
    ESP_LOGE(TAG, "Failed to write hourly record %u to LittleFS", id);
  }
}

void NeurioEvseComponent::handle_mqtt_ack(const std::string &payload) {
  uint32_t ack_id = parse_ack_id(payload);

  if (ack_id == 0) {
    ESP_LOGW(TAG, "Received unparseable ACK payload: %s", payload.c_str());
    return;
  }

  // Check in-flight records first (RAM / recent records)
  auto it_flight = std::find_if(
      this->in_flight_records_.begin(),
      this->in_flight_records_.end(),
      [ack_id](const InFlightRecord &r) { return r.id == ack_id; });

  if (it_flight != this->in_flight_records_.end()) {
    if (it_flight->in_flash) {
      std::string path = this->get_file_path(ack_id);
      LittleFS.remove(path.c_str());
      this->outage_queue_.handle_ack(ack_id);
      ESP_LOGI(TAG, "ACK received for %u (deleted from LittleFS flash).", ack_id);
    } else {
      ESP_LOGI(TAG, "ACK received for %u (cleared from RAM - flash was never written!).", ack_id);
    }
    this->in_flight_records_.erase(it_flight);
    this->publish_queue_status();
    return;
  }

  // If not in in_flight_records (e.g., loaded from flash on reboot)
  if (this->outage_queue_.handle_ack(ack_id)) {
    std::string path = this->get_file_path(ack_id);
    LittleFS.remove(path.c_str());
    ESP_LOGI(TAG, "ACK received for %u (deleted from LittleFS flash).", ack_id);
    this->publish_queue_status();
  } else {
    ESP_LOGD(TAG, "ACK received for unknown/already processed ID %u", ack_id);
  }
}

void NeurioEvseComponent::retry_pending_queue() {
  if (this->outage_queue_.empty()) {
    return;
  }

  ESP_LOGI(TAG, "Retrying %u unacknowledged records from flash...", this->outage_queue_.size());
  this->publish_queue_status();

  if (mqtt::global_mqtt_client == nullptr || !mqtt::global_mqtt_client->is_connected()) {
    ESP_LOGW(TAG, "Cannot retry queue: MQTT client not connected.");
    return;
  }

  for (uint32_t id : this->outage_queue_.get_pending_ids()) {
    std::string path = this->get_file_path(id);
    File f = LittleFS.open(path.c_str(), "r");
    if (f) {
      String content = f.readString();
      f.close();

      // Bump retry count
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, content);
      int retries = doc["retries"] | 0;
      doc["retries"] = retries + 1;
      std::string updated_json;
      serializeJson(doc, updated_json);

      // Re-save updated retry count
      File fw = LittleFS.open(path.c_str(), "w");
      if (fw) {
        fw.print(updated_json.c_str());
        fw.close();
      }

      mqtt::global_mqtt_client->publish(this->mqtt_hourly_topic_, updated_json, 1, false);
      ESP_LOGI(TAG, "Re-published unacked record %u (attempt %d)", id, retries + 1);
    }
  }
}

void NeurioEvseComponent::publish_queue_status() {
  // Count total unacknowledged records (both in flash and waiting in RAM)
  size_t total_pending = this->outage_queue_.size();
  uint32_t oldest_id = this->outage_queue_.oldest_id();

  for (const auto &rec : this->in_flight_records_) {
    if (!rec.in_flash) {
      total_pending++;
      if (oldest_id == 0 || rec.id < oldest_id) {
        oldest_id = rec.id;
      }
    }
  }

  if (this->queue_pending_sensor_ != nullptr) {
    this->queue_pending_sensor_->publish_state(total_pending);
  }

  if (mqtt::global_mqtt_client != nullptr && mqtt::global_mqtt_client->is_connected()) {
    StaticJsonDocument<128> doc;
    doc["pending_records"] = total_pending;
    if (total_pending > 0 && oldest_id > 0) {
      doc["oldest_id"] = oldest_id;
    }
    std::string json;
    serializeJson(doc, json);
    mqtt::global_mqtt_client->publish(this->mqtt_queue_status_topic_, json, 1, false);
  }
}

void NeurioEvseComponent::load_pending_queue() {
  std::vector<uint32_t> loaded_ids;
  File root = LittleFS.open("/neurio_q");
  if (!root || !root.isDirectory()) {
    return;
  }

  File file = root.openNextFile();
  while (file) {
    std::string fname = file.name();
    size_t pos = fname.find("rec_");
    if (pos != std::string::npos) {
      uint32_t id = strtoul(fname.c_str() + pos + 4, nullptr, 10);
      if (id > 0) {
        loaded_ids.push_back(id);
      }
    }
    file = root.openNextFile();
  }
  this->outage_queue_.load_ids(loaded_ids);
  ESP_LOGI(TAG, "Loaded %u pending unacknowledged records from LittleFS.", this->outage_queue_.size());
}

void NeurioEvseComponent::save_baseline() {
  File f = LittleFS.open("/neurio_q/baseline.json", "w");
  if (f) {
    DynamicJsonDocument doc(512);
    JsonArray arr = doc.createNestedArray("channels");
    for (int i = 1; i <= 5; i++) {
      JsonObject obj = arr.createNestedObject();
      obj["ch"] = i;
      obj["e_imp_ws"] = this->baselines_[i].e_imp_ws;
      obj["e_exp_ws"] = this->baselines_[i].e_exp_ws;
      obj["valid"] = this->baselines_[i].valid;
    }
    serializeJson(doc, f);
    f.close();
  }
}

void NeurioEvseComponent::load_baseline() {
  if (!LittleFS.exists("/neurio_q/baseline.json")) {
    return;
  }
  File f = LittleFS.open("/neurio_q/baseline.json", "r");
  if (f) {
    DynamicJsonDocument doc(512);
    DeserializationError err = deserializeJson(doc, f);
    if (!err) {
      JsonArray arr = doc["channels"];
      for (JsonObject obj : arr) {
        int ch = obj["ch"] | 0;
        if (ch >= 1 && ch <= 5) {
          this->baselines_[ch].e_imp_ws = obj["e_imp_ws"].as<uint64_t>();
          this->baselines_[ch].e_exp_ws = obj["e_exp_ws"].as<uint64_t>();
          this->baselines_[ch].valid = obj["valid"] | false;
        }
      }
      this->baselines_initialized_ = true;
      ESP_LOGI(TAG, "Hourly baselines restored from LittleFS.");
    }
    f.close();
  }
}

std::string NeurioEvseComponent::get_file_path(uint32_t id) {
  return "/neurio_q/rec_" + std::to_string(id) + ".json";
}

}  // namespace neurio_evse
}  // namespace esphome
