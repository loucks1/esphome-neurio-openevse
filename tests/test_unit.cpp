#include <iostream>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

  // 6. Test production capacity: 336 records = 14 days of outage, aligned
  // with the recorder's 14-day settle window.
  TEST_ASSERT(kOutageQueueCapacity == 336, "Production capacity is 336 records (14 days)");
  OutageQueue full_q;
  TEST_ASSERT(full_q.capacity() == kOutageQueueCapacity, "Default queue uses the production capacity");
  for (uint32_t i = 1; i <= kOutageQueueCapacity; i++) {
    full_q.push_record(1000 + i);
  }
  TEST_ASSERT(full_q.size() == kOutageQueueCapacity, "Supports 336 hourly records (14 days of outage)");
  TEST_ASSERT(full_q.oldest_id() == 1001, "Oldest record is 1001");

  uint32_t ev = full_q.push_record(2000);
  TEST_ASSERT(ev == 1001, "337th push evicts 1001");
  TEST_ASSERT(full_q.size() == kOutageQueueCapacity, "Capacity maintained at 336");

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
    {"type": "GENERATION", "ch": 4, "p_W": 2910, "eImp_Ws": 417700000000, "eExp_Ws": 145405882, "q_VAR": -180, "v_V": 123.849},
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
  TEST_ASSERT(sample.channels[3].p_w == -2256, "net power");
  TEST_ASSERT(sample.channels[3].e_imp_ws == 445441588847ULL, "net import counter");
  TEST_ASSERT(sample.channels[3].e_exp_ws == 236253198547ULL, "net export counter");
  TEST_ASSERT(sample.channels[4].p_w == 2910, "generation");
  TEST_ASSERT(sample.channels[4].e_imp_ws == 417700000000ULL, "generation import counter");
  TEST_ASSERT(sample.channels[4].e_exp_ws == 145405882ULL, "generation export residual");
  TEST_ASSERT(sample.channels[5].e_exp_ws == 0, "consumption export is zero");
  TEST_ASSERT(!sample.channels[0].valid, "channel 0 unused");

  const char *alt = R"JSON({"channels":[{"ch":3,"P":1500,"eImp_Ws":10,"eExp_Ws":2,"Q":4,"V":120.5}]})JSON";
  ParsedSample alt_sample;
  TEST_ASSERT(parse_neurio_sample(alt, std::strlen(alt), &alt_sample), "parses alternate keys");
  TEST_ASSERT(alt_sample.channels[3].p_w == 1500, "P alias");
  TEST_ASSERT(std::fabs(alt_sample.channels[3].v_v - 120.5f) < 0.01f, "V alias");

  ParsedSample bad;
  TEST_ASSERT(!parse_neurio_sample("{}", 2, &bad), "missing channels is not ok");
  TEST_ASSERT(!parse_neurio_sample("", 0, &bad), "empty body is not ok");
  const char *truncated = "{\"channels\":[{\"ch\":3,\"p_W\":1}";
  TEST_ASSERT(!parse_neurio_sample(truncated, std::strlen(truncated), &bad), "truncated object fails");

  const char *with_cts =
      "{\"sensorId\":\"0x1\",\"channels\":[{\"ch\":3,\"p_W\":-2256,\"eImp_Ws\":445441588847,\"eExp_Ws\":1}],"
      "\"cts\":[{\"ct\":4,\"p_W\":100}]}";
  ParsedSample cts_sample;
  TEST_ASSERT(parse_neurio_sample(with_cts, std::strlen(with_cts), &cts_sample), "object with cts after channels");
  TEST_ASSERT(cts_sample.channels[3].p_w == -2256, "net power still parsed when cts follows");
  TEST_ASSERT(cts_sample.channels[3].e_imp_ws == 445441588847ULL, "import counter with cts present");

  const char *cut_in_cts =
      "{\"channels\":[{\"ch\":3,\"p_W\":-2256,\"eImp_Ws\":10,\"eExp_Ws\":2}],\"cts\":[{\"ct\":4,\"p_W\":";
  TEST_ASSERT(!parse_neurio_sample(cut_in_cts, std::strlen(cut_in_cts), &bad), "truncated inside cts is not a sample");
  TEST_ASSERT(!bad.ok, "truncated cts does not mark the sample ok");
  TEST_ASSERT(!bad.channels[3].valid, "channels from a truncated object are not published");

  const char *channels_closed =
      "{\"channels\":[{\"ch\":3,\"p_W\":1,\"eImp_Ws\":2,\"eExp_Ws\":3}]";
  TEST_ASSERT(!parse_neurio_sample(channels_closed, std::strlen(channels_closed), &bad),
              "channels array closed but top-level object open");

  const char *trailing = "{\"channels\":[{\"ch\":3,\"p_W\":1}]}{\"cts\":";
  TEST_ASSERT(!parse_neurio_sample(trailing, std::strlen(trailing), &bad), "trailing junk after the object");

  const char *escaped = "{\"sensorId\":\"a\\\"b}\",\"channels\":[{\"ch\":1,\"p_W\":7}]}";
  ParsedSample escaped_sample;
  TEST_ASSERT(parse_neurio_sample(escaped, std::strlen(escaped), &escaped_sample), "braces inside strings still close");
  TEST_ASSERT(escaped_sample.channels[1].p_w == 7, "channel inside a string-escaped object");

  std::cout << "[PASS] test_sample_parser" << std::endl;
  return true;
}

bool test_close_delimited_http() {
  std::cout << "[RUN] test_close_delimited_http..." << std::endl;

  // Well-formed Content-Length responses are unchanged: success stays success,
  // and a perform() error with a declared length stays a failure.
  TEST_ASSERT(http_body_is_usable(true, 200, 877, false, 877), "content-length success");
  TEST_ASSERT(!http_body_is_usable(false, 200, 400, false, 877), "short content-length read stays a failure");
  TEST_ASSERT(!http_body_is_usable(false, 200, 877, false, 877), "content-length perform error stays a failure");

  // HTTP/1.0 close: no Content-Length (IDF reports length <= 0), status 200,
  // body already delivered, no overflow. perform() has returned an error.
  TEST_ASSERT(http_body_is_usable(false, 200, 877, false, -1), "close-delimited FIN is usable");
  TEST_ASSERT(http_body_is_usable(false, 200, 877, false, 0), "zero content-length class is close-delimited");
  TEST_ASSERT(http_body_is_usable(true, 200, 877, false, -1), "older IDF success on close-delimited still usable");

  TEST_ASSERT(!http_body_is_usable(false, 200, 0, false, -1), "empty close is not a sample");
  TEST_ASSERT(!http_body_is_usable(false, 200, 877, true, -1), "overflowed close is not usable");
  TEST_ASSERT(!http_body_is_usable(false, 500, 20, false, -1), "non-200 close is not usable");
  TEST_ASSERT(!http_body_is_usable(false, -1, 0, false, -1), "connect failure is not usable");

  std::cout << "[PASS] test_close_delimited_http" << std::endl;
  return true;
}

bool test_hourly_json() {
  std::cout << "[RUN] test_hourly_json..." << std::endl;
  HourlyRecord rec{};
  rec.magic = kHourlyMagic;
  rec.version = kHourlyVersion;
  rec.id = 1730980800;  // 2024-11-07T12:00:00Z
  rec.retries = 2;
  // A stored string that does not match the id. Publish rebuilds UTC from the id.
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
  TEST_ASSERT(json.find("\"timestamp\":\"2024-11-07T12:00:00Z\"") != std::string::npos, "timestamp follows id in UTC");
  TEST_ASSERT(json.find("2026-11-07T12:00:00Z") == std::string::npos, "stored timestamp is not published");
  TEST_ASSERT(json.find("\"delta_import_kwh\":1.450") != std::string::npos, "delta import");
  TEST_ASSERT(json.find("\"delta_export_kwh\":2.200") != std::string::npos, "delta export");
  TEST_ASSERT(json.find("\"delta_net_kwh\":-0.750") != std::string::npos, "delta net");
  TEST_ASSERT(json.find("\"generation\":{\"export_kwh\":1.000,\"delta_export_kwh\":1.000}") != std::string::npos,
              "generation object still uses the historical export_kwh keys");
  TEST_ASSERT(json.find("\"voltage_v\":123.7") != std::string::npos, "phase a voltage");
  TEST_ASSERT(format_hourly_json(rec, buf, 8) < 0, "tiny buffer fails");
  TEST_ASSERT(sizeof(HourlyRecord) == 144, "NVS record stays 144 bytes");

  std::cout << "[PASS] test_hourly_json" << std::endl;
  return true;
}

// Live GENERATION sample: eImp_Ws is ~417.7 billion Ws (~116000 kWh) and
// eExp_Ws is 145405882 Ws (~40.39 kWh). Lifetime solar and the hourly solar
// delta must follow the larger register. NET import/export stay independent.
bool test_generation_production_counter() {
  std::cout << "[RUN] test_generation_production_counter..." << std::endl;

  ParsedSample sample;
  TEST_ASSERT(parse_neurio_sample(kSample, std::strlen(kSample), &sample), "parses live generation fixture");
  const ParsedChannel &gen = sample.channels[4];
  const ParsedChannel &net = sample.channels[3];
  TEST_ASSERT(gen.valid, "generation channel present");
  TEST_ASSERT(gen.e_imp_ws == 417700000000ULL, "fixture import is ~417.7 billion Ws");
  TEST_ASSERT(gen.e_exp_ws == 145405882ULL, "fixture export residual");

  const uint64_t produced = generation_production_ws(gen.e_imp_ws, gen.e_exp_ws);
  TEST_ASSERT(produced == gen.e_imp_ws, "this Neurio accumulates solar on eImp_Ws");
  TEST_ASSERT(produced != gen.e_exp_ws, "eExp_Ws is not lifetime solar");
  const double lifetime_kwh = ws_to_kwh(produced);
  const double residual_kwh = ws_to_kwh(gen.e_exp_ws);
  TEST_ASSERT(std::abs(lifetime_kwh - (417700000000.0 / 3600000.0)) < 1e-6, "lifetime kWh is the import register");
  TEST_ASSERT(lifetime_kwh > 116000.0 && lifetime_kwh < 116100.0, "lifetime solar is about 116000 kWh");
  TEST_ASSERT(std::abs(residual_kwh - 40.39) < 0.01, "export residual is the ~40.39 kWh misread");

  // One hour later import has grown 2.5 kWh and the export residual has not moved.
  // Selecting eExp_Ws would publish generation delta 0.000.
  const uint64_t hour_ws = 9000000ULL;
  HourlyRecord rec{};
  rec.id = 1730995200;  // 2024-11-07T16:00:00Z; the JSON timestamp is this, not a stored string
  rec.net_imp_ws = net.e_imp_ws + 3600000ULL;
  rec.base_net_imp_ws = net.e_imp_ws;
  rec.net_exp_ws = net.e_exp_ws + 7200000ULL;
  rec.base_net_exp_ws = net.e_exp_ws;
  rec.cons_imp_ws = sample.channels[5].e_imp_ws;
  rec.base_cons_imp_ws = sample.channels[5].e_imp_ws;
  assign_hourly_generation(&rec, gen.e_imp_ws + hour_ws, gen.e_exp_ws, gen.e_imp_ws, gen.e_exp_ws);

  TEST_ASSERT(rec.gen_exp_ws == gen.e_imp_ws + hour_ws, "hourly total is the import register");
  TEST_ASSERT(rec.base_gen_exp_ws == gen.e_imp_ws, "hourly baseline is that same register");
  TEST_ASSERT(rec.net_imp_ws == net.e_imp_ws + 3600000ULL, "net import counter untouched");
  TEST_ASSERT(rec.net_exp_ws == net.e_exp_ws + 7200000ULL, "net export counter untouched");

  char buf[768];
  int n = format_hourly_json(rec, buf, sizeof(buf));
  TEST_ASSERT(n > 0, "hourly json fits");
  std::string json(buf);
  TEST_ASSERT(json.find("\"generation\":{\"export_kwh\":116030.278,\"delta_export_kwh\":2.500}") != std::string::npos,
              "hourly solar follows the import increase");
  TEST_ASSERT(json.find("\"delta_export_kwh\":0.000") == std::string::npos, "export residual does not yield a zero solar hour");
  TEST_ASSERT(json.find("\"delta_import_kwh\":1.000") != std::string::npos, "net import delta unchanged");
  TEST_ASSERT(json.find("\"import_kwh\":123734.775") != std::string::npos, "net import total stays on eImp_Ws");
  TEST_ASSERT(json.find("\"export_kwh\":65627.888") != std::string::npos, "net export total stays on eExp_Ws");

  // Export-oriented GENERATION install: production is eExp_Ws, so the larger
  // register is still export and the delta uses the export baseline.
  HourlyRecord flipped{};
  const uint64_t flipped_imp = 145405882ULL;
  const uint64_t flipped_exp = 417700000000ULL;
  assign_hourly_generation(&flipped, flipped_imp, flipped_exp, flipped_imp, flipped_exp - 3600000ULL);
  TEST_ASSERT(flipped.gen_exp_ws == flipped_exp, "export-oriented install uses eExp_Ws");
  TEST_ASSERT(flipped.base_gen_exp_ws == flipped_exp - 3600000ULL, "baseline is the export register");
  TEST_ASSERT(std::abs(calculate_delta_kwh(flipped.gen_exp_ws, flipped.base_gen_exp_ws) - 1.0) < 1e-9,
              "export-oriented hour is 1 kWh");

  // Tie uses import, including its baseline rather than the export baseline.
  HourlyRecord tied{};
  assign_hourly_generation(&tied, 5000, 5000, 1400, 3600);
  TEST_ASSERT(tied.gen_exp_ws == 5000ULL, "tie keeps the import counter");
  TEST_ASSERT(tied.base_gen_exp_ws == 1400ULL, "tie baseline is the import register");

  // Current import is larger, but the export baseline is larger than the import
  // baseline. The delta must be this register's own increase (1.000 kWh), not
  // max(current) - max(baseline) (0.444 kWh).
  HourlyRecord same_register{};
  assign_hourly_generation(&same_register, 13600000ULL, 50ULL, 10000000ULL, 12000000ULL);
  TEST_ASSERT(same_register.gen_exp_ws == 13600000ULL, "current production register is import");
  TEST_ASSERT(same_register.base_gen_exp_ws == 10000000ULL, "baseline stays on import");
  TEST_ASSERT(std::abs(calculate_delta_kwh(same_register.gen_exp_ws, same_register.base_gen_exp_ws) - 1.0) < 1e-9,
              "delta is the import register's own increase");

  // NET export can be the larger NET register without affecting generation,
  // and generation selection must not rewrite the NET counters.
  HourlyRecord net_larger_export{};
  net_larger_export.net_imp_ws = 3600000ULL;
  net_larger_export.base_net_imp_ws = 0;
  net_larger_export.net_exp_ws = 7200000ULL;
  net_larger_export.base_net_exp_ws = 0;
  assign_hourly_generation(&net_larger_export, 417700000000ULL, 145405882ULL, 417700000000ULL, 145405882ULL);
  TEST_ASSERT(net_larger_export.net_imp_ws == 3600000ULL, "larger generation import is not copied onto net import");
  TEST_ASSERT(net_larger_export.net_exp_ws == 7200000ULL, "net export stays the NET export register");
  TEST_ASSERT(net_larger_export.gen_exp_ws == 417700000000ULL, "generation still uses its own larger register");
  char net_buf[768];
  net_larger_export.id = 1730998800;  // 2024-11-07T17:00:00Z
  int net_n = format_hourly_json(net_larger_export, net_buf, sizeof(net_buf));
  TEST_ASSERT(net_n > 0, "net-semantics json fits");
  std::string net_json(net_buf);
  TEST_ASSERT(net_json.find("\"net\":{\"import_kwh\":1.000,\"export_kwh\":2.000,\"delta_import_kwh\":1.000,"
                            "\"delta_export_kwh\":2.000,\"delta_net_kwh\":-1.000}") != std::string::npos,
              "net keeps separate import and export when export is larger");
  TEST_ASSERT(net_json.find("\"generation\":{\"export_kwh\":116027.778,\"delta_export_kwh\":0.000}") != std::string::npos,
              "unchanged production hour still reports the import total");

  std::cout << "[PASS] test_generation_production_counter" << std::endl;
  return true;
}

bool test_feed_gate() {
  std::cout << "[RUN] test_feed_gate..." << std::endl;

  FeedGateInput in;
  FeedDecision d = decide_feed(in, GatedPublishMode::IMPORT_SENTINEL);
  TEST_ASSERT(d.publish == FeedPublish::LIVE, "no switch is live");

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

  std::cout << "[PASS] test_feed_gate" << std::endl;
  return true;
}

struct TzGuard {
  std::string saved;
  bool had{false};
  explicit TzGuard(const char *tz) {
    const char *old = std::getenv("TZ");
    had = old != nullptr;
    if (had) {
      saved = old;
    }
    set(tz);
  }
  void set(const char *tz) {
    ::setenv("TZ", tz, 1);
    ::tzset();
  }
  ~TzGuard() {
    if (had) {
      ::setenv("TZ", saved.c_str(), 1);
    } else {
      ::unsetenv("TZ");
    }
    ::tzset();
  }
};

struct Civil {
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int isdst;
};

Civil local_civil(time_t epoch) {
  struct tm local {};
  localtime_r(&epoch, &local);
  return {local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_isdst};
}

std::vector<uint32_t> hours_crossed(int64_t start, int64_t end, int64_t step) {
  uint32_t prev = 0;
  std::vector<uint32_t> emitted;
  for (int64_t t = start; t <= end; t += step) {
    uint32_t due = note_utc_sample(prev, t);
    if (due != 0) {
      emitted.push_back(due);
    }
  }
  return emitted;
}

bool hours_are_consecutive(const std::vector<uint32_t> &ids) {
  if (ids.empty()) {
    return false;
  }
  for (size_t i = 1; i < ids.size(); i++) {
    if (ids[i] != ids[i - 1] + 3600u) {
      return false;
    }
  }
  return true;
}

// The live bug: id 1794258000 is 2026-11-09T21:00:00Z, and America/Chicago
// (UTC-6 that day) formatted it as "2026-11-09T15:00:00Z". Hour buckets follow
// UTC so a DST fall-back cannot drop an hour and a half-hour zone cannot
// split a UTC hour.
bool test_utc_hourly_timestamps() {
  std::cout << "[RUN] test_utc_hourly_timestamps..." << std::endl;
  TzGuard tz("America/Chicago");

  const time_t bug_epoch = 1794258000;  // 2026-11-09T21:00:00Z
  Civil bug_local = local_civil(bug_epoch);
  TEST_ASSERT(bug_local.year == 2026 && bug_local.month == 11 && bug_local.day == 9, "Chicago date of the bug epoch");
  TEST_ASSERT(bug_local.hour == 15 && bug_local.minute == 0, "Chicago is 15:00, the bad string");
  TEST_ASSERT(bug_local.isdst == 0, "Nov 9 2026 is standard time");

  char utc_bug[24];
  TEST_ASSERT(format_utc_timestamp(static_cast<uint32_t>(bug_epoch), utc_bug, sizeof(utc_bug)), "formats bug epoch");
  TEST_ASSERT(std::string(utc_bug) == "2026-11-09T21:00:00Z", "Z timestamp is 21:00 UTC, not 15:00 local");

  char too_small[20];
  TEST_ASSERT(!format_utc_timestamp(static_cast<uint32_t>(bug_epoch), too_small, sizeof(too_small)),
              "20 bytes leaves no room for NUL");
  char exact[21];
  TEST_ASSERT(format_utc_timestamp(static_cast<uint32_t>(bug_epoch), exact, sizeof(exact)), "21 bytes fits");
  TEST_ASSERT(std::string(exact) == "2026-11-09T21:00:00Z", "exact buffer matches");

  // A few seconds past the boundary still names the hour the id names.
  char plus[24];
  TEST_ASSERT(format_utc_timestamp(static_cast<uint32_t>(bug_epoch) + 125, plus, sizeof(plus)), "formats +125s");
  TEST_ASSERT(std::string(plus) == "2026-11-09T21:02:05Z", "offset inside the hour is still UTC");
  TEST_ASSERT(utc_hour_id(bug_epoch + 3599) == static_cast<uint32_t>(bug_epoch), "floors to the UTC hour");
  TEST_ASSERT(utc_hour_id(0) == 0, "epoch 0 is not an hour id");
  TEST_ASSERT(utc_hour_id(-1) == 0, "negative time is not an hour id");

  // CDT is UTC-5. The string must stay 21:00Z while local time is 16:00.
  const time_t cdt_epoch = 1786309200;  // 2026-08-09T21:00:00Z
  Civil cdt_local = local_civil(cdt_epoch);
  TEST_ASSERT(cdt_local.hour == 16 && cdt_local.minute == 0 && cdt_local.isdst == 1, "August is 16:00 CDT");
  char cdt_utc[24];
  TEST_ASSERT(format_utc_timestamp(static_cast<uint32_t>(cdt_epoch), cdt_utc, sizeof(cdt_utc)), "formats CDT epoch");
  TEST_ASSERT(std::string(cdt_utc) == "2026-08-09T21:00:00Z", "CDT local hour is not written with Z");

  // Stored NVS string is the bug. The payload follows the id.
  HourlyRecord stored{};
  stored.magic = kHourlyMagic;
  stored.version = kHourlyVersion;
  stored.id = static_cast<uint32_t>(bug_epoch);
  stored.retries = 3;
  std::snprintf(stored.timestamp, sizeof(stored.timestamp), "2026-11-09T15:00:00Z");
  TEST_ASSERT(sizeof(HourlyRecord) == 144, "healing the string does not resize the NVS record");
  char json_buf[768];
  int json_n = format_hourly_json(stored, json_buf, sizeof(json_buf));
  TEST_ASSERT(json_n > 0, "stored record still renders");
  std::string json(json_buf);
  TEST_ASSERT(json.find("\"id\":1794258000") != std::string::npos, "id unchanged");
  TEST_ASSERT(json.find("\"timestamp\":\"2026-11-09T21:00:00Z\"") != std::string::npos, "publish rebuilds UTC from id");
  TEST_ASSERT(json.find("2026-11-09T15:00:00Z") == std::string::npos, "local string with Z is not published");
  TEST_ASSERT(json.find("\"retries\":3") != std::string::npos, "retries preserved");
  TEST_ASSERT(std::string(stored.timestamp) == "2026-11-09T15:00:00Z", "render does not mutate the stored blob");

  // Fall back, 2026-11-01. 06:00Z and 07:00Z are both local 01:00.
  const time_t fall_cdt = 1793512805;  // 2026-11-01T06:00:05Z = 01:00:05 CDT
  const time_t fall_cst = 1793516405;  // 2026-11-01T07:00:05Z = 01:00:05 CST
  Civil fall_cdt_local = local_civil(fall_cdt);
  Civil fall_cst_local = local_civil(fall_cst);
  TEST_ASSERT(fall_cdt_local.hour == 1 && fall_cdt_local.isdst == 1, "first 1:00 is CDT");
  TEST_ASSERT(fall_cst_local.hour == 1 && fall_cst_local.isdst == 0, "second 1:00 is CST");
  TEST_ASSERT(fall_cdt_local.hour == fall_cst_local.hour, "local hour number repeats across fall-back");
  // 05:00Z through 09:00Z, sampled faster than the device poll.
  std::vector<uint32_t> fall = hours_crossed(1793509200, 1793523600, 5);
  TEST_ASSERT(fall.size() == 4, "fall-back emits four UTC hours from 06:00Z through 09:00Z");
  TEST_ASSERT(hours_are_consecutive(fall), "fall-back hours are 3600s apart with no duplicate");
  TEST_ASSERT(fall.front() == 1793512800u && fall[1] == 1793516400u && fall.back() == 1793523600u,
              "both repeated local 1:00 hours are distinct UTC ids");
  for (uint32_t id : fall) {
    char stamp[24];
    TEST_ASSERT(format_utc_timestamp(id, stamp, sizeof(stamp)), "fall-back stamp formats");
    TEST_ASSERT(utc_hour_id(id) == id, "emitted id is already an hour boundary");
    TEST_ASSERT(std::string(stamp).size() == 20, "stamp is 20 characters");
    TEST_ASSERT(std::string(stamp).substr(13) == ":00:00Z", "emitted hour is aligned and labeled Z");
  }
  char fall_cdt_stamp[24];
  char fall_cst_stamp[24];
  TEST_ASSERT(format_utc_timestamp(1793512800u, fall_cdt_stamp, sizeof(fall_cdt_stamp)), "06:00Z formats");
  TEST_ASSERT(format_utc_timestamp(1793516400u, fall_cst_stamp, sizeof(fall_cst_stamp)), "07:00Z formats");
  TEST_ASSERT(std::string(fall_cdt_stamp) == "2026-11-01T06:00:00Z", "first 1:00 local is 06:00Z");
  TEST_ASSERT(std::string(fall_cst_stamp) == "2026-11-01T07:00:00Z", "second 1:00 local is 07:00Z");

  // Spring forward, 2026-03-08. 07:59:50Z is 01:59:50 CST; 08:00:05Z is 03:00:05 CDT.
  const time_t spring_before = 1772956790;
  const time_t spring_after = 1772956805;
  Civil spring_before_local = local_civil(spring_before);
  Civil spring_after_local = local_civil(spring_after);
  TEST_ASSERT(spring_before_local.hour == 1 && spring_before_local.isdst == 0, "just before the jump is 1:59 CST");
  TEST_ASSERT(spring_after_local.hour == 3 && spring_after_local.isdst == 1, "just after the jump is 3:00 CDT");
  std::vector<uint32_t> spring = hours_crossed(1772949600, 1772964000, 5);
  TEST_ASSERT(spring.size() == 4, "spring-forward emits four UTC hours from 07:00Z through 10:00Z");
  TEST_ASSERT(hours_are_consecutive(spring), "spring-forward does not skip or duplicate a UTC hour");
  TEST_ASSERT(spring.front() == 1772953200u && spring[1] == 1772956800u, "08:00Z is present even though local 2:00 is not");
  char spring_stamp[24];
  TEST_ASSERT(format_utc_timestamp(1772956800u, spring_stamp, sizeof(spring_stamp)), "08:00Z formats");
  TEST_ASSERT(std::string(spring_stamp) == "2026-03-08T08:00:00Z", "skipped local label is still 08:00Z");

  // Same local hour number a day later is still a new UTC hour.
  uint32_t tracked = 0;
  TEST_ASSERT(note_utc_sample(tracked, bug_epoch) == 0, "first valid sample does not emit");
  TEST_ASSERT(tracked == static_cast<uint32_t>(bug_epoch), "first sample arms the tracker");
  TEST_ASSERT(note_utc_sample(tracked, bug_epoch + 1800) == 0, "thirty minutes later is the same UTC hour");
  TEST_ASSERT(note_utc_sample(tracked, bug_epoch + 86400) == static_cast<uint32_t>(bug_epoch) + 86400u,
              "24h later emits even though the local hour number matches");
  TEST_ASSERT(note_utc_sample(tracked, bug_epoch + 3600) == 0, "a backward step does not emit a duplicate");
  TEST_ASSERT(tracked == static_cast<uint32_t>(bug_epoch) + 86400u, "tracker stays on the later hour");
  TEST_ASSERT(note_utc_sample(tracked, bug_epoch + 86400 + 3600 + 7) == static_cast<uint32_t>(bug_epoch) + 86400u + 3600u,
              "the next hour is floored, not the poll's seconds");

  // Asia/Kolkata is UTC+5:30, so local hours change at xx:30Z. Buckets stay on UTC hours.
  tz.set("Asia/Kolkata");
  Civil kolkata = local_civil(bug_epoch);
  TEST_ASSERT(kolkata.day == 10 && kolkata.hour == 2 && kolkata.minute == 30, "21:00Z is 02:30 in Kolkata");
  char kolkata_utc[24];
  TEST_ASSERT(format_utc_timestamp(static_cast<uint32_t>(bug_epoch), kolkata_utc, sizeof(kolkata_utc)),
              "formats under Kolkata");
  TEST_ASSERT(std::string(kolkata_utc) == "2026-11-09T21:00:00Z", "half-hour zone does not shift the Z string");
  Civil kolkata_half = local_civil(1794259800);  // 2026-11-09T21:30:00Z = 03:00 IST
  TEST_ASSERT(kolkata_half.hour == 3 && kolkata_half.minute == 0, "21:30Z is a local hour boundary");
  uint32_t kolkata_prev = 0;
  std::vector<uint32_t> kolkata_hours;
  for (int64_t t = 1794254400; t <= 1794265200; t += 1800) {  // 20:00Z .. 23:00Z
    uint32_t due = note_utc_sample(kolkata_prev, t);
    if (due != 0) {
      kolkata_hours.push_back(due);
    }
  }
  TEST_ASSERT(kolkata_hours.size() == 3, "three UTC hours, not every local hour");
  TEST_ASSERT(kolkata_hours[0] == 1794258000u && kolkata_hours[1] == 1794261600u && kolkata_hours[2] == 1794265200u,
              "emits 21:00Z, 22:00Z, 23:00Z and not the 21:30Z local boundary");

  std::cout << "[PASS] test_utc_hourly_timestamps" << std::endl;
  return true;
}

bool test_vrms_payload() {
  std::cout << "[RUN] test_vrms_payload..." << std::endl;

  char buf[16];
  int n = format_vrms_payload(buf, sizeof(buf), 120.4f, 121.3f);
  TEST_ASSERT(n == 5, "vrms payload length");
  TEST_ASSERT(std::string(buf) == "241.7", "vrms payload is the phase voltage sum, one decimal");

  TEST_ASSERT(format_vrms_payload(buf, sizeof(buf), 0.0f, 121.3f) < 0, "zero phase A voltage is not published");
  TEST_ASSERT(format_vrms_payload(buf, sizeof(buf), 120.4f, 0.0f) < 0, "zero phase B voltage is not published");
  TEST_ASSERT(format_vrms_payload(buf, 4, 120.4f, 121.3f) < 0, "tiny buffer fails");

  std::cout << "[PASS] test_vrms_payload" << std::endl;  return true;
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
  all_passed &= test_close_delimited_http();
  all_passed &= test_hourly_json();
  all_passed &= test_utc_hourly_timestamps();
  all_passed &= test_generation_production_counter();
  all_passed &= test_feed_gate();
  all_passed &= test_vrms_payload();

  if (all_passed) {
    std::cout << "============================================" << std::endl;
    std::cout << "ALL 11 C++ TEST SUITES PASSED (100% SUCCESS)!" << std::endl;
    std::cout << "============================================" << std::endl;
    return 0;
  } else {
    std::cerr << "TEST SUITE FAILED!" << std::endl;
    return 1;
  }
}
