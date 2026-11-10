#include "neurio_evse.h"

#include "esphome/core/log.h"

#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace esphome {
namespace neurio_evse {

static const char *const TAG = "neurio_evse";
static constexpr char kNvsNamespace[] = "neurio";
static constexpr char kBaselineKey[] = "baseline";
static constexpr uint32_t kBaselineMagic = 0x4E523042u;  // 'NR0B'
static constexpr uint32_t kFlushRetryMs = 60000;

// One response buffer for the life of the process. A second neurio_evse
// instance is rejected in setup(). 3071 bytes of body + NUL.
static char s_body[kHttpBodyCap];
static size_t s_body_len = 0;
static bool s_body_overflow = false;
static NeurioEvseComponent *s_owner = nullptr;

struct BaselineBlob {
  uint32_t magic;
  uint8_t valid[5];
  uint8_t reserved[7];
  uint64_t e_imp_ws[5];
  uint64_t e_exp_ws[5];
};
static_assert(sizeof(BaselineBlob) == 96, "BaselineBlob layout changed");

esp_err_t http_event(esp_http_client_event_t *evt) {
  if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data == nullptr || evt->data_len <= 0) {
    return ESP_OK;
  }
  size_t room = (kHttpBodyCap - 1) - s_body_len;
  size_t n = static_cast<size_t>(evt->data_len);
  if (n > room) {
    s_body_overflow = true;
    n = room;
  }
  if (n > 0) {
    std::memcpy(s_body + s_body_len, evt->data, n);
    s_body_len += n;
  }
  return ESP_OK;
}

void NeurioEvseNumber::setup() {
  this->pref_ = this->make_entity_preference<float>();
  float value = 0.0f;
  if (!this->pref_.load(&value)) {
    value = 0.0f;
  }
  this->publish_state(value);
  if (this->parent_ != nullptr) {
    this->parent_->set_offset_watts(value);
  }
}

void NeurioEvseNumber::control(float value) {
  this->publish_state(value);
  this->pref_.save(&value);
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

void NeurioEvseFeedSwitch::setup() {
  optional<bool> initial = this->get_initial_state_with_restore_mode();
  bool state = initial.value_or(true);
  ESP_LOGI(TAG, "OpenEVSE feed switch restored %s", state ? "ON" : "OFF");
  this->control(state);
}

void NeurioEvseFeedSwitch::write_state(bool state) {
  this->publish_state(state);
  if (this->parent_ != nullptr) {
    this->parent_->set_feed_switch_on(state);
    ESP_LOGI(TAG, "OpenEVSE feed switch %s", state ? "ON" : "OFF");
  }
}

void NeurioEvseComponent::enqueue_mqtt_ack(const char *payload, size_t len) {
  uint8_t w = this->ack_w_;
  uint8_t next = static_cast<uint8_t>((w + 1) % kAckSlots);
  if (next == this->ack_r_) {
    ESP_LOGW(TAG, "ACK ring full; dropping payload");
    return;
  }
  if (len >= kAckBytes) {
    len = kAckBytes - 1;
  }
  if (payload != nullptr && len > 0) {
    std::memcpy(this->ack_q_[w], payload, len);
  }
  this->ack_q_[w][len] = '\0';
  __atomic_store_n(&this->ack_w_, next, __ATOMIC_RELEASE);
}

void NeurioEvseComponent::drain_acks_() {
  uint8_t r = this->ack_r_;
  uint8_t w = __atomic_load_n(&this->ack_w_, __ATOMIC_ACQUIRE);
  while (r != w) {
    this->handle_mqtt_ack(this->ack_q_[r]);
    r = static_cast<uint8_t>((r + 1) % kAckSlots);
    __atomic_store_n(&this->ack_r_, r, __ATOMIC_RELEASE);
  }
}

bool NeurioEvseComponent::init_http_() {
  if (this->url_[0] == '\0') {
    return false;
  }
  esp_http_client_config_t cfg = {};
  cfg.url = this->url_;
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = 800;
  cfg.event_handler = http_event;
  cfg.user_data = this;
  // TCP keep-alive. Neurio is HTTP/1.0 and closes after every response.
  cfg.keep_alive_enable = false;
  cfg.keep_alive_idle = 5;
  cfg.keep_alive_interval = 5;
  cfg.keep_alive_count = 3;
  // Allocated once inside esp_http_client_init, reused on every poll.
  cfg.buffer_size = 1024;
  cfg.buffer_size_tx = 256;
  cfg.disable_auto_redirect = true;
  this->http_ = esp_http_client_init(&cfg);
  this->http_ready_ = this->http_ != nullptr;
  return this->http_ready_;
}

void NeurioEvseComponent::close_http_() {
  if (this->http_ != nullptr) {
    esp_http_client_close(static_cast<esp_http_client_handle_t>(this->http_));
  }
}

void NeurioEvseComponent::setup() {
  if (s_owner != nullptr && s_owner != this) {
    ESP_LOGE(TAG, "Only one neurio_evse instance is supported");
    this->mark_failed();
    return;
  }
  s_owner = this;

  ESP_LOGI(TAG, "Initializing Neurio EVSE component (esp_http_client + NVS)...");

  int url_n = std::snprintf(this->url_, sizeof(this->url_), "http://%s:%u/current-sample", this->neurio_host_.c_str(),
                            static_cast<unsigned>(this->neurio_port_));
  if (url_n <= 0 || static_cast<size_t>(url_n) >= sizeof(this->url_)) {
    this->url_[0] = '\0';
    ESP_LOGE(TAG, "Neurio URL does not fit in %u bytes", static_cast<unsigned>(sizeof(this->url_)));
    this->mark_failed();
    return;
  }

  nvs_handle_t probe;
  esp_err_t nvs_err = nvs_open(kNvsNamespace, NVS_READWRITE, &probe);
  if (nvs_err == ESP_OK) {
    nvs_close(probe);
    this->nvs_ok_ = true;
    this->load_pending_queue();
    this->load_baseline();
    ESP_LOGI(TAG, "NVS namespace '%s' ready (hourly queue + baseline).", kNvsNamespace);
  } else {
    this->nvs_ok_ = false;
    ESP_LOGE(TAG, "NVS open failed (%s). Store-and-forward will stay in RAM only.", esp_err_to_name(nvs_err));
  }

  if (mqtt::global_mqtt_client != nullptr) {
    mqtt::global_mqtt_client->subscribe(
        this->mqtt_ack_topic_,
        [this](const std::string &topic, const std::string &payload) {
          (void) topic;
          this->enqueue_mqtt_ack(payload.data(), payload.size());
        },
        1);
  } else {
    ESP_LOGW(TAG, "Global MQTT client is null; ACK subscription deferred.");
  }

  if (!this->init_http_()) {
    ESP_LOGE(TAG, "esp_http_client_init failed");
    this->mark_failed();
    return;
  }

  this->last_retry_time_ = millis();
  this->update_queue_pending_sensor();

  ESP_LOGI(TAG, "HTTP client ready. Free heap %u bytes (internal %u). Response buffer %u bytes, reused.",
           static_cast<unsigned>(esp_get_free_heap_size()),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
           static_cast<unsigned>(kHttpBodyCap));
}

void NeurioEvseComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Neurio EVSE Component:");
  ESP_LOGCONFIG(TAG, "  Neurio URL: %s", this->url_);
  ESP_LOGCONFIG(TAG, "  Poll interval: %u ms", static_cast<unsigned>(this->get_update_interval()));
  ESP_LOGCONFIG(TAG, "  OpenEVSE Topic: %s", this->mqtt_openevse_topic_.c_str());
  ESP_LOGCONFIG(TAG, "  OpenEVSE Vrms Topic: %s",
                this->mqtt_openevse_vrms_topic_.empty() ? "(disabled)" : this->mqtt_openevse_vrms_topic_.c_str());
  ESP_LOGCONFIG(TAG, "  Hourly Topic: %s", this->mqtt_hourly_topic_.c_str());
  ESP_LOGCONFIG(TAG, "  ACK Topic: %s", this->mqtt_ack_topic_.c_str());
  ESP_LOGCONFIG(TAG, "  Retry Interval: %u ms", static_cast<unsigned>(this->retry_interval_ms_));
  ESP_LOGCONFIG(TAG, "  ACK Timeout: %u ms", static_cast<unsigned>(this->ack_timeout_ms_));
  ESP_LOGCONFIG(TAG, "  Adaptive Backoff Interval: %u ms", static_cast<unsigned>(this->backoff_interval_ms_));
  ESP_LOGCONFIG(TAG, "  Socket Recycle Count: %u requests", static_cast<unsigned>(this->socket_recycle_count_));
  ESP_LOGCONFIG(TAG, "  Persistence: NVS namespace %s (%s)", kNvsNamespace, this->nvs_ok_ ? "ok" : "unavailable");
  ESP_LOGCONFIG(TAG, "  Persistence Mode: %s",
                (this->persistence_mode_ == PersistenceMode::IN_MEMORY_FIRST) ? "in_memory_first" : "write_first");
  ESP_LOGCONFIG(TAG, "  Require Hourly ACK: %s", YESNO(this->require_hourly_ack_));
  ESP_LOGCONFIG(TAG, "  OpenEVSE feed switch: %s", this->feed_switch_configured_ ? "configured" : "absent (always live)");
  ESP_LOGCONFIG(TAG, "  When gated: %s",
                (this->gated_mode_ == GatedPublishMode::SILENT) ? "silent" : "import sentinel");
  if (this->gated_mode_ == GatedPublishMode::IMPORT_SENTINEL) {
    ESP_LOGCONFIG(TAG, "  Gated power: %d W", static_cast<int>(this->gated_power_w_));
  }
  if (this->sun_gate_enabled_) {
    ESP_LOGCONFIG(TAG, "  Local sun gate: elevation > %.2f deg", this->min_elevation_deg_);
  }
  ESP_LOGCONFIG(TAG, "  Persisted outage records: %u", static_cast<unsigned>(this->outage_queue_.size()));
  ESP_LOGCONFIG(TAG, "  Free heap: %u bytes", static_cast<unsigned>(esp_get_free_heap_size()));
}

void NeurioEvseComponent::loop() {
  this->drain_acks_();
  PollingComponent::loop();
}

void NeurioEvseComponent::update() {
  uint32_t now_ms = millis();
  this->have_fresh_net_ = false;

  PollResult result = PollResult::SKIPPED;
  if (this->consecutive_failures_ >= this->MAX_FAILURES_BEFORE_BACKOFF &&
      (now_ms - this->last_poll_attempt_ms_ < this->backoff_interval_ms_)) {
    result = PollResult::SKIPPED;
  } else {
    this->last_poll_attempt_ms_ = now_ms;
    result = this->poll_neurio();
  }

  // Sentinel publishes even when Neurio did not answer, so Eco mode cannot
  // sit on the last export reading. Live watts are published only for a fresh sample.
  this->publish_openevse_(result == PollResult::OK);

  this->check_ack_timeouts();

  if (this->require_hourly_ack_) {
    if (!this->outage_queue_.empty() && (now_ms - this->last_retry_time_ >= this->retry_interval_ms_)) {
      this->last_retry_time_ = now_ms;
      this->retry_pending_queue();
    }
  }
}

FeedDecision NeurioEvseComponent::current_feed_decision_() const {
  FeedGateInput in;
  in.switch_configured = this->feed_switch_configured_;
  in.switch_on = this->feed_switch_on_;
  in.sun_gate_enabled = this->sun_gate_enabled_;
  in.min_elevation_deg = this->min_elevation_deg_;
#ifdef USE_NEURIO_SUN_GATE
  if (this->sun_gate_enabled_ && this->sun_ != nullptr) {
    double elev = this->sun_->elevation();
    if (!std::isnan(elev)) {
      in.sun_elevation_valid = true;
      in.sun_elevation_deg = static_cast<float>(elev);
    }
  }
#endif
  return decide_feed(in, this->gated_mode_);
}

void NeurioEvseComponent::publish_bytes_(const char *topic, const char *payload, size_t len, uint8_t qos) {
  if (topic == nullptr || topic[0] == '\0' || payload == nullptr) {
    return;
  }
  if (mqtt::global_mqtt_client == nullptr || !mqtt::global_mqtt_client->is_connected()) {
    return;
  }
  mqtt::global_mqtt_client->publish(topic, payload, len, qos, false);
}

void NeurioEvseComponent::publish_watts_(int32_t watts) {
  char buf[16];
  int n = std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(watts));
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(buf)) {
    return;
  }
  this->publish_bytes_(this->mqtt_openevse_topic_.c_str(), buf, static_cast<size_t>(n), 0);
}

void NeurioEvseComponent::publish_openevse_(bool fresh_sample) {
  FeedDecision decision = this->current_feed_decision_();
  if (!this->feed_decision_valid_ || decision.publish != this->last_feed_publish_ ||
      std::strcmp(decision.reason, this->last_feed_reason_) != 0) {
    ESP_LOGI(TAG, "OpenEVSE feed %s (%s)",
             decision.publish == FeedPublish::LIVE        ? "LIVE"
             : decision.publish == FeedPublish::SENTINEL ? "SENTINEL"
                                                         : "SILENT",
             decision.reason);
    this->last_feed_publish_ = decision.publish;
    this->last_feed_reason_ = decision.reason;
    this->feed_decision_valid_ = true;
    if (decision.publish == FeedPublish::SILENT && this->reported_evse_power_sensor_ != nullptr) {
      this->reported_evse_power_sensor_->publish_state(NAN);
    }
  }

  if (decision.publish == FeedPublish::LIVE) {
    if (!fresh_sample || !this->have_fresh_net_) {
      return;
    }
    this->publish_watts_(this->fresh_reported_w_);
    if (this->reported_evse_power_sensor_ != nullptr) {
      this->reported_evse_power_sensor_->publish_state(this->fresh_reported_w_);
    }
    return;
  }

  if (decision.publish == FeedPublish::SENTINEL) {
    this->publish_watts_(this->gated_power_w_);
    if (this->reported_evse_power_sensor_ != nullptr) {
      this->reported_evse_power_sensor_->publish_state(this->gated_power_w_);
    }
  }
}

NeurioEvseComponent::PollResult NeurioEvseComponent::poll_neurio() {
  if (!this->http_ready_ || this->http_ == nullptr) {
    return PollResult::FAILED;
  }

  s_body_len = 0;
  s_body_overflow = false;
  s_body[0] = '\0';

  auto *client = static_cast<esp_http_client_handle_t>(this->http_);
  esp_http_client_set_url(client, this->url_);
  // esp_http_client logs "Incomplete chunked data received" at error level for
  // the Neurio's normal close-delimited HTTP/1.0 response (see below). That
  // outcome is expected on every poll, so keep the HTTP_CLIENT tag quiet for
  // the duration of perform() and restore its level afterwards. The component
  // reports the outcome itself: the close-delimited success at debug level,
  // genuine failures as warnings.
  const esp_log_level_t http_client_level = esp_log_level_get("HTTP_CLIENT");
  esp_log_level_set("HTTP_CLIENT", ESP_LOG_NONE);
  esp_err_t err = esp_http_client_perform(client);
  esp_log_level_set("HTTP_CLIENT", http_client_level);
  int status = esp_http_client_get_status_code(client);
  int64_t content_length = esp_http_client_get_content_length(client);

  // Neurio answers HTTP/1.0 with no Content-Length and closes the socket.
  // esp_http_client treats a missing length as chunked, and IDF >= 5.5.2 then
  // fails perform() ("Incomplete chunked data received") after ON_DATA has the
  // body. A Content-Length response that fails perform() is still a failure.
  // parse_neurio_sample() rejects a body whose top-level object did not close.
  if (err != ESP_OK && http_body_is_usable(false, status, s_body_len, s_body_overflow, content_length)) {
    ESP_LOGD(TAG, "Close-delimited body (%u bytes) after %s", static_cast<unsigned>(s_body_len), esp_err_to_name(err));
    err = ESP_OK;
  }

  if (err != ESP_OK || status != 200 || s_body_overflow || s_body_len == 0) {
    this->consecutive_failures_++;
    this->successful_requests_count_ = 0;
    this->close_http_();
    if (this->consecutive_failures_ == this->MAX_FAILURES_BEFORE_BACKOFF) {
      ESP_LOGW(TAG, "Neurio unresponsive (%u consecutive failures). Backing off to %u ms.",
               static_cast<unsigned>(this->consecutive_failures_), static_cast<unsigned>(this->backoff_interval_ms_));
    } else if (this->consecutive_failures_ < this->MAX_FAILURES_BEFORE_BACKOFF) {
      ESP_LOGW(TAG, "Neurio HTTP GET failed (status %d, %s, body %u, overflow %s)", status, esp_err_to_name(err),
               static_cast<unsigned>(s_body_len), s_body_overflow ? "yes" : "no");
    }
    return PollResult::FAILED;
  }

  s_body[s_body_len] = '\0';

  ParsedSample sample;
  if (!parse_neurio_sample(s_body, s_body_len, &sample) || !sample.ok) {
    this->consecutive_failures_++;
    this->successful_requests_count_ = 0;
    this->close_http_();
    ESP_LOGW(TAG, "Failed to parse Neurio JSON (%u bytes)", static_cast<unsigned>(s_body_len));
    return PollResult::FAILED;
  }

  if (this->consecutive_failures_ >= this->MAX_FAILURES_BEFORE_BACKOFF) {
    ESP_LOGI(TAG, "Neurio communication restored.");
  }
  this->consecutive_failures_ = 0;
  this->successful_requests_count_++;

  if ((this->successful_requests_count_ % 60u) == 0) {
    ESP_LOGD(TAG, "Free heap %u (internal %u)", static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
  }

  if (this->successful_requests_count_ >= this->socket_recycle_count_) {
    this->successful_requests_count_ = 0;
    this->close_http_();
    ESP_LOGD(TAG, "Recycled TCP socket after %u requests.", static_cast<unsigned>(this->socket_recycle_count_));
  }

  for (int ch = 1; ch <= 5; ch++) {
    if (!sample.channels[ch].valid) {
      continue;
    }
    this->channels_[ch] = sample.channels[ch];
  }

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

  if (sample.channels[3].valid) {
    int32_t net_p = sample.channels[3].p_w;
    int32_t reported_p = calculate_reported_power(net_p, this->offset_watts_);
    this->have_fresh_net_ = true;
    this->fresh_reported_w_ = reported_p;
    if (this->net_power_sensor_ != nullptr) {
      this->net_power_sensor_->publish_state(net_p);
    }
  }

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
  // Line-to-line voltage for OpenEVSE's divert math, which divides grid watts
  // by this voltage. Deliberately not passed through the feed gate: it is a
  // measurement, not the divert signal. An empty topic disables the publish.
  if (this->channels_[1].valid && this->channels_[2].valid && !this->mqtt_openevse_vrms_topic_.empty()) {
    char vrms[16];
    int vn = format_vrms_payload(vrms, sizeof(vrms), this->channels_[1].v_v, this->channels_[2].v_v);
    if (vn > 0) {
      this->publish_bytes_(this->mqtt_openevse_vrms_topic_.c_str(), vrms, static_cast<size_t>(vn), 0);
    }
  }
  if (this->channels_[4].valid && this->generation_power_sensor_ != nullptr) {
    this->generation_power_sensor_->publish_state(this->channels_[4].p_w);
  }
  if (this->channels_[5].valid && this->consumption_power_sensor_ != nullptr) {
    this->consumption_power_sensor_->publish_state(this->channels_[5].p_w);
  }

  if (this->channels_[3].valid && this->net_import_energy_sensor_ != nullptr) {
    this->net_import_energy_sensor_->publish_state(ws_to_kwh(this->channels_[3].e_imp_ws));
  }
  if (this->channels_[3].valid && this->net_export_energy_sensor_ != nullptr) {
    this->net_export_energy_sensor_->publish_state(ws_to_kwh(this->channels_[3].e_exp_ws));
  }
  if (this->channels_[4].valid && this->generation_energy_sensor_ != nullptr) {
    const uint64_t produced_ws =
        generation_production_ws(this->channels_[4].e_imp_ws, this->channels_[4].e_exp_ws);
    this->generation_energy_sensor_->publish_state(ws_to_kwh(produced_ws));
  }
  if (this->channels_[5].valid && this->consumption_energy_sensor_ != nullptr) {
    this->consumption_energy_sensor_->publish_state(ws_to_kwh(this->channels_[5].e_imp_ws));
  }

  if (this->time_id_ != nullptr) {
    // utcnow(), not now(): now().hour / now().strftime() are local wall time.
    // The record id and the Z timestamp both name the UTC hour.
    auto utc = this->time_id_->utcnow();
    if (utc.is_valid() && note_utc_sample(this->last_utc_hour_id_, utc.timestamp) != 0) {
      this->trigger_hourly_snapshot(utc);
    }
  }

  return PollResult::OK;
}

void NeurioEvseComponent::trigger_hourly_snapshot(ESPTime utc) {
  if (!this->baselines_initialized_ || !this->channels_[3].valid) {
    ESP_LOGW(TAG, "Cannot trigger hourly snapshot: channel data not ready.");
    return;
  }

  HourlyRecord rec{};
  rec.magic = kHourlyMagic;
  rec.version = kHourlyVersion;
  rec.retries = 0;
  rec.id = utc_hour_id(utc.timestamp);
  // Stored for the NVS blob. Publish rebuilds this from rec.id, so a bad string
  // cannot reach MQTT. gmtime, not ESPTime::strftime: strftime prints the
  // broken-down fields, which are local on now() and were labeled with Z.
  if (!format_utc_timestamp(rec.id, rec.timestamp, sizeof(rec.timestamp))) {
    ESP_LOGW(TAG, "Cannot trigger hourly snapshot: UTC timestamp for %u did not format.",
             static_cast<unsigned>(rec.id));
    return;
  }
  rec.net_imp_ws = this->channels_[3].e_imp_ws;
  rec.net_exp_ws = this->channels_[3].e_exp_ws;
  rec.base_net_imp_ws = this->baselines_[3].e_imp_ws;
  rec.base_net_exp_ws = this->baselines_[3].e_exp_ws;
  assign_hourly_generation(&rec, this->channels_[4].e_imp_ws, this->channels_[4].e_exp_ws,
                           this->baselines_[4].e_imp_ws, this->baselines_[4].e_exp_ws);
  rec.cons_imp_ws = this->channels_[5].e_imp_ws;
  rec.base_cons_imp_ws = this->baselines_[5].e_imp_ws;
  rec.phase_a_imp_ws = this->channels_[1].e_imp_ws;
  rec.base_phase_a_imp_ws = this->baselines_[1].e_imp_ws;
  rec.phase_b_imp_ws = this->channels_[2].e_imp_ws;
  rec.base_phase_b_imp_ws = this->baselines_[2].e_imp_ws;
  rec.phase_a_voltage = this->channels_[1].v_v;
  rec.phase_b_voltage = this->channels_[2].v_v;

  for (int i = 1; i <= 5; i++) {
    this->baselines_[i].e_imp_ws = this->channels_[i].e_imp_ws;
    this->baselines_[i].e_exp_ws = this->channels_[i].e_exp_ws;
  }
  this->save_baseline();

  bool mqtt_online = (mqtt::global_mqtt_client != nullptr && mqtt::global_mqtt_client->is_connected());

  if (this->require_hourly_ack_) {
    bool write_now = (this->persistence_mode_ == PersistenceMode::WRITE_FIRST || !mqtt_online);
    bool in_flash = false;
    if (write_now) {
      in_flash = this->save_hourly_record(rec);
      ESP_LOGI(TAG, "Hourly record %u %s.", static_cast<unsigned>(rec.id),
               in_flash ? "written to NVS" : "NVS write failed; held in RAM");
    } else {
      ESP_LOGI(TAG, "Hourly record %u stored in RAM (awaiting ACK within %u ms).", static_cast<unsigned>(rec.id),
               static_cast<unsigned>(this->ack_timeout_ms_));
    }
    this->in_flight_records_.push_back({rec, millis(), 0, in_flash});
  } else {
    ESP_LOGI(TAG, "Testing Mode: require_hourly_ack is disabled; record %u published without persistence.",
             static_cast<unsigned>(rec.id));
  }

  char json[768];
  int n = format_hourly_json(rec, json, sizeof(json));
  if (n < 0) {
    ESP_LOGE(TAG, "Hourly JSON did not fit in the render buffer");
  } else if (mqtt_online) {
    this->publish_bytes_(this->mqtt_hourly_topic_.c_str(), json, static_cast<size_t>(n), 1);
    ESP_LOGI(TAG, "Published hourly energy record %u to %s", static_cast<unsigned>(rec.id),
             this->mqtt_hourly_topic_.c_str());
  } else {
    ESP_LOGW(TAG, "MQTT not connected; record %u held in outage queue.", static_cast<unsigned>(rec.id));
  }

  this->update_queue_pending_sensor();
}

void NeurioEvseComponent::check_ack_timeouts() {
  if (this->in_flight_records_.empty() || !this->require_hourly_ack_) {
    return;
  }
  uint32_t now_ms = millis();
  for (auto &rec : this->in_flight_records_) {
    if (rec.in_flash) {
      continue;
    }
    if (now_ms - rec.enqueued_ms < this->ack_timeout_ms_) {
      continue;
    }
    if (rec.last_flush_attempt_ms != 0 && (now_ms - rec.last_flush_attempt_ms) < kFlushRetryMs) {
      continue;
    }
    rec.last_flush_attempt_ms = now_ms;
    ESP_LOGW(TAG, "ACK timeout for record %u. Flushing to NVS.", static_cast<unsigned>(rec.record.id));
    if (this->save_hourly_record(rec.record)) {
      rec.in_flash = true;
      this->update_queue_pending_sensor();
    } else {
      ESP_LOGE(TAG, "NVS flush failed for record %u; will retry in %u ms", static_cast<unsigned>(rec.record.id),
               static_cast<unsigned>(kFlushRetryMs));
    }
  }
}

bool NeurioEvseComponent::nvs_write_record_(const HourlyRecord &rec) {
  if (!this->nvs_ok_) {
    return false;
  }
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) {
    return false;
  }
  char key[16];
  std::snprintf(key, sizeof(key), "r%u", static_cast<unsigned>(rec.id));
  esp_err_t err = nvs_set_blob(h, key, &rec, sizeof(rec));
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS write %s failed: %s", key, esp_err_to_name(err));
    return false;
  }
  return true;
}

bool NeurioEvseComponent::nvs_read_record_(uint32_t id, HourlyRecord *rec) {
  if (!this->nvs_ok_ || rec == nullptr) {
    return false;
  }
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) {
    return false;
  }
  char key[16];
  std::snprintf(key, sizeof(key), "r%u", static_cast<unsigned>(id));
  size_t sz = sizeof(*rec);
  esp_err_t err = nvs_get_blob(h, key, rec, &sz);
  nvs_close(h);
  if (err != ESP_OK || sz != sizeof(*rec) || rec->magic != kHourlyMagic || rec->version != kHourlyVersion ||
      rec->id != id) {
    return false;
  }
  return true;
}

void NeurioEvseComponent::nvs_erase_record_(uint32_t id) {
  if (!this->nvs_ok_ || id == 0) {
    return;
  }
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) {
    return;
  }
  char key[16];
  std::snprintf(key, sizeof(key), "r%u", static_cast<unsigned>(id));
  esp_err_t err = nvs_erase_key(h, key);
  if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
    nvs_commit(h);
  }
  nvs_close(h);
}

bool NeurioEvseComponent::save_hourly_record(const HourlyRecord &rec) {
  if (!this->nvs_write_record_(rec)) {
    return false;
  }
  uint32_t evicted_id = this->outage_queue_.push_record(rec.id);
  if (evicted_id > 0) {
    this->nvs_erase_record_(evicted_id);
    ESP_LOGW(TAG, "Queue capacity reached (120 hrs). Dropped oldest record %u", static_cast<unsigned>(evicted_id));
  }
  ESP_LOGI(TAG, "Queued hourly record %u in NVS", static_cast<unsigned>(rec.id));
  return true;
}

void NeurioEvseComponent::handle_mqtt_ack(const std::string &payload) {
  uint32_t ack_id = parse_ack_id(payload);
  if (ack_id == 0) {
    ESP_LOGW(TAG, "Received unparseable ACK payload: %s", payload.c_str());
    return;
  }

  auto it_flight = std::find_if(this->in_flight_records_.begin(), this->in_flight_records_.end(),
                                [ack_id](const InFlightRecord &r) { return r.record.id == ack_id; });

  if (it_flight != this->in_flight_records_.end()) {
    if (it_flight->in_flash) {
      this->nvs_erase_record_(ack_id);
      this->outage_queue_.handle_ack(ack_id);
      ESP_LOGI(TAG, "ACK received for %u (deleted from NVS).", static_cast<unsigned>(ack_id));
    } else {
      ESP_LOGI(TAG, "ACK received for %u (cleared from RAM).", static_cast<unsigned>(ack_id));
    }
    this->in_flight_records_.erase(it_flight);
    this->update_queue_pending_sensor();
    return;
  }

  if (this->outage_queue_.handle_ack(ack_id)) {
    this->nvs_erase_record_(ack_id);
    ESP_LOGI(TAG, "ACK received for %u (deleted from NVS).", static_cast<unsigned>(ack_id));
    this->update_queue_pending_sensor();
  } else {
    ESP_LOGD(TAG, "ACK received for unknown/already processed ID %u", static_cast<unsigned>(ack_id));
  }
}

void NeurioEvseComponent::retry_pending_queue() {
  if (this->outage_queue_.empty()) {
    return;
  }

  ESP_LOGI(TAG, "Retrying %u unacknowledged records from NVS...", static_cast<unsigned>(this->outage_queue_.size()));
  this->update_queue_pending_sensor();

  if (mqtt::global_mqtt_client == nullptr || !mqtt::global_mqtt_client->is_connected()) {
    ESP_LOGW(TAG, "Cannot retry queue: MQTT client not connected.");
    return;
  }

  for (uint32_t id : this->outage_queue_.get_pending_ids()) {
    HourlyRecord rec;
    if (!this->nvs_read_record_(id, &rec)) {
      ESP_LOGW(TAG, "Pending id %u is missing from NVS; dropping it", static_cast<unsigned>(id));
      this->outage_queue_.handle_ack(id);
      continue;
    }
    if (rec.retries < 65535) {
      rec.retries++;
    }
    // The blob already has to be rewritten for retries. Refresh timestamp from
    // id so a record stored as local time with a Z suffix heals in NVS too.
    // Publish does not read that field; format_hourly_json rebuilds it.
    format_utc_timestamp(rec.id, rec.timestamp, sizeof(rec.timestamp));
    this->nvs_write_record_(rec);
    char json[768];
    int n = format_hourly_json(rec, json, sizeof(json));
    if (n < 0) {
      ESP_LOGE(TAG, "Retry JSON for %u did not fit", static_cast<unsigned>(id));
      continue;
    }
    this->publish_bytes_(this->mqtt_hourly_topic_.c_str(), json, static_cast<size_t>(n), 1);
    ESP_LOGI(TAG, "Re-published unacked record %u (attempt %u)", static_cast<unsigned>(id),
             static_cast<unsigned>(rec.retries));
  }
}

void NeurioEvseComponent::update_queue_pending_sensor() {
  size_t total_pending = this->outage_queue_.size();

  for (const auto &rec : this->in_flight_records_) {
    if (!rec.in_flash) {
      total_pending++;
    }
  }

  if (this->queue_pending_sensor_ != nullptr) {
    this->queue_pending_sensor_->publish_state(static_cast<float>(total_pending));
  }
}

void NeurioEvseComponent::load_pending_queue() {
  std::vector<uint32_t> loaded_ids;
  nvs_iterator_t it = nullptr;
  esp_err_t err = nvs_entry_find("nvs", kNvsNamespace, NVS_TYPE_BLOB, &it);
  while (err == ESP_OK && it != nullptr) {
    nvs_entry_info_t info;
    if (nvs_entry_info(it, &info) == ESP_OK && info.key[0] == 'r') {
      char *end = nullptr;
      unsigned long id = std::strtoul(info.key + 1, &end, 10);
      if (end != info.key + 1 && id > 0) {
        loaded_ids.push_back(static_cast<uint32_t>(id));
      }
    }
    err = nvs_entry_next(&it);
  }
  if (it != nullptr) {
    nvs_release_iterator(it);
  }

  std::sort(loaded_ids.begin(), loaded_ids.end());
  while (loaded_ids.size() > 120) {
    this->nvs_erase_record_(loaded_ids.front());
    loaded_ids.erase(loaded_ids.begin());
  }
  this->outage_queue_.load_ids(loaded_ids);
  ESP_LOGI(TAG, "Loaded %u pending records from NVS.", static_cast<unsigned>(this->outage_queue_.size()));
}

void NeurioEvseComponent::save_baseline() {
  if (!this->nvs_ok_) {
    return;
  }
  BaselineBlob blob{};
  blob.magic = kBaselineMagic;
  for (int i = 1; i <= 5; i++) {
    blob.valid[i - 1] = this->baselines_[i].valid ? 1 : 0;
    blob.e_imp_ws[i - 1] = this->baselines_[i].e_imp_ws;
    blob.e_exp_ws[i - 1] = this->baselines_[i].e_exp_ws;
  }
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) {
    return;
  }
  esp_err_t err = nvs_set_blob(h, kBaselineKey, &blob, sizeof(blob));
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to store baseline: %s", esp_err_to_name(err));
  }
}

void NeurioEvseComponent::load_baseline() {
  if (!this->nvs_ok_) {
    return;
  }
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) {
    return;
  }
  BaselineBlob blob{};
  size_t sz = sizeof(blob);
  esp_err_t err = nvs_get_blob(h, kBaselineKey, &blob, &sz);
  nvs_close(h);
  if (err != ESP_OK || sz != sizeof(blob) || blob.magic != kBaselineMagic) {
    return;
  }
  for (int i = 1; i <= 5; i++) {
    this->baselines_[i].valid = blob.valid[i - 1] != 0;
    this->baselines_[i].e_imp_ws = blob.e_imp_ws[i - 1];
    this->baselines_[i].e_exp_ws = blob.e_exp_ws[i - 1];
  }
  this->baselines_initialized_ = true;
  ESP_LOGI(TAG, "Hourly baselines restored from NVS.");
}

}  // namespace neurio_evse
}  // namespace esphome
