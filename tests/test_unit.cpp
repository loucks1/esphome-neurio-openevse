#include <iostream>
#include <cassert>
#include <cmath>
#include <string>
#include <vector>

#include "../components/neurio_evse/neurio_math.h"
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

int main() {
  std::cout << "============================================" << std::endl;
  std::cout << "Starting Neurio EVSE Native Unit Test Suite" << std::endl;
  std::cout << "============================================" << std::endl;

  bool all_passed = true;
  all_passed &= test_energy_math();
  all_passed &= test_openevse_offset();
  all_passed &= test_ack_parser();
  all_passed &= test_outage_queue();

  if (all_passed) {
    std::cout << "============================================" << std::endl;
    std::cout << "ALL 4 C++ TEST SUITES PASSED (100% SUCCESS)!" << std::endl;
    std::cout << "============================================" << std::endl;
    return 0;
  } else {
    std::cerr << "TEST SUITE FAILED!" << std::endl;
    return 1;
  }
}
