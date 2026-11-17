"""Black-box HTTP tests. Uses only Python's standard library and temporary data."""

from concurrent.futures import ThreadPoolExecutor
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest


BINARY = str(Path(sys.argv.pop(1)).resolve())


def unused_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def fixture(directory, count):
    """An append-only TSV fixture; only the last row has age=150."""
    with (Path(directory) / "users.tsv").open("w", encoding="utf-8", newline="\n") as output:
        for number in range(1, count + 1):
            output.write(f"{number}\tuser-{number}\t{150 if number == count else number % 150}\n")


class Server:
    def __init__(self, directory, *extra, process_options=None):
        self.directory = str(directory)
        self.port = unused_port()
        self.log = tempfile.TemporaryFile(mode="w+b")
        self.process = subprocess.Popen(
            [BINARY, "--data-dir", self.directory, "--port", str(self.port), *extra],
            stdout=self.log, stderr=subprocess.STDOUT,
            **(process_options or {}),
        )
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise RuntimeError("server failed to start: " + self.logs())
            try:
                if self.request("GET", "/health")[0] == 200:
                    return
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(0.025)
        self.process.kill()
        self.process.wait()
        raise RuntimeError("server readiness timed out: " + self.logs())

    def logs(self):
        self.log.seek(0)
        return self.log.read().decode("utf-8", errors="replace")

    def request(self, method, path, body=None, content_type="application/json"):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=15)
        try:
            headers = {"Connection": "close"}
            if body is not None:
                headers["Content-Type"] = content_type
            connection.request(method, path, body=body, headers=headers)
            response = connection.getresponse()
            payload = response.read()
            return response.status, json.loads(payload.decode("utf-8"))
        finally:
            connection.close()

    def query(self, sql):
        return self.request("POST", "/query", json.dumps({"sql": sql}, ensure_ascii=False).encode("utf-8"))

    def metrics(self):
        return self.request("GET", "/metrics")[1]

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
                raise AssertionError("shutdown did not finish: " + self.logs())
        if os.name != "nt" and self.process.returncode != 0:
            raise AssertionError("server exited abnormally: " + self.logs())
        self.log.close()

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        self.close()


class ApiTest(unittest.TestCase):
    def test_insert_read_and_restart(self):
        with tempfile.TemporaryDirectory(prefix="minidb-api-") as directory:
            with Server(directory) as server:
                status, empty = server.query("SELECT * FROM users;")
                self.assertEqual((status, empty["rows"]), (200, []))
                status, inserted = server.query("INSERT INTO users (name, age) VALUES ('한글 O''Brien', 25);")
                self.assertEqual(status, 200)
                self.assertEqual(inserted["inserted_id"], "1")
                self.assertEqual(inserted["write_outcome"], "applied")
                status, selected = server.query("SELECT * FROM users WHERE id = 1;")
                self.assertEqual(status, 200)
                self.assertEqual(selected["rows"], [{"id": "1", "name": "한글 O'Brien", "age": 25}])
                status, all_rows = server.query("SELECT * FROM users;")
                self.assertEqual((status, all_rows["row_count"]), (200, 1))
                self.assertEqual(server.query("SELECT * FROM users WHERE id = 999;")[1]["rows"], [])
            with Server(directory) as restarted:
                self.assertEqual(restarted.query("SELECT * FROM users WHERE id = 1;")[1]["row_count"], 1)
                self.assertEqual(restarted.query("SELECT * FROM users WHERE id = 999;")[1]["rows"], [])
                status, inserted = restarted.query("INSERT INTO users (name, age) VALUES ('next', 26);")
                self.assertEqual((status, inserted["inserted_id"]), (200, "2"))

    def test_multi_row_insert_response(self):
        with tempfile.TemporaryDirectory(prefix="minidb-api-") as directory:
            with Server(directory) as server:
                # A single-row response keeps its previous fields and values.
                status, single = server.query("INSERT INTO users (name, age) VALUES ('one', 1);")
                self.assertEqual(status, 200)
                self.assertEqual((single["inserted_id"], single["last_inserted_id"], single["affected_rows"],
                                  single["write_outcome"]), ("1", "1", 1, "applied"))
                status, batch = server.query(
                    "INSERT INTO users (age, name) VALUES (2, 'a'), (3, 'b'), (4, 'O''Brien');")
                self.assertEqual(status, 200)
                self.assertEqual((batch["inserted_id"], batch["last_inserted_id"], batch["affected_rows"],
                                  batch["write_outcome"]), ("2", "4", 3, "applied"))
                status, selected = server.query("SELECT * FROM users WHERE id = 4;")
                self.assertEqual(selected["rows"], [{"id": "4", "name": "O'Brien", "age": 4}])

                status, invalid = server.query("INSERT INTO users (name, age) VALUES ('x', 1), ('y', 151);")
                self.assertEqual((status, invalid["error"]["code"]), (422, "INVALID_VALUES"))
                too_many = "INSERT INTO users (name, age) VALUES " + ", ".join(["('x', 1)"] * 1001) + ";"
                status, rejected = server.query(too_many)
                self.assertEqual((status, rejected["error"]["code"]), (400, "INVALID_SQL"))
                self.assertEqual(server.query("SELECT * FROM users;")[1]["row_count"], 4)

    def test_future_id_becomes_visible_after_insert(self):
        with tempfile.TemporaryDirectory(prefix="minidb-api-") as directory:
            with Server(directory) as server:
                self.assertEqual(server.query("INSERT INTO users (name, age) VALUES ('first', 1);")[0], 200)
                self.assertEqual(server.query("SELECT * FROM users WHERE id = 2;")[1]["rows"], [])
                status, inserted = server.query("INSERT INTO users (name, age) VALUES ('second', 2);")
                self.assertEqual((status, inserted["inserted_id"]), (200, "2"))
                status, selected = server.query("SELECT * FROM users WHERE id = 2;")
                self.assertEqual((status, selected["rows"]), (200, [{"id": "2", "name": "second", "age": 2}]))
                self.assertNotIn("fallbacks", server.metrics()["engine"])
            with Server(directory) as restarted:
                self.assertEqual(restarted.query("SELECT * FROM users WHERE id = 2;")[1]["row_count"], 1)
                self.assertEqual(restarted.query("SELECT * FROM users WHERE id = 3;")[1]["rows"], [])

    def test_lock_wait_breakdown(self):
        with tempfile.TemporaryDirectory(prefix="minidb-api-") as directory:
            fixture(directory, 1)
            with Server(directory) as server:
                for sql in ("SELECT * FROM users WHERE id = 1;",
                            "INSERT INTO users (name, age) VALUES ('new', 2);",
                            "SELECT * FROM users WHERE id = 2;"):
                    self.assertEqual(server.query(sql)[0], 200)
                engine = server.metrics()["engine"]
                waits = engine["lock_wait"]
                expected = {"reader_mutex": 2, "read_gate": 2, "write_gate": 1}
                self.assertEqual(set(waits), set(expected))
                for name, count in expected.items():
                    self.assertEqual(set(waits[name]), {"count", "total_us", "max_us"})
                    self.assertEqual(waits[name]["count"], count)
                    self.assertLessEqual(waits[name]["max_us"], waits[name]["total_us"])
                self.assertEqual(sum(v["total_us"] for v in waits.values()), engine["lock_wait_us"])

    def test_request_kind_queue_metrics(self):
        with tempfile.TemporaryDirectory(prefix="minidb-api-") as directory:
            fixture(directory, 1)
            with Server(directory) as server:
                for sql in ("SELECT * FROM users WHERE id = 1;",
                            "INSERT INTO users (name, age) VALUES ('new', 2);",
                            "INSERT INTO users (name, age) VALUES ('a', 1), ('b', 2);",
                            "SELECT * FROM users;",
                            "SELEC broken;"):
                    server.query(sql)
                metrics = server.metrics()
                kinds = metrics["request_kinds"]
                self.assertEqual({name: kinds[name]["count"] for name in kinds},
                                 {"insert": 2, "select": 2, "other": 1})
                for detail in kinds.values():
                    self.assertEqual(set(detail), {"count", "queue_wait_us", "max_queue_wait_us", "total_us"})
                    self.assertLessEqual(detail["max_queue_wait_us"], detail["queue_wait_us"])
                    self.assertLessEqual(detail["queue_wait_us"], detail["total_us"])
                self.assertEqual(sum(d["count"] for d in kinds.values()), metrics["pool"]["completed"])

    def test_invalid_input_has_no_insert_side_effect(self):
        with tempfile.TemporaryDirectory(prefix="minidb-api-") as directory, Server(directory) as server:
            insert = "INSERT INTO users (name, age) VALUES ('must-not-exist', 25);"
            for sql in (insert + " SELECT * FROM users;", insert + " bad", insert + "\x00ignored", "", "SELECT"):
                with self.subTest(sql=repr(sql)):
                    status, error = server.query(sql)
                    self.assertEqual(status, 400)
                    self.assertFalse(error["ok"])
            for sql in (
                "INSERT INTO users (name, age) VALUES ('', 25);",
                "INSERT INTO users (name, age) VALUES ('bad', 151);",
                "INSERT INTO users (name, age) VALUES ('" + "가" * 86 + "', 25);",
            ):
                self.assertEqual(server.query(sql)[0], 422)
            bad_bodies = (
                b'{}', b'{"sql": 1}', b'{"sql":"SELECT * FROM users;","extra":true}',
                b'{"sql":"SELECT * FROM users;","sql":"SELECT * FROM users;"}',
                b'{"sql":"SELECT * FROM users;"} trailing', b'{"sql":"\xff"}',
                b'{"sql":' + b'[' * 40 + b'0' + b']' * 40 + b'}',
            )
            for body in bad_bodies:
                with self.subTest(body=body):
                    self.assertEqual(server.request("POST", "/query", body)[0], 400)
            self.assertEqual(server.request("POST", "/query", b"SELECT", "text/plain")[0], 415)
            self.assertEqual(server.request("GET", "/unknown")[0], 404)
            self.assertNotEqual(server.request("GET", "/query")[0], 200)
            self.assertEqual(server.query("SELECT * FROM users;")[1]["rows"], [])

    def test_parallel_reads_and_mixed_writes(self):
        rows = int(os.environ.get("MINIDB_API_TEST_ROWS", "100000"))
        self.assertGreater(rows, 0)
        with tempfile.TemporaryDirectory(prefix="minidb-api-") as directory:
            fixture(directory, rows)
            with Server(directory, "--workers", "4") as server:
                barrier = threading.Barrier(8)

                def scan(_):
                    barrier.wait(timeout=10)
                    return server.query("SELECT * FROM users WHERE age = 150;")

                with ThreadPoolExecutor(max_workers=8) as clients:
                    results = list(clients.map(scan, range(8)))
                for status, selected in results:
                    self.assertEqual(status, 200)
                    self.assertEqual(selected["rows"], [{"id": str(rows), "name": f"user-{rows}", "age": 150}])
                self.assertGreaterEqual(server.metrics()["engine"]["peak_executions"], 2)

                def insert_then_read(number):
                    name = f"parallel-{number}"
                    status, inserted = server.query(f"INSERT INTO users (name, age) VALUES ('{name}', 31);")
                    self.assertEqual(status, 200)
                    row_id = inserted["inserted_id"]
                    status, selected = server.query(f"SELECT * FROM users WHERE id = {row_id};")
                    self.assertEqual(status, 200)
                    self.assertEqual(selected["rows"], [{"id": row_id, "name": name, "age": 31}])
                    return int(row_id)

                with ThreadPoolExecutor(max_workers=12) as clients:
                    ids = list(clients.map(insert_then_read, range(64)))
                self.assertEqual(sorted(ids), list(range(rows + 1, rows + 65)))
                self.assertTrue(server.request("GET", "/health")[1]["ok"])
                self.assertEqual(server.metrics()["pool"]["failed_tasks"], 0)
                if "MINIDB_API_TEST_ROWS" in os.environ:
                    print(json.dumps({"fixture_rows": rows, "metrics": server.metrics()}), flush=True)

    def test_limits_and_rejection(self):
        with tempfile.TemporaryDirectory(prefix="minidb-api-") as directory:
            fixture(directory, 3)
            with Server(directory, "--max-rows", "2", "--max-sql-bytes", "128") as server:
                status, too_many = server.query("SELECT * FROM users;")
                self.assertEqual((status, too_many["error"]["code"]), (422, "RESULT_TOO_LARGE"))
                self.assertNotIn("rows", too_many)
                self.assertEqual(server.query("SELECT * FROM users WHERE id = 1;")[0], 200)
                self.assertEqual(server.query(" " * 129)[0], 413)
                self.assertTrue(server.request("GET", "/health")[1]["ok"])
            with Server(directory, "--queue-bytes", "16") as server:
                status, rejected = server.query("INSERT INTO users (name, age) VALUES ('rejected', 25);")
                self.assertEqual((status, rejected["error"]["code"]), (503, "QUEUE_FULL"))
                self.assertEqual(rejected["write_outcome"], "not_started")
                self.assertEqual(server.metrics()["engine"]["requests"], 0)
            self.assertEqual(len((Path(directory) / "users.tsv").read_text().splitlines()), 3)

    def test_same_directory_cannot_be_opened_by_two_servers(self):
        with tempfile.TemporaryDirectory(prefix="minidb-api-") as directory, Server(directory):
            second = subprocess.run(
                [BINARY, "--data-dir", directory, "--port", str(unused_port())],
                capture_output=True, timeout=10,
            )
            self.assertNotEqual(second.returncode, 0)
            self.assertIn(b"database", second.stderr)

    @unittest.skipIf(os.name == "nt", "POSIX SIGTERM drain; Windows terminate forcibly kills")
    def test_shutdown_drains_accepted_inserts(self):
        with tempfile.TemporaryDirectory(prefix="minidb-api-") as directory:
            fixture(directory, 300000)
            with Server(directory, "--workers", "1", "--queue-timeout-ms", "60000") as server:
                with ThreadPoolExecutor(max_workers=11) as clients:
                    work = [clients.submit(server.query, "SELECT * FROM users WHERE age = 150;")]
                    work += [clients.submit(server.query,
                        f"INSERT INTO users (name, age) VALUES ('drain-{number}', 40);") for number in range(10)]
                    deadline = time.monotonic() + 10
                    while server.metrics()["pool"]["accepted"] < 11:
                        self.assertLess(time.monotonic(), deadline)
                        time.sleep(0.005)
                    server.process.send_signal(signal.SIGTERM)
                    for future in work:
                        try:
                            status, result = future.result(timeout=15)
                            self.assertEqual(status, 200, result)
                        except (OSError, http.client.HTTPException):
                            # Persisted SQL completion is guaranteed; socket delivery
                            # is not a transaction acknowledgement across disconnects.
                            pass
                    self.assertEqual(server.process.wait(timeout=15), 0)
            with (Path(directory) / "users.tsv").open(encoding="utf-8") as data:
                tail = [line for line in data if "\tdrain-" in line]
            self.assertEqual(len(tail), 10)
            self.assertEqual({int(line.split("\t", 1)[0]) for line in tail}, set(range(300001, 300011)))

    def test_int64_id_and_invalid_stored_utf8(self):
        with tempfile.TemporaryDirectory(prefix="minidb-api-") as directory:
            path = Path(directory) / "users.tsv"
            path.write_bytes(b"9007199254740993\tprecise\t25\n")
            with Server(directory) as server:
                status, selected = server.query("SELECT * FROM users WHERE id = 9007199254740993;")
                self.assertEqual((status, selected["rows"][0]["id"]), (200, "9007199254740993"))
            path.write_bytes(b"1\t\xff\t25\n")
            with Server(directory) as server:
                status, error = server.query("SELECT * FROM users WHERE id = 1;")
                self.assertEqual((status, error["error"]["code"]), (500, "INVALID_STORED_TEXT"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
