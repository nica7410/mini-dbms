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
            with Server(directory, "--workers", "1", "--queue-timeout-ms", "60000",
                        process_options=control.process_options()) as server:
                with ThreadPoolExecutor(max_workers=4) as clients:
                    try:
                        scan = clients.submit(server.query, "SELECT * FROM users;")
                        control.wait_entered()
                        writes = [clients.submit(server.query,
                            f"INSERT INTO users (name, age) VALUES ('pending-{n}', 1);") for n in range(3)]
                        await_state(lambda: server.metrics()["pool"]["queued"] == 3)
                        state = server.metrics()["pool"]
                        self.assertEqual((state["active"], state["accepted"], state["completed"]), (1, 4, 0))
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

    def test_queue_expiry_while_worker_is_held(self):
        with tempfile.TemporaryDirectory(prefix="minidb-controlled-") as directory, ExecutorControl() as control:
            with Server(directory, "--workers", "1", "--queue-timeout-ms", "500",
                        process_options=control.process_options()) as server:
                with ThreadPoolExecutor(max_workers=2) as clients:
                    try:
                        scan = clients.submit(server.query, "SELECT * FROM users;")
                        control.wait_entered()
                        write = clients.submit(server.query, "INSERT INTO users (name, age) VALUES ('expired', 1);")
                        await_state(lambda: server.metrics()["pool"]["queued"] == 1)
                        # At this observation the task is already queued. Keep the
                        # worker held until its queue budget has definitely elapsed.
                        release_at = time.monotonic() + 0.55
                        await_state(lambda: time.monotonic() >= release_at)
                        control.release()
                        self.assertEqual(scan.result(timeout=10)[0], 200)
                        status, expired = write.result(timeout=10)
                        self.assertEqual((status, expired["error"]["code"]), (503, "QUEUE_TIMEOUT"))
                        self.assertEqual(expired["write_outcome"], "not_started")
                        self.assertEqual(server.query("SELECT * FROM users;")[1]["rows"], [])
                        self.assertEqual(server.metrics()["pool"]["expired"], 1)
                    finally:
                        control.release()


if __name__ == "__main__":
    unittest.main(verbosity=2)
