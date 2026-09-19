import json
import tempfile
import unittest
from pathlib import Path

from relayd import CommandStore, RelayError


class FakeClock:
    def __init__(self, value=1000.0):
        self.value = value

    def __call__(self):
        return self.value

    def advance(self, seconds):
        self.value += seconds


class RelayStoreTests(unittest.TestCase):
    def make_store(self, **kwargs):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        clock = FakeClock()
        store = CommandStore(Path(directory.name) / "state.sqlite3", clock=clock,
                             initial_id=1, **kwargs)
        self.addCleanup(store.close)
        return store, clock

    def test_lost_ack_releases_replayable_command_after_lease(self):
        store, clock = self.make_store(lease=10, ttl=100)
        command_id = store.enqueue("WOL")["id"]
        first = store.poll("esp32")["attempt"]
        self.assertEqual(first, 1)
        clock.advance(11)
        second = store.poll("esp32")["attempt"]
        self.assertEqual(second, 2)
        self.assertEqual(store.acknowledge(command_id, "esp32", True, second)["state"], "succeeded")

    def test_wrong_client_cannot_take_command(self):
        store, _ = self.make_store()
        store.enqueue("STATUS")
        with self.assertRaisesRegex(RelayError, "client=esp32"):
            store.poll("bridge")
        with self.assertRaisesRegex(RelayError, "client=esp32"):
            store.poll(None)
        self.assertEqual(store.poll("esp32")["cmd"], "STATUS")

    def test_pulse_is_not_replayed_after_lease_expiry(self):
        store, clock = self.make_store(lease=10, ttl=100)
        command_id = store.enqueue("PULSE")["id"]
        self.assertEqual(store.poll("esp32")["id"], command_id)
        clock.advance(11)
        self.assertIsNone(store.poll("esp32")["cmd"])
        self.assertEqual(store.status()["results"][0]["state"], "unknown")

    def test_state_and_sequence_survive_restart(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "state.sqlite3"
        clock = FakeClock()
        first = CommandStore(path, clock=clock, initial_id=41)
        self.addCleanup(first.close)
        self.assertEqual(first.enqueue("WOL")["id"], 41)
        first.close()
        second = CommandStore(path, clock=clock, initial_id=1)
        self.addCleanup(second.close)
        self.assertEqual(second.enqueue("STATUS")["id"], 42)
        self.assertEqual(second.poll("esp32")["id"], 41)

    def test_queue_full_returns_explicit_503_error(self):
        store, _ = self.make_store(capacity=1)
        store.enqueue("STATUS")
        with self.assertRaisesRegex(RelayError, "queue full") as raised:
            store.enqueue("WOL")
        self.assertEqual(raised.exception.status, 503)

    def test_failed_wol_has_bounded_retries(self):
        store, clock = self.make_store(lease=10, ttl=100, max_attempts=3)
        command_id = store.enqueue("WOL")["id"]
        for expected_attempt in (1, 2, 3):
            delivered = store.poll("esp32")
            self.assertEqual(delivered["attempt"], expected_attempt)
            result = store.acknowledge(command_id, "esp32", False, expected_attempt, "udp_failed")
            if expected_attempt < 3:
                self.assertEqual(result["state"], "pending")
            else:
                self.assertEqual(result["state"], "failed")
            if expected_attempt < 3:
                # The command is immediately available after a negative ACK.
                continue
        self.assertEqual(store.status()["results"][0]["detail"], "udp_failed")

    def test_duplicate_ack_is_idempotent_and_conflicts_are_rejected(self):
        store, _ = self.make_store()
        command_id = store.enqueue("STATUS")["id"]
        delivery = store.poll("esp32")
        first = store.acknowledge(command_id, "esp32", True, delivery["attempt"], "executed")
        duplicate = store.acknowledge(command_id, "esp32", True, delivery["attempt"], "executed")
        self.assertFalse(first["duplicate"])
        self.assertTrue(duplicate["duplicate"])
        with self.assertRaisesRegex(RelayError, "conflicting"):
            store.acknowledge(command_id, "esp32", False, delivery["attempt"], "wrong")


if __name__ == "__main__":
    unittest.main()
