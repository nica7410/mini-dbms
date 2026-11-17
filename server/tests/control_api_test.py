"""POSIX integration tests against the test-only linker-wrapped server."""
from concurrent.futures import ThreadPoolExecutor
import http.client
import os
import select
import signal
import tempfile
import time
import unittest

# Reuses the ordinary harness; this import consumes the server binary argument.
from api_test import Server


def await_state(predicate):
    deadline = time.monotonic() + 5
    while not predicate():
        if time.monotonic() >= deadline:
            raise AssertionError("state observation deadline exceeded")
        time.sleep(0.002)


class ExecutorControl:
    def __init__(self):
        self.enter_read, self.enter_write = os.pipe()
        self.release_read, self.release_write = os.pipe()
        self.released = False

    def process_options(self):
        env = dict(os.environ)
        env["MINIDB_TEST_ENTER_FD"] = str(self.enter_write)
        env["MINIDB_TEST_RELEASE_FD"] = str(self.release_read)
        return {"env": env, "pass_fds": (self.enter_write, self.release_read)}

    def wait_entered(self):
        if not select.select([self.enter_read], [], [], 5)[0]:
            raise AssertionError("executor did not reach the control point")
        if os.read(self.enter_read, 1) != b"x":
            raise AssertionError("executor handshake failed")

    def release(self):
        if not self.released:
            self.released = True
            os.write(self.release_write, b"x")

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.release()
        for fd in (self.enter_read, self.enter_write, self.release_read, self.release_write):
            os.close(fd)


class ControlledApiTest(unittest.TestCase):
    def test_sigterm_drains_a_confirmed_pending_queue(self):
        with tempfile.TemporaryDirectory(prefix="minidb-controlled-") as directory, ExecutorControl() as control:
            with Server(directory, "--workers", "1", "--write-workers", "1", "--queue-timeout-ms", "60000",
                        process_options=control.process_options()) as server:
                with ThreadPoolExecutor(max_workers=4) as clients:
                    try:
                        scan = clients.submit(server.query, "SELECT * FROM users;")
                        control.wait_entered()
                        writes = [clients.submit(server.query,
                            f"INSERT INTO users (name, age) VALUES ('pending-{n}', 1);") for n in range(3)]
                        # One INSERT waits for the write lock held off by the scan;
                        # the other two wait in the write queue.
                        await_state(lambda: server.metrics()["pools"]["write"]["queued"] == 2)
                        metrics = server.metrics()
                        write = metrics["pools"]["write"]
                        self.assertEqual((write["active"], write["accepted"], write["completed"]), (1, 3, 0))
                        state = metrics["pool"]
                        self.assertEqual((state["active"], state["accepted"], state["completed"]), (2, 4, 0))
                        server.process.send_signal(signal.SIGTERM)
                        # The blocked SQL cannot finish before this observation.
                        await_state(lambda: server.request("GET", "/health")[0] == 503)
                        status, rejected = server.query("INSERT INTO users (name, age) VALUES ('rejected', 1);")
                        self.assertEqual((status, rejected["error"]["code"]), (503, "SHUTTING_DOWN"))
                        self.assertEqual(rejected["write_outcome"], "not_started")
                        control.release()
                        for future in [scan, *writes]:
                            try:
                                status, body = future.result(timeout=10)
                                self.assertEqual(status, 200, body)
                            except (OSError, http.client.HTTPException):
                                # Persistence, not final socket delivery, is the contract.
                                pass
                        self.assertEqual(server.process.wait(timeout=10), 0)
                    finally:
                        control.release()
            with Server(directory) as restarted:
                status, result = restarted.query("SELECT * FROM users;")
                self.assertEqual(status, 200)
                self.assertEqual({row["name"] for row in result["rows"]}, {f"pending-{n}" for n in range(3)})
                self.assertEqual({row["id"] for row in result["rows"]}, {"1", "2", "3"})

    def test_insert_expiry_at_the_write_lock_and_in_the_queue(self):
        with tempfile.TemporaryDirectory(prefix="minidb-controlled-") as directory, ExecutorControl() as control:
            with Server(directory, "--workers", "1", "--write-workers", "1", "--queue-timeout-ms", "500",
                        process_options=control.process_options()) as server:
                with ThreadPoolExecutor(max_workers=3) as clients:
                    try:
                        scan = clients.submit(server.query, "SELECT * FROM users;")
                        control.wait_entered()
                        # The first INSERT leaves the queue at once and waits for the
                        # write lock; the second waits in the write queue behind it.
                        at_lock = clients.submit(server.query, "INSERT INTO users (name, age) VALUES ('at-lock', 1);")
                        await_state(lambda: server.metrics()["pools"]["write"]["active"] == 1)
                        queued = clients.submit(server.query, "INSERT INTO users (name, age) VALUES ('queued', 1);")
                        await_state(lambda: server.metrics()["pools"]["write"]["queued"] == 1)
                        # Both are accepted by now. Hold the scan past both budgets.
                        release_at = time.monotonic() + 0.55
                        await_state(lambda: time.monotonic() >= release_at)
                        control.release()
                        self.assertEqual(scan.result(timeout=10)[0], 200)
                        for future in (at_lock, queued):
                            status, expired = future.result(timeout=10)
                            self.assertEqual((status, expired["error"]["code"]), (503, "QUEUE_TIMEOUT"))
                            self.assertEqual(expired["write_outcome"], "not_started")
                        self.assertEqual(server.query("SELECT * FROM users;")[1]["rows"], [])
                        metrics = server.metrics()
                        self.assertEqual((metrics["pools"]["write"]["expired"], metrics["pool"]["expired"]), (2, 2))
                        # The lock was acquired once; the queued task never reached it.
                        self.assertEqual(metrics["engine"]["lock_wait"]["write_gate"]["count"], 1)
                    finally:
                        control.release()

    def test_insert_skips_queued_scans(self):
        with tempfile.TemporaryDirectory(prefix="minidb-controlled-") as directory, ExecutorControl() as control:
            with Server(directory, "--workers", "1", "--write-workers", "1", "--queue-timeout-ms", "60000",
                        process_options=control.process_options()) as server:
                with ThreadPoolExecutor(max_workers=4) as clients:
                    try:
                        held = clients.submit(server.query, "SELECT * FROM users;")
                        control.wait_entered()
                        queued = [clients.submit(server.query, "SELECT * FROM users;") for _ in range(2)]
                        await_state(lambda: server.metrics()["pools"]["read"]["queued"] == 2)
                        write = clients.submit(server.query, "INSERT INTO users (name, age) VALUES ('first', 1);")
                        await_state(lambda: server.metrics()["pools"]["write"]["active"] == 1)
                        # Give the write worker time to parse and block on the write
                        # lock; the writer-preferred lock then admits it before any
                        # queued scan reaches the lock.
                        settle_at = time.monotonic() + 0.2
                        await_state(lambda: time.monotonic() >= settle_at)
                        self.assertEqual(server.metrics()["pools"]["read"]["queued"], 2)
                        control.release()
                        status, held_result = held.result(timeout=10)
                        self.assertEqual((status, held_result["rows"]), (200, []))
                        self.assertEqual(write.result(timeout=10)[0], 200)
                        for future in queued:
                            status, result = future.result(timeout=10)
                            self.assertEqual(status, 200)
                            self.assertEqual([row["name"] for row in result["rows"]], ["first"])
                    finally:
                        control.release()

    def test_full_write_queue_does_not_reject_selects(self):
        with tempfile.TemporaryDirectory(prefix="minidb-controlled-") as directory, ExecutorControl() as control:
            with Server(directory, "--workers", "2", "--write-workers", "1", "--queue-capacity", "1",
                        "--queue-timeout-ms", "60000", process_options=control.process_options()) as server:
                with ThreadPoolExecutor(max_workers=5) as clients:
                    try:
                        scan = clients.submit(server.query, "SELECT * FROM users;")
                        control.wait_entered()
                        writes = [clients.submit(server.query, "INSERT INTO users (name, age) VALUES ('w0', 1);")]
                        await_state(lambda: server.metrics()["pools"]["write"]["active"] == 1)
                        writes.append(clients.submit(server.query, "INSERT INTO users (name, age) VALUES ('w1', 1);"))
                        await_state(lambda: server.metrics()["pools"]["write"]["queued"] == 1)
                        status, rejected = server.query("INSERT INTO users (name, age) VALUES ('rejected', 1);")
                        self.assertEqual((status, rejected["error"]["code"]), (503, "QUEUE_FULL"))
                        self.assertEqual(rejected["write_outcome"], "not_started")
                        # The write queue is full, yet a SELECT is still accepted.
                        lookup = clients.submit(server.query, "SELECT * FROM users WHERE id = 1;")
                        await_state(lambda: server.metrics()["pools"]["read"]["accepted"] == 2)
                        control.release()
                        self.assertEqual(scan.result(timeout=10)[0], 200)
                        for future in writes:
                            self.assertEqual(future.result(timeout=10)[0], 200)
                        self.assertEqual(lookup.result(timeout=10)[0], 200)
                        pools = server.metrics()["pools"]
                        self.assertEqual((pools["write"]["rejected"], pools["read"]["rejected"]), (1, 0))
                        names = {row["name"] for row in server.query("SELECT * FROM users;")[1]["rows"]}
                        self.assertEqual(names, {"w0", "w1"})
                    finally:
                        control.release()


if __name__ == "__main__":
    unittest.main(verbosity=2)
