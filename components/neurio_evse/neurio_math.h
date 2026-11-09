#pragma once

#include <cstdint>
#include <string>
#include <cstdlib>
#include <cmath>

namespace esphome {
namespace neurio_evse {

// Converts Watt-seconds (Ws) to kilowatt-hours (kWh)
inline double ws_to_kwh(uint64_t ws) {
  return static_cast<double>(ws) / 3600000.0;
}

// Calculates delta in kWh between current and baseline counters
// Safely handles hardware resets or counter rollovers
inline double calculate_delta_kwh(uint64_t current_ws, uint64_t baseline_ws) {
  if (current_ws >= baseline_ws) {
    return static_cast<double>(current_ws - baseline_ws) / 3600000.0;
  }
  // If counter wrapped or reset, treat delta as 0 or current reading
  return 0.0;
}

// Calculates reported power for OpenEVSE by applying user offset
// Positive net = importing; negative net = solar export
inline int32_t calculate_reported_power(int32_t net_power_w, float offset_watts) {
  return net_power_w + static_cast<int32_t>(std::round(offset_watts));
}

// Parses MQTT ACK payload which can be either a raw numeric string or a JSON object:
// e.g., "1730980800" or '{"id": 1730980800, "status": "ok"}'
inline uint32_t parse_ack_id(const std::string &payload) {
  if (payload.empty()) {
    return 0;
  }

  // Fast-path: Check if raw integer string
  char *endptr = nullptr;
  unsigned long val = std::strtoul(payload.c_str(), &endptr, 10);
  if (endptr != nullptr && (*endptr == '\0' || *endptr == '\r' || *endptr == '\n') && val > 0) {
    return static_cast<uint32_t>(val);
  }

  // JSON path: Look for "id" key
  size_t id_pos = payload.find("\"id\"");
  if (id_pos != std::string::npos) {
    size_t colon_pos = payload.find(':', id_pos);
    if (colon_pos != std::string::npos) {
      size_t num_start = payload.find_first_of("0123456789", colon_pos);
      if (num_start != std::string::npos) {
        val = std::strtoul(payload.c_str() + num_start, nullptr, 10);
        return static_cast<uint32_t>(val);
      }
    }
  }

  return 0;
}

}  // namespace neurio_evse
}  // namespace esphome
