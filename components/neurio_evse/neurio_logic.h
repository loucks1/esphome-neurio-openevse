#pragma once

// Host-testable Neurio parsing, hourly JSON, and the OpenEVSE feed gate.
// No ESPHome or ESP-IDF headers: tests/test_unit.cpp compiles this on the host.

#include "neurio_math.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>

namespace esphome {
namespace neurio_evse {

static constexpr uint32_t kHourlyMagic = 0x4E523031u;  // 'NR01'
static constexpr uint16_t kHourlyVersion = 1;
static constexpr size_t kHttpBodyCap = 3072;

// One hourly snapshot. Stored as a single NVS blob (see neurio_evse.cpp) and
// rendered to the MQTT JSON contract when publishing or retrying.
// timestamp is kept in the blob for compatibility. The JSON string is rebuilt
// from id at publish time, so a stored local-time value is not what MQTT sends.
struct HourlyRecord {
  uint32_t magic;
  uint32_t id;
  uint16_t version;
  uint16_t retries;
  char timestamp[24];
  uint32_t reserved;
  uint64_t net_imp_ws;
  uint64_t net_exp_ws;
  uint64_t base_net_imp_ws;
  uint64_t base_net_exp_ws;
  // GENERATION production counter and the baseline of that same register.
  // Chosen by generation_production_ws (the larger of eImp_Ws and eExp_Ws),
  // not raw eExp_Ws. The member names stay so the 144-byte NVS layout does not move.
  uint64_t gen_exp_ws;
  uint64_t base_gen_exp_ws;
  uint64_t cons_imp_ws;
  uint64_t base_cons_imp_ws;
  uint64_t phase_a_imp_ws;
  uint64_t base_phase_a_imp_ws;
  uint64_t phase_b_imp_ws;
  uint64_t base_phase_b_imp_ws;
  float phase_a_voltage;
  float phase_b_voltage;
};

static_assert(sizeof(HourlyRecord) == 144, "HourlyRecord NVS layout changed");
static_assert(offsetof(HourlyRecord, net_imp_ws) == 40, "HourlyRecord alignment changed");

struct ParsedChannel {
  int32_t p_w{0};
  uint64_t e_imp_ws{0};
  uint64_t e_exp_ws{0};
  float v_v{0.0f};
  bool valid{false};
};

struct ParsedSample {
  ParsedChannel channels[6];  // index 1..5 match Neurio channel numbers
  bool ok{false};
};

// Unscoped so ESPHome codegen can emit neurio_evse::IMPORT_SENTINEL.
enum GatedPublishMode : uint8_t {
  IMPORT_SENTINEL = 0,
  SILENT = 1,
};

enum class FeedPublish : uint8_t {
  LIVE = 0,
  SENTINEL = 1,
  SILENT = 2,
};

struct FeedGateInput {
  bool switch_configured{false};
  bool switch_on{true};
};

struct FeedDecision {
  FeedPublish publish{FeedPublish::LIVE};
  // Points at a string literal. Used for transition logs.
  const char *reason{"enabled"};
};

// Live when the optional HA feed switch is on (or absent). The switch is the
// only gate: Home Assistant owns gating policy (the README's sun-elevation
// automation example drives this switch).
inline FeedDecision decide_feed(const FeedGateInput &in, GatedPublishMode mode) {
  FeedPublish gated = (mode == GatedPublishMode::SILENT) ? FeedPublish::SILENT : FeedPublish::SENTINEL;
  if (in.switch_configured && !in.switch_on) {
    return {gated, "switch off"};
  }
  return {FeedPublish::LIVE, "enabled"};
}

inline const char *skip_ws(const char *s, const char *end) {
  while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) {
    s++;
  }
  return s;
}

// Returns a pointer to the first value byte after "key": or nullptr.
inline const char *find_key_value(const char *s, size_t len, const char *key) {
  const size_t klen = std::strlen(key);
  const char *end = s + len;
  if (klen == 0 || len < klen + 3) {
    return nullptr;
  }
  for (const char *p = s; p + klen + 2 < end; p++) {
    if (*p != '"') {
      continue;
    }
    if (std::memcmp(p + 1, key, klen) != 0 || p[1 + klen] != '"') {
      continue;
    }
    const char *v = skip_ws(p + 2 + klen, end);
    if (v < end && *v == ':') {
      return skip_ws(v + 1, end);
    }
  }
  return nullptr;
}

inline bool parse_i32(const char *s, const char *end, int32_t *out) {
  s = skip_ws(s, end);
  if (s >= end) {
    return false;
  }
  int sign = 1;
  if (*s == '-') {
    sign = -1;
    s++;
  } else if (*s == '+') {
    s++;
  }
  if (s >= end || *s < '0' || *s > '9') {
    return false;
  }
  int32_t v = 0;
  while (s < end && *s >= '0' && *s <= '9') {
    v = v * 10 + (*s - '0');
    s++;
  }
  *out = v * sign;
  return true;
}

inline bool parse_u64(const char *s, const char *end, uint64_t *out) {
  s = skip_ws(s, end);
  if (s >= end || *s < '0' || *s > '9') {
    return false;
  }
  uint64_t v = 0;
  while (s < end && *s >= '0' && *s <= '9') {
    v = v * 10u + static_cast<uint64_t>(*s - '0');
    s++;
  }
  *out = v;
  return true;
}

inline bool parse_f32(const char *s, const char *end, float *out) {
  s = skip_ws(s, end);
  if (s >= end) {
    return false;
  }
  char tmp[32];
  size_t n = 0;
  if (*s == '-' || *s == '+') {
    tmp[n++] = *s++;
  }
  bool any = false;
  while (s < end && n + 1 < sizeof(tmp) && ((*s >= '0' && *s <= '9') || *s == '.')) {
    tmp[n++] = *s++;
    any = true;
  }
  if (!any) {
    return false;
  }
  tmp[n] = '\0';
  char *stop = nullptr;
  float v = std::strtof(tmp, &stop);
  if (stop == tmp) {
    return false;
  }
  *out = v;
  return true;
}

inline void parse_channel_object(const char *obj, size_t len, ParsedSample *out) {
  const char *ch_v = find_key_value(obj, len, "ch");
  if (ch_v == nullptr) {
    return;
  }
  int32_t ch = 0;
  if (!parse_i32(ch_v, obj + len, &ch) || ch < 1 || ch > 5) {
    return;
  }
  ParsedChannel &dst = out->channels[ch];
  dst = ParsedChannel{};
  dst.valid = true;

  const char *p = find_key_value(obj, len, "p_W");
  if (p == nullptr) {
    p = find_key_value(obj, len, "P");
  }
  if (p != nullptr) {
    parse_i32(p, obj + len, &dst.p_w);
  }

  const char *imp = find_key_value(obj, len, "eImp_Ws");
  if (imp != nullptr) {
    parse_u64(imp, obj + len, &dst.e_imp_ws);
  }
  const char *exp = find_key_value(obj, len, "eExp_Ws");
  if (exp != nullptr) {
    parse_u64(exp, obj + len, &dst.e_exp_ws);
  }

  const char *v = find_key_value(obj, len, "v_V");
  if (v == nullptr) {
    v = find_key_value(obj, len, "V");
  }
  if (v != nullptr) {
    parse_f32(v, obj + len, &dst.v_v);
  }
}

// True when json is one complete top-level object and nothing but whitespace follows it.
// A body that ends inside a later field (Neurio's cts, for example) is not complete,
// even if the channels array already closed.
inline bool json_toplevel_object_closed(const char *json, size_t len) {
  if (json == nullptr || len == 0) {
    return false;
  }
  const char *end = json + len;
  const char *p = skip_ws(json, end);
  if (p >= end || *p != '{') {
    return false;
  }
  int depth = 0;
  bool in_str = false;
  bool escape = false;
  for (; p < end; p++) {
    char c = *p;
    if (in_str) {
      if (escape) {
        escape = false;
        continue;
      }
      if (c == '\\') {
        escape = true;
        continue;
      }
      if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"') {
      in_str = true;
      continue;
    }
    if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;
      if (depth == 0) {
        return skip_ws(p + 1, end) == end;
      }
      if (depth < 0) {
        return false;
      }
    }
  }
  return false;
}

// perform_ok is esp_http_client_perform() == ESP_OK. content_length is
// esp_http_client_get_content_length(); <= 0 means the response had no
// Content-Length (IDF then treats the body as chunked). A positive length
// that failed perform() stays a failure, so a short Content-Length read is
// unchanged. A missing length with status 200 and a non-empty, non-overflowed
// body is the HTTP/1.0 close-delimited case: IDF >= 5.5.2 returns an error
// from perform() on the closing FIN after HTTP_EVENT_ON_DATA already delivered
// the bytes.
inline bool http_body_is_usable(bool perform_ok, int status, size_t body_len, bool overflow, int64_t content_length) {
  if (status != 200 || overflow || body_len == 0) {
    return false;
  }
  if (perform_ok) {
    return true;
  }
  return content_length <= 0;
}

// Parses a Neurio /current-sample body. The top-level object must be closed.
// ok is true when that object contains a channels array.
// Individual channels are marked valid when their object contains ch in 1..5.
inline bool parse_neurio_sample(const char *json, size_t len, ParsedSample *out) {
  *out = ParsedSample{};
  if (json == nullptr || len == 0 || !json_toplevel_object_closed(json, len)) {
    return false;
  }
  const char *channels = find_key_value(json, len, "channels");
  if (channels == nullptr) {
    return false;
  }
  const char *end = json + len;
  const char *p = channels;
  while (p < end && *p != '[') {
    p++;
  }
  if (p >= end) {
    return false;
  }
  p++;
  while (p < end) {
    p = skip_ws(p, end);
    if (p >= end) {
      break;
    }
    if (*p == ']') {
      out->ok = true;
      return true;
    }
    if (*p != '{') {
      p++;
      continue;
    }
    const char *obj = p;
    int depth = 0;
    bool in_str = false;
    const char *q = p;
    for (; q < end; q++) {
      char c = *q;
      if (c == '"' && (q == obj || q[-1] != '\\')) {
        in_str = !in_str;
        continue;
      }
      if (in_str) {
        continue;
      }
      if (c == '{') {
        depth++;
      } else if (c == '}') {
        depth--;
        if (depth == 0) {
          q++;
          break;
        }
      }
    }
    if (depth != 0) {
      return false;
    }
    parse_channel_object(obj, static_cast<size_t>(q - obj), out);
    p = q;
  }
  return false;
}

// Lifetime production on the GENERATION channel.
//
// Which cumulative register holds solar depends on CT orientation. This Neurio
// accumulates production on eImp_Ws (~417.7e9 Ws, ~116000 kWh) while eExp_Ws
// sits at a residual (~1.45e8 Ws, ~40.39 kWh). Other installs do the reverse.
// The register that has actually accumulated production is the larger of the
// two. A tie uses import.
//
// Only the GENERATION channel is chosen this way. NET import and export,
// consumption, and the phase channels keep the registers they already use.
inline bool generation_production_on_import(uint64_t e_imp_ws, uint64_t e_exp_ws) {
  return e_imp_ws >= e_exp_ws;
}

inline uint64_t generation_production_ws(uint64_t e_imp_ws, uint64_t e_exp_ws) {
  return generation_production_on_import(e_imp_ws, e_exp_ws) ? e_imp_ws : e_exp_ws;
}

// Writes the current production register and that same register's baseline into
// the hourly record. The choice follows the current sample, so the delta is one
// register's increase and never import minus export. gen_exp_ws keeps its name;
// format_hourly_json publishes it as generation.export_kwh.
inline void assign_hourly_generation(HourlyRecord *rec, uint64_t e_imp_ws, uint64_t e_exp_ws, uint64_t base_imp_ws,
                                     uint64_t base_exp_ws) {
  if (generation_production_on_import(e_imp_ws, e_exp_ws)) {
    rec->gen_exp_ws = e_imp_ws;
    rec->base_gen_exp_ws = base_imp_ws;
  } else {
    rec->gen_exp_ws = e_exp_ws;
    rec->base_gen_exp_ws = base_exp_ws;
  }
}

// Hourly records are keyed by UTC, whatever timezone the ESPHome time component uses.
//
// id is the Unix epoch floored to 3600s. timestamp is that same instant as
// "YYYY-MM-DDTHH:MM:SSZ". ESPTime::now() / strftime() break the epoch into local
// wall time, so a format ending in a literal Z publishes local time labeled as
// UTC (15:00Z for id 1794258000 while America/Chicago is UTC-6). Formatting goes
// through gmtime instead, and the MQTT payload rebuilds the string from id so a
// record already stored with the bad local string still publishes UTC.
//
// Hour boundaries use this same UTC id, not the local hour-of-day. Local hour
// numbers repeat at a DST fall-back (Chicago 1:00 CDT and 1:00 CST) and would
// drop that real hour, and they also look unchanged 24 hours later. UTC hours
// stay 3600s apart through both DST transitions: fall-back emits both hours,
// spring-forward skips a local label (2:00) but not a UTC hour. The first valid
// sample does not emit. A later sample emits only when its hour id is greater,
// so a backward NTP step does not publish a second record for an hour already
// snapshotted. A forward jump of several hours still yields one snapshot, for
// the hour we land in, not a backfill of the gap.

// Unix time floored to the UTC hour. This is the hourly record id.
// Returns 0 for times that are not a positive uint32 epoch.
inline uint32_t utc_hour_id(int64_t unix_time) {
  if (unix_time <= 0 || unix_time > static_cast<int64_t>(UINT32_MAX)) {
    return 0;
  }
  const auto secs = static_cast<uint64_t>(unix_time);
  return static_cast<uint32_t>((secs / 3600u) * 3600u);
}

// True when hour_id is a later UTC hour than one already sampled.
// previous_hour_id == 0 means the clock has not been sampled yet.
inline bool utc_hour_boundary_crossed(uint32_t previous_hour_id, uint32_t hour_id) {
  return previous_hour_id != 0 && hour_id > previous_hour_id;
}

// Notes one clock sample. Returns the UTC hour id to snapshot, or 0 when no
// record is due. previous_hour_id advances only forward (and from the unset 0).
inline uint32_t note_utc_sample(uint32_t &previous_hour_id, int64_t unix_time) {
  const uint32_t hour_id = utc_hour_id(unix_time);
  if (hour_id == 0) {
    return 0;
  }
  const uint32_t due = utc_hour_boundary_crossed(previous_hour_id, hour_id) ? hour_id : 0;
  if (previous_hour_id == 0 || hour_id > previous_hour_id) {
    previous_hour_id = hour_id;
  }
  return due;
}

// Writes "YYYY-MM-DDTHH:MM:SSZ" (20 characters, no quotes). The process timezone
// is ignored. Returns false if buf is smaller than 21 bytes or conversion fails.
inline bool format_utc_timestamp(uint32_t epoch, char *buf, size_t cap) {
  if (buf == nullptr || cap < 21) {
    return false;
  }
  time_t unix_time = static_cast<time_t>(epoch);
  struct tm tm_utc {};
  if (::gmtime_r(&unix_time, &tm_utc) == nullptr) {
    buf[0] = '\0';
    return false;
  }
  int n = std::snprintf(buf, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ", tm_utc.tm_year + 1900, tm_utc.tm_mon + 1,
                        tm_utc.tm_mday, tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
  if (n != 20 || static_cast<size_t>(n) >= cap) {
    buf[0] = '\0';
    return false;
  }
  return true;
}

// Renders the OpenEVSE vrms payload: line-to-line voltage on a split-phase
// service is the sum of the two phase voltages (about 240 V). OpenEVSE
// divides grid watts by this voltage in its divert math. Returns the length,
// or -1 when either phase voltage is implausible (not measured yet) or the
// buffer is too small.
inline int format_vrms_payload(char *buf, size_t cap, float phase_a_v, float phase_b_v) {
  if (buf == nullptr || cap == 0 || phase_a_v <= 0.0f || phase_b_v <= 0.0f) {
    return -1;
  }
  int n = std::snprintf(buf, cap, "%.1f", static_cast<double>(phase_a_v + phase_b_v));
  if (n <= 0 || static_cast<size_t>(n) >= cap) {
    return -1;
  }
  return n;
}

// Renders the hourly MQTT payload. Returns the length, or -1 if the buffer is too small
// or rec.id cannot be formatted as UTC. generation.export_kwh / delta_export_kwh are the
// production counter from assign_hourly_generation. The key names are historical; the
// values are not raw eExp_Ws. net.import_kwh and net.export_kwh stay on the NET channel's
// own registers. timestamp is rebuilt from rec.id, not from the bytes stored in the record.
// ack_required is the bridge's require-hourly-ACK setting at publish time, passed in
// rather than stored: it is component state, not record data, and HourlyRecord's
// NVS-serialized layout must not change for it. false marks a testing-mode record that
// was published without persistence and expects no ACK.
inline int format_hourly_json(const HourlyRecord &rec, bool ack_required, char *buf, size_t cap) {
  if (buf == nullptr || cap == 0) {
    return -1;
  }
  char timestamp[24];
  if (!format_utc_timestamp(rec.id, timestamp, sizeof(timestamp))) {
    return -1;
  }
  double net_delta_imp = calculate_delta_kwh(rec.net_imp_ws, rec.base_net_imp_ws);
  double net_delta_exp = calculate_delta_kwh(rec.net_exp_ws, rec.base_net_exp_ws);
  double net_delta = net_delta_imp - net_delta_exp;
  double gen_delta_exp = calculate_delta_kwh(rec.gen_exp_ws, rec.base_gen_exp_ws);
  double cons_delta_imp = calculate_delta_kwh(rec.cons_imp_ws, rec.base_cons_imp_ws);
  double p1_delta_imp = calculate_delta_kwh(rec.phase_a_imp_ws, rec.base_phase_a_imp_ws);
  double p2_delta_imp = calculate_delta_kwh(rec.phase_b_imp_ws, rec.base_phase_b_imp_ws);

  // timestamp is digits and fixed separators from gmtime; it contains no quotes.
  int n = std::snprintf(
      buf, cap,
      "{\"id\":%u,\"timestamp\":\"%s\",\"retries\":%u,\"ack_required\":%s,"
      "\"net\":{\"import_kwh\":%.3f,\"export_kwh\":%.3f,\"delta_import_kwh\":%.3f,"
      "\"delta_export_kwh\":%.3f,\"delta_net_kwh\":%.3f},"
      "\"generation\":{\"export_kwh\":%.3f,\"delta_export_kwh\":%.3f},"
      "\"consumption\":{\"import_kwh\":%.3f,\"delta_import_kwh\":%.3f},"
      "\"phase_a\":{\"delta_import_kwh\":%.3f,\"voltage_v\":%.1f},"
      "\"phase_b\":{\"delta_import_kwh\":%.3f,\"voltage_v\":%.1f}}",
      static_cast<unsigned>(rec.id), timestamp, static_cast<unsigned>(rec.retries), ack_required ? "true" : "false",
      ws_to_kwh(rec.net_imp_ws),
      ws_to_kwh(rec.net_exp_ws), net_delta_imp, net_delta_exp, net_delta, ws_to_kwh(rec.gen_exp_ws), gen_delta_exp,
      ws_to_kwh(rec.cons_imp_ws), cons_delta_imp, p1_delta_imp, static_cast<double>(rec.phase_a_voltage), p2_delta_imp,
      static_cast<double>(rec.phase_b_voltage));
  if (n < 0 || static_cast<size_t>(n) >= cap) {
    return -1;
  }
  return n;
}

}  // namespace neurio_evse
}  // namespace esphome
