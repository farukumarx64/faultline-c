"""Exercise the real binaries with independent Python TCP peers (stdlib only)."""

import argparse
import concurrent.futures
from contextlib import ExitStack
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest


PING = bytes.fromhex("46 4c 49 4e 00 01 00 01 00 00 00 00")
PONG = bytes.fromhex("46 4c 49 4e 00 01 00 02 00 00 00 00")
REGISTER = bytes.fromhex("46 4c 49 4e 00 01 00 03 00 00 00 00")
REGISTER_ACK_HEADER = bytes.fromhex("46 4c 49 4e 00 01 00 04 00 00 00 04")
HEARTBEAT_HEADER = bytes.fromhex("46 4c 49 4e 00 01 00 05 00 00 00 04")
# Exercise field boundaries and splits inside each field, in both directions.
FRAGMENT_SIZES = [(1,) * 12, (3, 5, 4)] + [(cut, 12 - cut) for cut in range(1, 12)]
BIN_DIR = Path("build/debug")
TEST_PORT = None


def receive_exact(connection, size):
    result = bytearray()
    while len(result) < size:
        chunk = connection.recv(size - len(result))
        if not chunk:
            raise AssertionError(f"EOF after {len(result)} of {size} bytes")
        result.extend(chunk)
    return bytes(result)


def heartbeat(worker_id):
    return HEARTBEAT_HEADER + struct.pack("!I", worker_id)


class CoordinatorTestCase(unittest.TestCase):
    COORDINATOR_ARGUMENTS = ()

    @classmethod
    def setUpClass(cls):
        cls.cli = str((BIN_DIR / "faultline").resolve())
        cls.coordinator = str((BIN_DIR / "faultline-coordinator").resolve())
        cls.log = tempfile.TemporaryFile(mode="w+")
        cls.addClassCleanup(cls.log.close)
        if TEST_PORT is None:
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                cls.port = reservation.getsockname()[1]
        else:
            cls.port = TEST_PORT
        # Each fixture owns a new durable store. Port 9000 still tests the default endpoint.
        cls.storage = tempfile.TemporaryDirectory(prefix="faultline-integration-")
        cls.addClassCleanup(cls.storage.cleanup)
        cls.wal = str(Path(cls.storage.name) / "state.wal")
        command = [cls.coordinator, "--wal", cls.wal, "--init-wal"]
        if cls.port != 9000:
            command.extend(["--port", str(cls.port)])
        command.extend(cls.COORDINATOR_ARGUMENTS)
        cls.server = subprocess.Popen(
            command,
            stdout=cls.log, stderr=cls.log,
        )
        cls.addClassCleanup(cls.stop_server)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if cls.server.poll() is not None:
                cls.log.seek(0)
                raise AssertionError(f"Coordinator exited:\n{cls.log.read()}")
            try:
                with socket.create_connection(("127.0.0.1", cls.port), timeout=0.2):
                    return
            except OSError:
                time.sleep(0.02)
        raise AssertionError("Coordinator did not become ready in 5 seconds")

    @classmethod
    def stop_server(cls):
        if cls.server.poll() is None:
            cls.server.send_signal(signal.SIGTERM)
        try:
            code = cls.server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            cls.server.kill()
            cls.server.wait()
            raise AssertionError("Coordinator did not stop on SIGTERM")
        cls.log.seek(0)
        output = cls.log.read()
        if (code != 0 or "ERROR: AddressSanitizer" in output or
                "runtime error:" in output or "[ERROR]" in output):
            raise AssertionError(f"Coordinator failed ({code}):\n{output}")

    def connect(self):
        return socket.create_connection(("127.0.0.1", self.port), timeout=2)

    def run_cli(self, port=None):
        return subprocess.run(
            [self.cli, "ping", "--coordinator", f"127.0.0.1:{port or self.port}"],
            capture_output=True, text=True, timeout=8,
        )

    def assert_pong(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "PONG\n")
        self.assertEqual(result.stderr, "")

    def assert_closed(self, connection):
        try:
            self.assertEqual(connection.recv(1), b"")
        except ConnectionResetError:
            pass

    def assert_waiting_for_more(self, connection):
        previous_timeout = connection.gettimeout()
        connection.settimeout(0.05)
        try:
            # Data, EOF, or reset all fail: an incomplete frame must stay pending.
            with self.assertRaises(socket.timeout, msg="Peer replied or closed while input was pending"):
                connection.recv(1)
        finally:
            connection.settimeout(previous_timeout)

    def send_fragments(self, connection, frame, sizes):
        self.assertEqual(sum(sizes), len(frame))
        offset = 0
        for size in sizes:
            connection.sendall(frame[offset:offset + size])
            offset += size
            if offset < len(frame):
                # Withhold the suffix; separate send calls alone can coalesce.
                self.assert_waiting_for_more(connection)

    def receive_registration_ack(self, connection):
        reply = receive_exact(connection, 16)
        self.assertEqual(reply[:12], REGISTER_ACK_HEADER)
        worker_id = struct.unpack("!I", reply[12:])[0]
        self.assertGreater(worker_id, 0)
        return worker_id

    def register_worker(self, connection):
        connection.sendall(REGISTER)
        return self.receive_registration_ack(connection)

    def worker_events(self, event, worker_id):
        # pread leaves the file offset shared with the child's stdout untouched.
        output = os.pread(self.log.fileno(), os.fstat(self.log.fileno()).st_size, 0).decode()
        pattern = rf"\b{event} worker_id={worker_id}\b([^\n]*)\n"
        return [dict(re.findall(r"(\w+)=([^ ]+)", fields))
                for fields in re.findall(pattern, output)]

    def wait_for_worker_event(self, event, worker_id):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            events = self.worker_events(event, worker_id)
            if events:
                return events[-1]
            time.sleep(0.01)
        self.fail(f"Missing {event} for worker {worker_id}")

    def send_heartbeat_and_ping(self, connection, worker_id):
        # The following PONG proves the preceding heartbeat was fully processed.
        connection.sendall(heartbeat(worker_id) + PING)
        self.assertEqual(receive_exact(connection, 12), PONG)

    def fake_peer(self, reply):
        """Run the CLI against a peer with deliberately controlled replies."""
        errors = []
        finished = threading.Event()
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(8)
            port = listener.getsockname()[1]

            def serve():
                try:
                    with listener.accept()[0] as connection:
                        connection.settimeout(7)
                        if receive_exact(connection, 12) != PING:
                            raise AssertionError("CLI did not send the exact PING bytes")
                        reply(connection, finished)
                except Exception as error:
                    errors.append(error)

            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            try:
                result = self.run_cli(port)
            finally:
                finished.set()
                thread.join(timeout=8)
            self.assertFalse(thread.is_alive(), "Fake peer did not finish")
            if errors:
                raise errors[0]
            return result

class CoordinatorTests(CoordinatorTestCase):
    def test_cli_exchange_and_sequential_clients(self):
        for _ in range(3):
            self.assert_pong(self.run_cli())

    def test_cli_default_endpoint(self):
        if self.port != 9000:
            self.skipTest("Run with --port 9000 to exercise the default CLI endpoint")
        result = subprocess.run(
            [self.cli, "ping"], capture_output=True, text=True, timeout=8,
        )
        self.assert_pong(result)

    def test_fragmented_ping(self):
        with self.connect() as connection:
            for sizes in FRAGMENT_SIZES:
                with self.subTest(sizes=sizes):
                    self.send_fragments(connection, PING, sizes)
                    self.assertEqual(receive_exact(connection, 12), PONG)

    def test_coalesced_and_repeated_frames(self):
        with self.connect() as connection:
            connection.sendall(PING * 3)
            self.assertEqual(receive_exact(connection, 36), PONG * 3)
            connection.sendall(PING)
            self.assertEqual(receive_exact(connection, 12), PONG)

    def test_complete_frame_followed_by_partial_frame(self):
        with self.connect() as connection:
            connection.sendall(PING + PING[:5])
            self.assertEqual(receive_exact(connection, 12), PONG)
            self.assert_waiting_for_more(connection)
            self.send_fragments(connection, PING[5:], (3, 4))
            self.assertEqual(receive_exact(connection, 12), PONG)
            connection.sendall(PING)
            self.assertEqual(receive_exact(connection, 12), PONG)

    def test_idle_and_partial_clients_do_not_block_others(self):
        with self.connect(), self.connect() as partial:
            partial.sendall(PING[:3])
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                results = list(pool.map(lambda _: self.run_cli(), range(4)))
            for result in results:
                self.assert_pong(result)

    def test_half_close_still_receives_pong(self):
        with self.connect() as connection:
            connection.sendall(PING)
            connection.shutdown(socket.SHUT_WR)
            self.assertEqual(receive_exact(connection, 12), PONG)
            self.assert_closed(connection)

    def test_truncated_request_does_not_stop_server(self):
        for size in range(len(PING)):
            with self.subTest(size=size), self.connect() as connection:
                connection.sendall(PING[:size])
                connection.shutdown(socket.SHUT_WR)
                self.assert_closed(connection)
        self.assert_pong(self.run_cli())

    def test_invalid_requests_are_rejected(self):
        requests = [
            b"NOPE" + PING[4:],
            PING[:4] + b"\x00\x02" + PING[6:],
            PING[:6] + b"\x00\x0c" + PING[8:],
            PONG,
            PING[:8] + struct.pack("!I", 1),
            PING[:8] + struct.pack("!I", 1048577),
        ]
        for request in requests:
            with self.subTest(request=request.hex()), self.connect() as connection:
                connection.sendall(request)
                self.assert_closed(connection)
        self.assert_pong(self.run_cli())

    def test_job_reply_headers_are_rejected_from_clients(self):
        # Submission is supported; replies/assignments and unregistered worker
        # reports remain invalid even when their generic headers are well formed.
        # Status responses are outbound-only; status requests are supported.
        with self.connect() as healthy:
            worker_id = self.register_worker(healthy)
            for message_type, length in ((7, 8), (8, 26), (9, 20), (10, 24), (11, 22), (8, 1050),
                                         (13, 36), (13, 1060), (14, 8), (16, 4), (18, 8), (20, 192)):
                with self.subTest(message_type=message_type), self.connect() as connection:
                    connection.sendall(struct.pack("!IHHI", 0x464c494e, 1, message_type, length))
                    self.assert_closed(connection)
                self.send_heartbeat_and_ping(healthy, worker_id)
        self.assert_pong(self.run_cli())

    def test_reset_during_reply_does_not_stop_server(self):
        with self.connect() as connection:
            connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            connection.sendall(PING)
        self.assert_pong(self.run_cli())

    def test_cli_accepts_fragmented_pong(self):
        for sizes in FRAGMENT_SIZES:
            with self.subTest(sizes=sizes):
                self.assert_pong(self.fake_peer(
                    lambda connection, _done: self.send_fragments(connection, PONG, sizes)
                ))

    def test_cli_rejects_bad_and_incomplete_replies(self):
        responses = [PONG[:size] for size in range(len(PONG))]
        responses.extend((PING, b"NOPE" + PONG[4:], PONG[:8] + struct.pack("!I", 1)))
        for response in responses:
            with self.subTest(response=response.hex()):
                result = self.fake_peer(lambda connection, _done: connection.sendall(response))
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertTrue(result.stderr.startswith("faultline:"), result.stderr)

    def test_cli_receive_timeout(self):
        result = self.fake_peer(lambda _connection, done: done.wait(timeout=7))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertIn("faultline: receive PONG:", result.stderr)

    def test_connection_refused(self):
        with socket.socket() as unused:
            unused.bind(("127.0.0.1", 0))
            result = self.run_cli(unused.getsockname()[1])
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("faultline: connect:", result.stderr)

    def test_port_conflict_and_bad_arguments(self):
        for command in (
            [self.coordinator, "--wal", str(Path(self.storage.name) / "conflict.wal"),
             "--init-wal", "--port", str(self.port)],
            [self.coordinator, "--port", "65536"],
            [self.cli, "ping", "--coordinator", "127.0.0.1:0"],
            [self.cli, "ping", "--coordinator", "invalid:9000"],
        ):
            with self.subTest(command=command):
                result = subprocess.run(command, capture_output=True, text=True, timeout=3)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertTrue(result.stderr)

    def test_worker_multiple_registrations_and_heartbeats(self):
        with ExitStack() as stack:
            connections = [stack.enter_context(self.connect()) for _ in range(3)]
            ids = [self.register_worker(connection) for connection in connections]
            self.assertEqual(len(set(ids)), 3)
            self.assert_pong(self.run_cli())
            for connection, worker_id in zip(connections, ids):
                registered = self.wait_for_worker_event("worker_registered", worker_id)
                self.assertEqual(registered["state"], "ALIVE")
                self.send_heartbeat_and_ping(connection, worker_id)
                events = self.worker_events("heartbeat_received", worker_id)
                self.assertEqual(len(events), 1)
                self.assertEqual(events[0]["fd"], registered["fd"])
                self.assertGreaterEqual(int(events[0]["last_heartbeat_ms"]),
                                        int(registered["last_heartbeat_ms"]))

    def test_worker_fragmented_registration(self):
        with self.connect() as connection:
            self.send_fragments(connection, REGISTER, (1,) * 12)
            worker_id = self.receive_registration_ack(connection)
            self.send_heartbeat_and_ping(connection, worker_id)

    def test_worker_fragmented_heartbeat_payload(self):
        with self.connect() as connection:
            worker_id = self.register_worker(connection)
            frame = heartbeat(worker_id)
            for completed, cut in enumerate(range(1, 16)):
                with self.subTest(cut=cut):
                    connection.sendall(frame[:cut])
                    self.assert_waiting_for_more(connection)
                    self.assertEqual(len(self.worker_events("heartbeat_received", worker_id)),
                                     completed)
                    self.assert_pong(self.run_cli())
                    connection.sendall(frame[cut:] + PING)
                    self.assertEqual(receive_exact(connection, 12), PONG)
                    self.assertEqual(len(self.worker_events("heartbeat_received", worker_id)),
                                     completed + 1)

    def test_worker_registration_coalesced_with_ping_and_half_close(self):
        with self.connect() as connection:
            connection.sendall(REGISTER + PING)
            connection.shutdown(socket.SHUT_WR)
            worker_id = self.receive_registration_ack(connection)
            self.assertEqual(receive_exact(connection, 12), PONG)
            self.assert_closed(connection)
            dead = self.wait_for_worker_event("worker_dead", worker_id)
            self.assertEqual(dead["state"], "DEAD")

    def test_worker_disconnect_and_registration_churn(self):
        previous_id = 0
        # More registrations than registry slots proves disconnects release capacity.
        for _ in range(70):
            with self.connect() as connection:
                worker_id = self.register_worker(connection)
                self.assertGreater(worker_id, previous_id)
                registered = self.wait_for_worker_event("worker_registered", worker_id)
            dead = self.wait_for_worker_event("worker_dead", worker_id)
            self.assertEqual(dead["fd"], registered["fd"])
            self.assertEqual(dead["last_heartbeat_ms"], registered["last_heartbeat_ms"])
            self.assertEqual(dead["state"], "DEAD")
            previous_id = worker_id

    def test_worker_heartbeat_identity_is_bound_to_connection(self):
        with self.connect() as owner:
            owner_id = self.register_worker(owner)
            with self.connect() as unregistered:
                unregistered.sendall(heartbeat(owner_id))
                self.assert_closed(unregistered)
            with self.connect() as other:
                other_id = self.register_worker(other)
                other.sendall(heartbeat(owner_id))
                self.assert_closed(other)
                self.wait_for_worker_event("worker_dead", other_id)
            self.assertEqual(self.worker_events("heartbeat_received", owner_id), [])
            self.assertEqual(self.worker_events("worker_dead", owner_id), [])
            self.send_heartbeat_and_ping(owner, owner_id)
            self.assertEqual(len(self.worker_events("heartbeat_received", owner_id)), 1)

        self.wait_for_worker_event("worker_dead", owner_id)
        with self.connect() as replacement:
            new_id = self.register_worker(replacement)
            self.assertNotEqual(new_id, owner_id)
            replacement.sendall(heartbeat(owner_id))
            self.assert_closed(replacement)
            self.wait_for_worker_event("worker_dead", new_id)
            self.assertEqual(self.worker_events("heartbeat_received", new_id), [])

    def test_worker_duplicate_registration(self):
        with self.connect() as connection:
            worker_id = self.register_worker(connection)
            connection.sendall(REGISTER)
            self.assert_closed(connection)
            self.wait_for_worker_event("worker_dead", worker_id)
        with self.connect() as replacement:
            self.assertEqual(self.register_worker(replacement), worker_id + 1)

    def test_worker_truncated_and_invalid_frames_mark_dead(self):
        for size in range(16):
            with self.subTest(size=size), self.connect() as connection:
                worker_id = self.register_worker(connection)
                connection.sendall(heartbeat(worker_id)[:size])
                connection.shutdown(socket.SHUT_WR)
                self.assert_closed(connection)
                self.wait_for_worker_event("worker_dead", worker_id)
                self.assertEqual(self.worker_events("heartbeat_received", worker_id), [])
        bad_frames = [
            heartbeat(0), PONG, REGISTER_ACK_HEADER + struct.pack("!I", 1),
            HEARTBEAT_HEADER[:8] + struct.pack("!I", 3),
            HEARTBEAT_HEADER[:8] + struct.pack("!I", 5),
            REGISTER[:8] + struct.pack("!I", 1),
        ]
        for frame in bad_frames:
            with self.subTest(frame=frame.hex()), self.connect() as connection:
                worker_id = self.register_worker(connection)
                connection.sendall(frame)
                self.assert_closed(connection)
                self.wait_for_worker_event("worker_dead", worker_id)
                self.assertEqual(self.worker_events("heartbeat_received", worker_id), [])

    def test_worker_idle_connection_and_stalled_payload(self):
        with self.connect() as idle, self.connect() as partial:
            idle_id = self.register_worker(idle)
            partial_id = self.register_worker(partial)
            idle_start = self.wait_for_worker_event("worker_registered", idle_id)
            partial_start = self.wait_for_worker_event("worker_registered", partial_id)
            partial.sendall(heartbeat(partial_id)[:14])
            self.assert_waiting_for_more(partial)
            partial.settimeout(7)
            self.assert_closed(partial)
            dead = self.wait_for_worker_event("worker_dead", partial_id)
            self.assertEqual(dead["reason"], "io_timeout")
            self.assertEqual(dead["last_heartbeat_ms"], partial_start["last_heartbeat_ms"])
            # Silence expires from registration even if the socket stays open.
            idle.settimeout(3)
            self.assert_closed(idle)
            self.assertEqual(self.worker_events("heartbeat_received", idle_id), [])
        idle_dead = self.wait_for_worker_event("worker_dead", idle_id)
        self.assertEqual(idle_dead["reason"], "heartbeat_timeout")
        self.assertEqual(idle_dead["last_heartbeat_ms"], idle_start["last_heartbeat_ms"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", type=Path, default=BIN_DIR)
    parser.add_argument("--port", type=int, default=None)
    arguments = parser.parse_args()
    BIN_DIR = arguments.bin_dir
    TEST_PORT = arguments.port
    unittest.main(argv=[__file__], verbosity=2)
