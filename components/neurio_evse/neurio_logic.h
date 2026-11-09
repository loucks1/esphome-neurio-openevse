#pragma once

// Host-testable Neurio parsing, hourly JSON, and the OpenEVSE feed gate.
// No ESPHome or ESP-IDF headers: tests/test_unit.cpp compiles this on the host.

#include "neurio_math.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>

namespace esphome {
namespace neurio_evse {

static constexpr uint32_t kHourlyMagic = 0x4E523031u;  // 'NR01'
static constexpr uint16_t kHourlyVersion = 1;
static constexpr size_t kHttpBodyCap = 3072;

// One hourly snapshot. Stored as a single NVS blob (see neurio_evse.cpp) and
// rendered to the MQTT JSON contract when publishing or retrying.
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
  int32_t q_var{0};
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
  bool sun_gate_enabled{false};
  bool sun_elevation_valid{false};
  float sun_elevation_deg{0.0f};
  float min_elevation_deg{0.0f};
};

struct FeedDecision {
  FeedPublish publish{FeedPublish::LIVE};
  // Points at a string literal. Used for transition logs.
  const char *reason{"enabled"};
};

// Live when the optional HA switch is on (or absent) and, if a local sun gate
// is configured and the clock is valid, elevation is strictly above the minimum.
// A missing sun fix does not block: SNTP may still be catching up after boot.
// The switch, when configured and off, always wins.
inline FeedDecision decide_feed(const FeedGateInput &in, GatedPublishMode mode) {
  FeedPublish gated = (mode == GatedPublishMode::SILENT) ? FeedPublish::SILENT : FeedPublish::SENTINEL;
  if (in.switch_configured && !in.switch_on) {
    return {gated, "switch off"};
  }
  if (in.sun_gate_enabled && in.sun_elevation_valid && !(in.sun_elevation_deg > in.min_elevation_deg)) {
    return {gated, "sun elevation"};
  }
  if (in.sun_gate_enabled && !in.sun_elevation_valid) {
    return {FeedPublish::LIVE, "sun not synced"};
  }
  return {FeedPublish::LIVE, "enabled"};
}

// neurio/raw is Home Assistant's source of truth and is never passed through the gate.
inline bool raw_topic_is_gated() { return false; }

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

  const char *q = find_key_value(obj, len, "q_VAR");
  if (q == nullptr) {
    q = find_key_value(obj, len, "Q");
  }
  if (q != nullptr) {
    parse_i32(q, obj + len, &dst.q_var);
  }

  const char *v = find_key_value(obj, len, "v_V");
  if (v == nullptr) {
    v = find_key_value(obj, len, "V");
  }
  if (v != nullptr) {
    parse_f32(v, obj + len, &dst.v_v);
  }
}

// Parses a Neurio /current-sample body. ok is true when a channels array is present.
// Individual channels are marked valid when their object contains ch in 1..5.
inline bool parse_neurio_sample(const char *json, size_t len, ParsedSample *out) {
  *out = ParsedSample{};
  if (json == nullptr || len == 0) {
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

// Renders the hourly MQTT payload. Returns the length, or -1 if the buffer is too small.
inline int format_hourly_json(const HourlyRecord &rec, char *buf, size_t cap) {
  if (buf == nullptr || cap == 0) {
    return -1;
  }
  double net_delta_imp = calculate_delta_kwh(rec.net_imp_ws, rec.base_net_imp_ws);
  double net_delta_exp = calculate_delta_kwh(rec.net_exp_ws, rec.base_net_exp_ws);
  double net_delta = net_delta_imp - net_delta_exp;
  double gen_delta_exp = calculate_delta_kwh(rec.gen_exp_ws, rec.base_gen_exp_ws);
  double cons_delta_imp = calculate_delta_kwh(rec.cons_imp_ws, rec.base_cons_imp_ws);
  double p1_delta_imp = calculate_delta_kwh(rec.phase_a_imp_ws, rec.base_phase_a_imp_ws);
  double p2_delta_imp = calculate_delta_kwh(rec.phase_b_imp_ws, rec.base_phase_b_imp_ws);

  // timestamp is produced by strftime and contains no quotes.
  int n = std::snprintf(
      buf, cap,
      "{\"id\":%u,\"timestamp\":\"%s\",\"retries\":%u,"
      "\"net\":{\"import_kwh\":%.3f,\"export_kwh\":%.3f,\"delta_import_kwh\":%.3f,"
      "\"delta_export_kwh\":%.3f,\"delta_net_kwh\":%.3f},"
      "\"generation\":{\"export_kwh\":%.3f,\"delta_export_kwh\":%.3f},"
      "\"consumption\":{\"import_kwh\":%.3f,\"delta_import_kwh\":%.3f},"
      "\"phase_a\":{\"delta_import_kwh\":%.3f,\"voltage_v\":%.1f},"
      "\"phase_b\":{\"delta_import_kwh\":%.3f,\"voltage_v\":%.1f}}",
      static_cast<unsigned>(rec.id), rec.timestamp, static_cast<unsigned>(rec.retries), ws_to_kwh(rec.net_imp_ws),
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
