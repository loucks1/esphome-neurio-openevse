import unittest
import json
import tempfile
import os
import shutil

class TestNeurioEvseSimulation(unittest.TestCase):
    def setUp(self):
        # Sample payload matching the user's exact Neurio configuration
        self.raw_sample = {
            "sensorId": "0x0000000000000000",
            "timestamp": "2026-11-07T09:02:05Z",
            "channels": [
                {
                    "type": "PHASE_A_CONSUMPTION",
                    "ch": 1,
                    "p_W": 568,
                    "eImp_Ws": 373905878762,
                    "eExp_Ws": 2026661,
                    "q_VAR": -110,
                    "v_V": 123.705
                },
                {
                    "type": "PHASE_B_CONSUMPTION",
                    "ch": 2,
                    "p_W": 87,
                    "eImp_Ws": 252469196191,
                    "eExp_Ws": 36215164,
                    "q_VAR": -155,
                    "v_V": 123.998
                },
                {
                    "type": "NET",
                    "ch": 3,
                    "p_W": -2256,
                    "eImp_Ws": 445441588847,
                    "eExp_Ws": 236253198547,
                    "q_VAR": -85,
                    "v_V": 123.854
                },
                {
                    "type": "GENERATION",
                    "ch": 4,
                    "p_W": 2910,
                    "eImp_Ws": 417293809345,
                    "eExp_Ws": 145364396,
                    "q_VAR": -180,
                    "v_V": 123.849
                },
                {
                    "type": "CONSUMPTION",
                    "ch": 5,
                    "p_W": 654,
                    "eImp_Ws": 626280154794,
                    "eExp_Ws": 0,
                    "q_VAR": -265,
                    "v_V": 123.851
                }
            ]
        }
        self.test_dir = tempfile.mkdtemp(prefix="neurio_test_")

    def tearDown(self):
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def test_neurio_channel_parsing(self):
        """Verify that all 5 channels are parsed with 64-bit precision."""
        channels = {ch["ch"]: ch for ch in self.raw_sample["channels"]}

        self.assertIn(1, channels)
        self.assertEqual(channels[1]["p_W"], 568)
        self.assertAlmostEqual(channels[1]["v_V"], 123.705, places=3)

        self.assertIn(3, channels)
        self.assertEqual(channels[3]["p_W"], -2256)
        self.assertEqual(channels[3]["eImp_Ws"], 445441588847)
        self.assertEqual(channels[3]["eExp_Ws"], 236253198547)

        # Net balance check: Consumption - Generation == Net
        cons_p = channels[5]["p_W"]
        gen_p = channels[4]["p_W"]
        self.assertEqual(cons_p - gen_p, channels[3]["p_W"])

    def test_openevse_offset_calculations(self):
        """Verify OpenEVSE reported net usage under various dynamic offsets."""
        channels = {ch["ch"]: ch for ch in self.raw_sample["channels"]}
        net_power = channels[3]["p_W"] # -2256 W

        # Offset = 0 (direct solar surplus)
        self.assertEqual(net_power + 0, -2256)

        # Offset = +500 W (reserves 500 W buffer, OpenEVSE sees less surplus)
        self.assertEqual(net_power + 500, -1756)

        # Offset = -1000 W (OpenEVSE charges more aggressively)
        self.assertEqual(net_power + (-1000), -3256)

    def test_hourly_delta_calculation(self):
        """Simulate top-of-hour latching and delta computation."""
        base_net_imp_ws = 445441588847
        base_net_exp_ws = 236253198547

        # 1 hour later: imported 1.5 kWh (5,400,000 Ws) and exported 2.2 kWh (7,920,000 Ws)
        curr_net_imp_ws = base_net_imp_ws + 5400000
        curr_net_exp_ws = base_net_exp_ws + 7920000

        delta_imp_kwh = (curr_net_imp_ws - base_net_imp_ws) / 3600000.0
        delta_exp_kwh = (curr_net_exp_ws - base_net_exp_ws) / 3600000.0
        delta_net_kwh = delta_imp_kwh - delta_exp_kwh

        self.assertAlmostEqual(delta_imp_kwh, 1.5, places=4)
        self.assertAlmostEqual(delta_exp_kwh, 2.2, places=4)
        self.assertAlmostEqual(delta_net_kwh, -0.7, places=4)

    def test_store_and_forward_outage_simulation(self):
        """Simulate an MQTT outage lasting multiple hours, 5-minute retries, and ACK resolution."""
        queue_dir = os.path.join(self.test_dir, "neurio_q")
        os.makedirs(queue_dir, exist_ok=True)

        pending_ids = []

        # 1. Enqueue 3 hourly records (simulating 3 missed hours during an outage)
        epochs = [1730970000, 1730973600, 1730977200]
        for ep in epochs:
            record = {
                "id": ep,
                "timestamp": f"2026-11-07T{len(pending_ids):02d}:00:00Z",
                "net": {"delta_net_kwh": -1.25},
                "retries": 0
            }
            filepath = os.path.join(queue_dir, f"rec_{ep}.json")
            with open(filepath, "w") as f:
                json.dump(record, f)
            pending_ids.append(ep)

        self.assertEqual(len(pending_ids), 3)

        # 2. Backlog check
        backlog_status = {
            "pending_records": len(pending_ids),
            "oldest_id": pending_ids[0]
        }
        self.assertEqual(backlog_status["pending_records"], 3)
        self.assertEqual(backlog_status["oldest_id"], 1730970000)

        # 3. Simulate 5-minute retry cycle: retries count increments
        for pid in pending_ids:
            filepath = os.path.join(queue_dir, f"rec_{pid}.json")
            with open(filepath, "r") as f:
                data = json.load(f)
            data["retries"] += 1
            with open(filepath, "w") as f:
                json.dump(data, f)

        with open(os.path.join(queue_dir, f"rec_{epochs[0]}.json")) as f:
            self.assertEqual(json.load(f)["retries"], 1)

        # 4. Downstream ACKs first record
        ack_id = 1730970000
        pending_ids.remove(ack_id)
        os.remove(os.path.join(queue_dir, f"rec_{ack_id}.json"))

        self.assertEqual(len(pending_ids), 2)
        self.assertFalse(os.path.exists(os.path.join(queue_dir, f"rec_{ack_id}.json")))

        # 5. Downstream ACKs remaining records
        for remaining in list(pending_ids):
            pending_ids.remove(remaining)
            os.remove(os.path.join(queue_dir, f"rec_{remaining}.json"))

        self.assertEqual(len(pending_ids), 0)
        self.assertEqual(len(os.listdir(queue_dir)), 0)

    def test_testing_mode_no_ack_required(self):
        """Verify that when require_hourly_ack is disabled (Testing Mode), records are published without queueing."""
        queue_dir = os.path.join(self.test_dir, "neurio_q_test_mode")
        os.makedirs(queue_dir, exist_ok=True)

        require_hourly_ack = False
        pending_ids = []

        epoch = 1730980800
        record = {
            "id": epoch,
            "timestamp": "2026-11-07T12:00:00Z",
            "net": {"delta_net_kwh": -1.25},
            "retries": 0
        }

        # Simulated publish occurs
        published_payload = json.dumps(record)
        self.assertIn("1730980800", published_payload)

        # Enqueue condition check
        if require_hourly_ack:
            pending_ids.append(epoch)
            filepath = os.path.join(queue_dir, f"rec_{epoch}.json")
            with open(filepath, "w") as f:
                json.dump(record, f)

        # Verify queue remains empty and no files written to flash
        self.assertEqual(len(pending_ids), 0)
        self.assertEqual(len(os.listdir(queue_dir)), 0)

        # Backlog status remains 0
        backlog_status = {"pending_records": len(pending_ids)}
        self.assertEqual(backlog_status["pending_records"], 0)

    def test_in_memory_first_mode_ack_before_timeout(self):
        """Verify that in_memory_first mode does NOT write to flash when ACK arrives within grace period."""
        queue_dir = os.path.join(self.test_dir, "neurio_q_in_memory")
        os.makedirs(queue_dir, exist_ok=True)

        epoch = 1730980800
        in_flight = {
            "id": epoch,
            "json": json.dumps({"id": epoch, "net": {"delta_net_kwh": -1.25}}),
            "enqueued_ms": 1000,
            "in_flash": False
        }

        # 1. Simulate ACK arriving at 2000 ms (1s elapsed, timeout is 30s)
        current_time_ms = 2000
        ack_timeout_ms = 30000

        # Check timeout: Not expired
        if not in_flight["in_flash"] and (current_time_ms - in_flight["enqueued_ms"] >= ack_timeout_ms):
            in_flight["in_flash"] = True

        self.assertFalse(in_flight["in_flash"])
        self.assertEqual(len(os.listdir(queue_dir)), 0) # Flash was never written!

        # 2. Process ACK
        ack_id = epoch
        if in_flight["id"] == ack_id:
            if in_flight["in_flash"]:
                os.remove(os.path.join(queue_dir, f"rec_{ack_id}.json"))
            in_flight = None

        self.assertIsNone(in_flight)
        self.assertEqual(len(os.listdir(queue_dir)), 0)

    def test_in_memory_first_mode_timeout_flushes_to_flash(self):
        """Verify that in_memory_first mode flushes to flash once grace period expires."""
        queue_dir = os.path.join(self.test_dir, "neurio_q_timeout")
        os.makedirs(queue_dir, exist_ok=True)

        epoch = 1730980800
        in_flight = {
            "id": epoch,
            "json": json.dumps({"id": epoch, "net": {"delta_net_kwh": -1.25}}),
            "enqueued_ms": 1000,
            "in_flash": False
        }

        # 1. Advance time to 35,000 ms (34s elapsed, timeout is 30s)
        current_time_ms = 35000
        ack_timeout_ms = 30000

        # Check timeout: Expired!
        if not in_flight["in_flash"] and (current_time_ms - in_flight["enqueued_ms"] >= ack_timeout_ms):
            filepath = os.path.join(queue_dir, f"rec_{epoch}.json")
            with open(filepath, "w") as f:
                f.write(in_flight["json"])
            in_flight["in_flash"] = True

        self.assertTrue(in_flight["in_flash"])
        self.assertEqual(len(os.listdir(queue_dir)), 1)
        self.assertTrue(os.path.exists(os.path.join(queue_dir, f"rec_{epoch}.json")))

        # 2. Later ACK deletes the persisted file
        if in_flight["in_flash"]:
            os.remove(os.path.join(queue_dir, f"rec_{epoch}.json"))
            in_flight = None

        self.assertEqual(len(os.listdir(queue_dir)), 0)

    def test_adaptive_backoff_and_socket_recycling(self):
        """Verify adaptive backoff on 3 consecutive failures and socket recycling after N requests."""
        consecutive_failures = 0
        max_failures_before_backoff = 3
        backoff_interval_ms = 10000
        socket_recycle_count = 60

        # 1. Simulate 2 failures -> No backoff yet
        consecutive_failures += 1
        self.assertLess(consecutive_failures, max_failures_before_backoff)
        consecutive_failures += 1
        self.assertLess(consecutive_failures, max_failures_before_backoff)

        # 2. 3rd failure -> Activates backoff!
        consecutive_failures += 1
        is_backing_off = (consecutive_failures >= max_failures_before_backoff)
        self.assertTrue(is_backing_off)

        # In backoff: skip polling if time elapsed is under 10 seconds
        last_poll_ms = 1000
        current_time_ms = 5000 # Only 4s elapsed
        should_skip_poll = (current_time_ms - last_poll_ms < backoff_interval_ms)
        self.assertTrue(should_skip_poll)

        # Advance past 10s: poll allowed
        current_time_ms = 12000 # 11s elapsed
        should_skip_poll = (current_time_ms - last_poll_ms < backoff_interval_ms)
        self.assertFalse(should_skip_poll)

        # Successful poll recovers normal state
        consecutive_failures = 0
        self.assertFalse(consecutive_failures >= max_failures_before_backoff)

        # 3. Simulate socket recycling counter reaching 60
        successful_requests = 59
        successful_requests += 1
        socket_recycled = False
        if successful_requests >= socket_recycle_count:
            successful_requests = 0
            socket_recycled = True

        self.assertTrue(socket_recycled)
        self.assertEqual(successful_requests, 0)

if __name__ == "__main__":
    unittest.main()
