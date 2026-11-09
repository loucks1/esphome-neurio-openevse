#include <iostream>
#include <cassert>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "../components/neurio_evse/neurio_math.h"
#include "../components/neurio_evse/neurio_logic.h"
#include "../components/neurio_evse/outage_queue.h"

using namespace esphome::neurio_evse;

#define TEST_ASSERT(cond, msg) \
  do { \
    if (!(cond)) { \
      std::cerr << "FAILED: " << msg << " (" << #cond << ") at line " << __LINE__ << std::endl; \
      return false; \
    } \
  } while(0)

bool test_energy_math() {
  std::cout << "[RUN] test_energy_math..." << std::endl;

  // 1. Basic conversion: 3,600,000 Ws = 1.0 kWh
  double kwh1 = ws_to_kwh(3600000ULL);
  TEST_ASSERT(std::abs(kwh1 - 1.0) < 1e-6, "3.6M Ws should equal 1.0 kWh");

  // 2. 64-bit precision test with real Neurio user values:
  // User's Net Import counter: 445441588847 Ws
  uint64_t neurio_net_imp_ws = 445441588847ULL;
  double net_kwh = ws_to_kwh(neurio_net_imp_ws);
  double expected_kwh = 445441588847.0 / 3600000.0;
  TEST_ASSERT(std::abs(net_kwh - expected_kwh) < 1e-4, "64-bit conversion matches expected");
  TEST_ASSERT(net_kwh > 123733.0 && net_kwh < 123734.0, "Net kWh in expected 123,733 range");

  // 3. Hourly Delta calculation
  uint64_t base_ws = 445441588847ULL;
  uint64_t next_hour_ws = base_ws + 5220000ULL; // 5.22M Ws = 1.45 kWh
  double delta_kwh = calculate_delta_kwh(next_hour_ws, base_ws);
  TEST_ASSERT(std::abs(delta_kwh - 1.45) < 1e-5, "Delta kWh calculation should equal 1.45 kWh");

  // 4. Counter Reset / Rollover safety
  double reset_delta = calculate_delta_kwh(100ULL, 500ULL);
  TEST_ASSERT(reset_delta == 0.0, "Reset delta should return 0.0 safely");

  std::cout << "[PASS] test_energy_math" << std::endl;
  return true;
}

bool test_openevse_offset() {
  std::cout << "[RUN] test_openevse_offset..." << std::endl;

  // Real user data: Net power = -2256 W (solar surplus)
  int32_t net_p = -2256;

  // 1. Zero offset
  TEST_ASSERT(calculate_reported_power(net_p, 0.0f) == -2256, "Zero offset leaves power unchanged");

  // 2. Positive offset (+500 W): Throttles charger to reserve 500 W buffer
  TEST_ASSERT(calculate_reported_power(net_p, 500.0f) == -1756, "Positive offset shifts power upwards");

  // 3. Negative offset (-1000 W): Forces charger to pull more
  TEST_ASSERT(calculate_reported_power(net_p, -1000.0f) == -3256, "Negative offset shifts power downwards");

  // 4. Grid import case (positive net power = 1500 W)
  TEST_ASSERT(calculate_reported_power(1500, 250.0f) == 1750, "Works for positive grid import power");

  std::cout << "[PASS] test_openevse_offset" << std::endl;
  return true;
}

bool test_ack_parser() {
  std::cout << "[RUN] test_ack_parser..." << std::endl;

  // 1. Raw integer string
  TEST_ASSERT(parse_ack_id("1730980800") == 1730980800, "Parses raw integer string");
  TEST_ASSERT(parse_ack_id("1730980800\n") == 1730980800, "Parses raw string with trailing newline");
  TEST_ASSERT(parse_ack_id("1730980800\r\n") == 1730980800, "Parses raw string with CRLF");

  // 2. JSON payloads
  TEST_ASSERT(parse_ack_id("{\"id\": 1730980800}") == 1730980800, "Parses standard JSON with id");
  TEST_ASSERT(parse_ack_id("{\"id\": 1730980800, \"status\": \"ok\"}") == 1730980800, "Parses JSON with extra fields");
  TEST_ASSERT(parse_ack_id("{\"status\":\"ok\",\"id\":1730980800}") == 1730980800, "Parses JSON with id later");

  // 3. Invalid inputs
  TEST_ASSERT(parse_ack_id("") == 0, "Empty payload returns 0");
  TEST_ASSERT(parse_ack_id("not_a_number") == 0, "Invalid string returns 0");
  TEST_ASSERT(parse_ack_id("{\"status\": \"ok\"}") == 0, "JSON missing id returns 0");

  std::cout << "[PASS] test_ack_parser" << std::endl;
  return true;
}

bool test_outage_queue() {
  std::cout << "[RUN] test_outage_queue..." << std::endl;

  OutageQueue q(5); // Test with a small capacity of 5

  TEST_ASSERT(q.empty(), "New queue should be empty");
  TEST_ASSERT(q.size() == 0, "New queue size should be 0");
  TEST_ASSERT(q.oldest_id() == 0, "New queue oldest_id should be 0");

  // 1. Push records within capacity
  uint32_t evicted = q.push_record(101);
  TEST_ASSERT(evicted == 0, "No eviction on first push");
  TEST_ASSERT(q.size() == 1, "Queue size 1");
  TEST_ASSERT(q.oldest_id() == 101, "Oldest is 101");

  q.push_record(102);
  q.push_record(103);
  q.push_record(104);
  q.push_record(105);
  TEST_ASSERT(q.size() == 5, "Queue full at 5");
  TEST_ASSERT(q.oldest_id() == 101, "Oldest remains 101");

  // 2. Exceed capacity (push 6th record -> should evict 101)
  evicted = q.push_record(106);
  TEST_ASSERT(evicted == 101, "Evicted oldest record 101");
  TEST_ASSERT(q.size() == 5, "Queue size stays bounded at 5");
  TEST_ASSERT(q.oldest_id() == 102, "New oldest is 102");

  // 3. Handle ACK for a record in the middle
  bool handled = q.handle_ack(104);
  TEST_ASSERT(handled == true, "ACK for 104 handled");
  TEST_ASSERT(q.size() == 4, "Queue size decremented to 4");

  // 4. Handle ACK for non-existent record
  handled = q.handle_ack(999);
  TEST_ASSERT(handled == false, "ACK for unknown record returns false");
  TEST_ASSERT(q.size() == 4, "Queue size unchanged");

  // 5. Handle ACK for oldest record
  handled = q.handle_ack(102);
  TEST_ASSERT(handled == true, "ACK for 102 handled");
  TEST_ASSERT(q.oldest_id() == 103, "New oldest is 103");

  // 6. Test 120 capacity for 5-day outage requirement
  OutageQueue full_q(120);
  for (uint32_t i = 1; i <= 120; i++) {
    full_q.push_record(1000 + i);
  }
  TEST_ASSERT(full_q.size() == 120, "Supports 120 hourly records (5 days of outage)");
  TEST_ASSERT(full_q.oldest_id() == 1001, "Oldest record is 1001");

  uint32_t ev = full_q.push_record(2000);
  TEST_ASSERT(ev == 1001, "121st push evicts 1001");
  TEST_ASSERT(full_q.size() == 120, "Capacity maintained at 120");

  std::cout << "[PASS] test_outage_queue" << std::endl;
  return true;
}

const char *kSample = R"JSON(
{
  "sensorId": "0x0000000000000000",
  "timestamp": "2026-11-07T09:02:05Z",
  "channels": [
    {"type": "PHASE_A_CONSUMPTION", "ch": 1, "p_W": 568, "eImp_Ws": 373905878762, "eExp_Ws": 2026661, "q_VAR": -110, "v_V": 123.705},
    {"type": "PHASE_B_CONSUMPTION", "ch": 2, "p_W": 87, "eImp_Ws": 252469196191, "eExp_Ws": 36215164, "q_VAR": -155, "v_V": 123.998},
    {"type": "NET", "ch": 3, "p_W": -2256, "eImp_Ws": 445441588847, "eExp_Ws": 236253198547, "q_VAR": -85, "v_V": 123.854},
    {"type": "GENERATION", "ch": 4, "p_W": 2910, "eImp_Ws": 417293809345, "eExp_Ws": 145364396, "q_VAR": -180, "v_V": 123.849},
    {"type": "CONSUMPTION", "ch": 5, "p_W": 654, "eImp_Ws": 626280154794, "eExp_Ws": 0, "q_VAR": -265, "v_V": 123.851}
  ]
}
)JSON";

bool test_sample_parser() {
  std::cout << "[RUN] test_sample_parser..." << std::endl;
  ParsedSample sample;
  TEST_ASSERT(parse_neurio_sample(kSample, std::strlen(kSample), &sample), "parses sample");
  TEST_ASSERT(sample.ok, "channels array present");
  TEST_ASSERT(sample.channels[1].valid, "ch1 valid");
  TEST_ASSERT(sample.channels[1].p_w == 568, "ch1 power");
  TEST_ASSERT(std::fabs(sample.channels[1].v_v - 123.705f) < 0.001f, "ch1 voltage");
  TEST_ASSERT(sample.channels[1].q_var == -110, "ch1 reactive");
  TEST_ASSERT(sample.channels[3].p_w == -2256, "net power");
  TEST_ASSERT(sample.channels[3].e_imp_ws == 445441588847ULL, "net import counter");
  TEST_ASSERT(sample.channels[3].e_exp_ws == 236253198547ULL, "net export counter");
  TEST_ASSERT(sample.channels[4].p_w == 2910, "generation");
  TEST_ASSERT(sample.channels[5].e_exp_ws == 0, "consumption export is zero");
  TEST_ASSERT(!sample.channels[0].valid, "channel 0 unused");

  const char *alt = R"JSON({"channels":[{"ch":3,"P":1500,"eImp_Ws":10,"eExp_Ws":2,"Q":4,"V":120.5}]})JSON";
  ParsedSample alt_sample;
  TEST_ASSERT(parse_neurio_sample(alt, std::strlen(alt), &alt_sample), "parses alternate keys");
  TEST_ASSERT(alt_sample.channels[3].p_w == 1500, "P alias");
  TEST_ASSERT(alt_sample.channels[3].q_var == 4, "Q alias");
  TEST_ASSERT(std::fabs(alt_sample.channels[3].v_v - 120.5f) < 0.01f, "V alias");

  ParsedSample bad;
  TEST_ASSERT(!parse_neurio_sample("{}", 2, &bad), "missing channels is not ok");
  TEST_ASSERT(!parse_neurio_sample("", 0, &bad), "empty body is not ok");
  const char *truncated = "{\"channels\":[{\"ch\":3,\"p_W\":1}";
  TEST_ASSERT(!parse_neurio_sample(truncated, std::strlen(truncated), &bad), "truncated object fails");

  TEST_ASSERT(raw_topic_is_gated() == false, "neurio/raw is never gated");

  std::cout << "[PASS] test_sample_parser" << std::endl;
  return true;
}

bool test_hourly_json() {
  std::cout << "[RUN] test_hourly_json..." << std::endl;
  HourlyRecord rec{};
  rec.magic = kHourlyMagic;
  rec.version = kHourlyVersion;
  rec.id = 1730980800;
  rec.retries = 2;
  std::snprintf(rec.timestamp, sizeof(rec.timestamp), "2026-11-07T12:00:00Z");
  rec.base_net_imp_ws = 445441588847ULL;
  rec.net_imp_ws = rec.base_net_imp_ws + 5220000ULL;
  rec.base_net_exp_ws = 1000;
  rec.net_exp_ws = 1000 + 7920000ULL;
  rec.base_gen_exp_ws = 0;
  rec.gen_exp_ws = 3600000ULL;
  rec.base_cons_imp_ws = 0;
  rec.cons_imp_ws = 1800000ULL;
  rec.phase_a_voltage = 123.7f;
  rec.phase_b_voltage = 124.0f;

  char buf[768];
  int n = format_hourly_json(rec, buf, sizeof(buf));
  TEST_ASSERT(n > 0, "json fits");
  std::string json(buf);
  TEST_ASSERT(json.find("\"id\":1730980800") != std::string::npos, "id");
  TEST_ASSERT(json.find("\"retries\":2") != std::string::npos, "retries");
  TEST_ASSERT(json.find("\"timestamp\":\"2026-11-07T12:00:00Z\"") != std::string::npos, "timestamp");
  TEST_ASSERT(json.find("\"delta_import_kwh\":1.450") != std::string::npos, "delta import");
  TEST_ASSERT(json.find("\"delta_export_kwh\":2.200") != std::string::npos, "delta export");
  TEST_ASSERT(json.find("\"delta_net_kwh\":-0.750") != std::string::npos, "delta net");
  TEST_ASSERT(json.find("\"voltage_v\":123.7") != std::string::npos, "phase a voltage");
  TEST_ASSERT(format_hourly_json(rec, buf, 8) < 0, "tiny buffer fails");
  TEST_ASSERT(sizeof(HourlyRecord) == 144, "NVS record stays 144 bytes");

  std::cout << "[PASS] test_hourly_json" << std::endl;
  return true;
}

bool test_feed_gate() {
  std::cout << "[RUN] test_feed_gate..." << std::endl;

  FeedGateInput in;
  FeedDecision d = decide_feed(in, GatedPublishMode::IMPORT_SENTINEL);
  TEST_ASSERT(d.publish == FeedPublish::LIVE, "no switch and no sun is live");

  in.switch_configured = true;
  in.switch_on = true;
  d = decide_feed(in, GatedPublishMode::IMPORT_SENTINEL);
  TEST_ASSERT(d.publish == FeedPublish::LIVE, "switch on is live");

  in.switch_on = false;
  d = decide_feed(in, GatedPublishMode::IMPORT_SENTINEL);
  TEST_ASSERT(d.publish == FeedPublish::SENTINEL, "switch off publishes sentinel");
  TEST_ASSERT(std::string(d.reason) == "switch off", "reason switch off");

  d = decide_feed(in, GatedPublishMode::SILENT);
  TEST_ASSERT(d.publish == FeedPublish::SILENT, "silent mode publishes nothing");

  in.switch_on = true;
  in.sun_gate_enabled = true;
  in.sun_elevation_valid = false;
  d = decide_feed(in, GatedPublishMode::IMPORT_SENTINEL);
  TEST_ASSERT(d.publish == FeedPublish::LIVE, "unsynced sun does not block");

  in.sun_elevation_valid = true;
  in.sun_elevation_deg = 10.0f;
  in.min_elevation_deg = 0.0f;
  d = decide_feed(in, GatedPublishMode::IMPORT_SENTINEL);
  TEST_ASSERT(d.publish == FeedPublish::LIVE, "sun above minimum is live");

  in.sun_elevation_deg = 0.0f;
  d = decide_feed(in, GatedPublishMode::IMPORT_SENTINEL);
  TEST_ASSERT(d.publish == FeedPublish::SENTINEL, "elevation equal to minimum is gated");

  in.sun_elevation_deg = -1.0f;
  d = decide_feed(in, GatedPublishMode::IMPORT_SENTINEL);
  TEST_ASSERT(d.publish == FeedPublish::SENTINEL, "sun below minimum is gated");
  TEST_ASSERT(std::string(d.reason) == "sun elevation", "reason sun");

  in.switch_on = false;
  in.sun_elevation_deg = 20.0f;
  d = decide_feed(in, GatedPublishMode::IMPORT_SENTINEL);
  TEST_ASSERT(d.publish == FeedPublish::SENTINEL, "switch off wins over a high sun");

  in.switch_configured = false;
  in.switch_on = false;
  in.sun_elevation_deg = -5.0f;
  d = decide_feed(in, GatedPublishMode::IMPORT_SENTINEL);
  TEST_ASSERT(d.publish == FeedPublish::SENTINEL, "sun gate works without the switch");

  std::cout << "[PASS] test_feed_gate" << std::endl;
  return true;
}

int main() {
  std::cout << "============================================" << std::endl;
  std::cout << "Starting Neurio EVSE Native Unit Test Suite" << std::endl;
  std::cout << "============================================" << std::endl;

  bool all_passed = true;
  all_passed &= test_energy_math();
  all_passed &= test_openevse_offset();
  all_passed &= test_ack_parser();
  all_passed &= test_outage_queue();
  all_passed &= test_sample_parser();
  all_passed &= test_hourly_json();
  all_passed &= test_feed_gate();

  if (all_passed) {
    std::cout << "============================================" << std::endl;
    std::cout << "ALL 7 C++ TEST SUITES PASSED (100% SUCCESS)!" << std::endl;
    std::cout << "============================================" << std::endl;
    return 0;
  } else {
    std::cerr << "TEST SUITE FAILED!" << std::endl;
    return 1;
  }
}
