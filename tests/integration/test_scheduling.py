"""CLI acceptance, FIFO dispatch, and worker ownership over real TCP."""

import argparse
import concurrent.futures
from contextlib import contextmanager
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest

import test_ping
from test_worker import WorkerProcessTestCase, read_output


def frame(kind, payload=b''):
    return struct.pack('!IHHI', 0x464c494e, 1, kind, len(payload)) + payload


def submit(arguments=b'abc', retries=0, task=4):
    return frame(6, struct.pack('!HII', task, retries, len(arguments)) + arguments)


def identity(job_id, worker_id, attempt=1):
    return struct.pack('!QIQ', job_id, worker_id, attempt)


def assignment(job_id, worker_id, arguments=b'abc', attempt=1, task=4):
    return frame(8, identity(job_id, worker_id, attempt) + struct.pack('!HI', task, len(arguments)) + arguments)


def receive_frame(connection):
    header = test_ping.receive_exact(connection, 12)
    magic, version, kind, size = struct.unpack('!IHHI', header)
    if magic != 0x464c494e or version != 1 or size > 1060:
        raise AssertionError(f'Invalid frame header: {header.hex()}')
    return kind, test_ping.receive_exact(connection, size)


class JobProcessTestCase(WorkerProcessTestCase):
    # Each test needs an empty job store; the shared lifecycle fixture normally
    # keeps its server for a class because it has no retained jobs.
    @classmethod
    def setUpClass(cls):
        pass

    def setUp(self):
        self.addCleanup(type(self).doClassCleanups)
        super().setUpClass()

    def submit_cli(self, task='hash', *arguments, port=None):
        return subprocess.run([self.cli, 'submit', task, *arguments, '--coordinator',
                               f'127.0.0.1:{port or self.port}'],
                              capture_output=True, text=True, timeout=8)

    def accepted_id(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, '')
        self.assertRegex(result.stdout, r'^job_id=[1-9][0-9]*\n$')
        return int(result.stdout.split('=')[1])

    def receive_ack(self, connection):
        kind, payload = receive_frame(connection)
        self.assertEqual(kind, 7)
        self.assertEqual(len(payload), 8)
        job_id, = struct.unpack('!Q', payload)
        self.assertGreater(job_id, 0)
        return job_id

    def receive_assignment(self, connection, worker_id, job_id, arguments=b'abc', attempt=1):
        kind, payload = receive_frame(connection)
        self.assertEqual(kind, 8)
        self.assertEqual(payload, identity(job_id, worker_id, attempt) +
                         struct.pack('!HI', 4, len(arguments)) + arguments)

    def complete(self, connection, job_id, worker_id, attempt=1, result=b'OK'):
        reference = identity(job_id, worker_id, attempt)
        connection.sendall(frame(9, reference) + frame(10, reference + struct.pack('!I', len(result)) + result))

    def wait_job_event(self, event, job_id):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            matches = re.findall(rf'\b{event} job_id={job_id}\b([^\n]*)', read_output(self.log))
            if matches:
                return dict(re.findall(r'(\w+)=([^ ]+)', matches[-1]))
            time.sleep(0.01)
        self.fail(f'Missing {event} for job {job_id}: {read_output(self.log)}')


class SchedulingTests(JobProcessTestCase):
    def test_jobs_wait_then_two_workers_receive_fifo_assignments(self):
        first = self.accepted_id(self.submit_cli('hash', '--args', 'abc'))
        second = self.accepted_id(self.submit_cli('hash', '--args', 'second'))
        third = self.accepted_id(self.submit_cli('hash', '--args', 'third'))
        self.assertEqual((first, second, third), (1, 2, 3))
        self.assertNotIn('job_assigned', read_output(self.log))
        with self.connect() as worker_a, self.connect() as worker_b:
            a = self.register_worker(worker_a)
            self.receive_assignment(worker_a, a, first)
            b = self.register_worker(worker_b)
            self.receive_assignment(worker_b, b, second, b'second')
            self.assert_waiting_for_more(worker_a)
            self.assert_waiting_for_more(worker_b)
            self.complete(worker_a, first, a, result=b'a\x00b')
            self.receive_assignment(worker_a, a, third, b'third')
            event = self.wait_job_event('job_completed', first)
            self.assertEqual(event['state'], 'DONE')
            self.assertEqual(event['result_bytes'], '3')
            self.send_heartbeat_and_ping(worker_b, b)
        self.assert_pong(self.run_cli())

    def test_idle_worker_and_concurrent_cli_acceptance(self):
        with self.connect() as worker:
            worker_id = self.register_worker(worker)
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                results = list(pool.map(lambda _: self.submit_cli('hash', '--args', 'abc'), range(8)))
            ids = sorted(self.accepted_id(result) for result in results)
            self.assertEqual(ids, list(range(1, 9)))
            for job_id in ids:
                self.receive_assignment(worker, worker_id, job_id)
                self.assert_waiting_for_more(worker)
                self.complete(worker, job_id, worker_id)
            self.wait_job_event('job_completed', ids[-1])
            self.send_heartbeat_and_ping(worker, worker_id)

    def test_fragmented_and_coalesced_submissions_preserve_binary_data(self):
        arguments = bytes(range(256)) * 4
        request = submit(arguments)
        with self.connect() as client:
            for cut in (1, 11, 12, 21, 22, len(request) - 1):
                client.sendall(request[:cut])
                self.assert_waiting_for_more(client)
                client.sendall(request[cut:])
                self.receive_ack(client)
            client.sendall(submit(b'') + submit(b'abc') + test_ping.PING)
            self.assertEqual(self.receive_ack(client), 7)
            self.assertEqual(self.receive_ack(client), 8)
            self.assertEqual(test_ping.receive_exact(client, 12), test_ping.PONG)
        with self.connect() as worker:
            worker_id = self.register_worker(worker)
            self.receive_assignment(worker, worker_id, 1, arguments)
        self.assert_pong(self.run_cli())

    def test_partial_worker_frame_is_not_discarded_by_assignment(self):
        with self.connect() as worker:
            worker_id = self.register_worker(worker)
            beat = test_ping.heartbeat(worker_id)
            worker.sendall(beat[:5])
            self.assert_waiting_for_more(worker)
            job_id = self.accepted_id(self.submit_cli('hash', '--args', 'abc'))
            self.assert_waiting_for_more(worker)
            worker.sendall(beat[5:])
            self.receive_assignment(worker, worker_id, job_id)
            self.assertEqual(len(self.worker_events('heartbeat_received', worker_id)), 1)

    def test_task_failure_and_disconnect_retry_join_the_back(self):
        first = self.accepted_id(self.submit_cli('hash', '--args', 'abc', '--max-retries', '1'))
        second = self.accepted_id(self.submit_cli('hash', '--args', 'second', '--max-retries', '1'))
        with self.connect() as worker:
            worker_id = self.register_worker(worker)
            self.receive_assignment(worker, worker_id, first)
            worker.sendall(frame(11, identity(first, worker_id) + struct.pack('!H', 1)))
            self.receive_assignment(worker, worker_id, second, b'second')
            worker.shutdown(socket.SHUT_RDWR)
        self.wait_job_event('job_worker_lost', second)
        with self.connect() as replacement:
            replacement_id = self.register_worker(replacement)
            self.assertNotEqual(replacement_id, worker_id)
            self.receive_assignment(replacement, replacement_id, first, attempt=2)
            self.complete(replacement, first, replacement_id, attempt=2)
            self.receive_assignment(replacement, replacement_id, second, b'second', attempt=2)
            replacement.sendall(frame(11, identity(second, replacement_id, 2) + struct.pack('!H', 1)))
            self.assertEqual(self.wait_job_event('job_failed', second)['state'], 'FAILED')
            self.send_heartbeat_and_ping(replacement, replacement_id)

    def test_wrong_worker_and_stale_reports_cannot_complete_jobs(self):
        first = self.accepted_id(self.submit_cli('hash', '--args', 'abc', '--max-retries', '1'))
        with self.connect() as owner, self.connect() as intruder:
            owner_id = self.register_worker(owner)
            self.receive_assignment(owner, owner_id, first)
            self.register_worker(intruder)
            intruder.sendall(frame(9, identity(first, owner_id)))
            self.assert_closed(intruder)
            self.send_heartbeat_and_ping(owner, owner_id)
            owner.sendall(frame(11, identity(first, owner_id) + struct.pack('!H', 1)))
            self.receive_assignment(owner, owner_id, first, attempt=2)
            owner.sendall(frame(9, identity(first, owner_id, 1)))
            self.assert_closed(owner)
        event = self.wait_job_event('job_worker_lost', first)
        self.assertEqual(event['state'], 'FAILED')
        self.assertEqual(event['attempt'], '2')
        self.assertNotIn('job_completed', read_output(self.log))
        self.assert_pong(self.run_cli())

    def test_full_store_rejects_without_ack_or_losing_older_jobs(self):
        with self.connect() as client:
            for expected in range(1, 257):
                client.sendall(submit())
                self.assertEqual(self.receive_ack(client), expected)
            client.sendall(submit())
            self.assert_closed(client)
        self.assertEqual(len(re.findall(r'job_submitted job_id=', read_output(self.log))), 256)
        with self.connect() as worker:
            worker_id = self.register_worker(worker)
            self.receive_assignment(worker, worker_id, 1)
        self.assert_pong(self.run_cli())

    def test_real_workers_hold_assignments_and_keep_heartbeating(self):
        first = self.accepted_id(self.submit_cli('sleep', '--args', '60000'))
        second = self.accepted_id(self.submit_cli('sleep', '--args', '60000'))
        third = self.accepted_id(self.submit_cli('sleep', '--args', '60000'))
        with self.worker_process(interval_ms=100) as (a, out_a, err_a), \
                self.worker_process(interval_ms=100) as (b, out_b, err_b):
            a_id = self.wait_for_registration(a, out_a, err_a)
            b_id = self.wait_for_registration(b, out_b, err_b)
            self.wait_job_event('job_assigned', first)
            self.wait_job_event('job_assigned', second)
            time.sleep(0.5)
            for worker_id, process, output in ((a_id, a, out_a), (b_id, b, out_b)):
                self.assertIsNone(process.poll())
                self.assertEqual(read_output(output).count('worker job_assigned'), 1)
                self.assertGreaterEqual(len(self.worker_events('heartbeat_received', worker_id)), 2)
            self.assertNotIn(f'job_assigned job_id={third}', read_output(self.log))
            self.assertEqual(self.wait_job_event('job_started', first)['state'], 'RUNNING')
            self.assertEqual(self.wait_job_event('job_started', second)['state'], 'RUNNING')
            self.stop_worker(a, err_a)
            self.wait_for_worker_event('worker_dead', a_id)
            with self.worker_process(interval_ms=100) as (c, out_c, err_c):
                self.wait_for_registration(c, out_c, err_c)
                self.wait_job_event('job_assigned', third)
                self.stop_worker(c, err_c)
            self.stop_worker(b, err_b)

    def test_worker_assignment_fragments_keep_heartbeats_active(self):
        with self.fake_coordinator(interval_ms=80) as (connection, process, output, errors):
            connection.sendall(test_ping.REGISTER_ACK_HEADER + struct.pack('!I', 7))
            self.wait_for_registration(process, output, errors)
            request = assignment(42, 7, b'60000', task=1)
            offset = 0
            for size in (1, 11, 13, len(request) - 26):
                connection.sendall(request[offset:offset + size])
                offset += size
                kind, payload = receive_frame(connection)
                self.assertEqual((kind, payload), (5, struct.pack('!I', 7)))
                self.assertNotIn('job_assigned', read_output(output))
            connection.sendall(request[offset:])
            deadline = time.monotonic() + 2
            while 'job_assigned' not in read_output(output) and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertIn('job_id=42 worker_id=7 attempt=1 task_type=1 argument_bytes=5', read_output(output))
            connection.sendall(assignment(43, 7))
            self.assertEqual(process.wait(timeout=2), 1)
            self.assertIn('already busy', read_output(errors))

    def test_invalid_and_truncated_submissions_do_not_consume_ids(self):
        invalid = [submit(task=0), frame(6, struct.pack('!HII', 4, 0, 1)),
                   submit(b'x' * 1025), frame(6, b''),
                   frame(6, struct.pack('!HII', 4, 0, 0xffffffff))]
        for request in invalid:
            with self.subTest(request=request[:24].hex()), self.connect() as client:
                client.sendall(request)
                self.assert_closed(client)
        request = submit()
        for cut in (1, 11, 12, 20, len(request) - 1):
            with self.connect() as client:
                client.sendall(request[:cut])
                client.shutdown(socket.SHUT_WR)
                self.assert_closed(client)
        self.assertEqual(self.accepted_id(self.submit_cli()), 1)
        with self.connect() as worker:
            worker_id = self.register_worker(worker)
            self.receive_assignment(worker, worker_id, 1, b'')
            worker.sendall(submit())
            self.assert_closed(worker)
        self.assertEqual(len(re.findall(r'job_submitted job_id=', read_output(self.log))), 1)

    def test_heartbeat_expiry_reassigns_to_a_live_idle_worker(self):
        job_id = self.accepted_id(self.submit_cli('hash', '--args', 'abc', '--max-retries', '1'))
        with self.connect() as silent:
            old_id = self.register_worker(silent)
            self.receive_assignment(silent, old_id, job_id)
            with self.worker_process(interval_ms=100) as (worker, output, errors):
                new_id = self.wait_for_registration(worker, output, errors)
                deadline = time.monotonic() + 8
                while 'job_assigned' not in read_output(output) and time.monotonic() < deadline:
                    self.assertIsNone(worker.poll())
                    time.sleep(0.02)
                self.assertIn(f'job_id={job_id} worker_id={new_id} attempt=2', read_output(output))
                self.assertEqual(self.wait_for_worker_event('worker_dead', old_id)['reason'], 'heartbeat_timeout')
                self.assert_closed(silent)
                self.assert_pong(self.run_cli())
                self.stop_worker(worker, errors)

    def test_worker_rejects_wrong_identity_and_times_out_partial_assignment(self):
        for request in (assignment(42, 8), assignment(42, 7, attempt=0)):
            with self.fake_coordinator(interval_ms=80) as (connection, process, output, errors):
                connection.sendall(test_ping.REGISTER_ACK_HEADER + struct.pack('!I', 7) + request)
                self.assertEqual(process.wait(timeout=2), 1)
                self.assertNotIn('job_assigned', read_output(output))
        with self.fake_coordinator(interval_ms=80) as (connection, process, output, errors):
            connection.sendall(test_ping.REGISTER_ACK_HEADER + struct.pack('!I', 7))
            self.wait_for_registration(process, output, errors)
            connection.sendall(assignment(42, 7)[:20])
            self.assertEqual(process.wait(timeout=6), 1)
            self.assertIn('assignment receive timeout', read_output(errors))
            self.assertGreaterEqual(read_output(output).count('heartbeat_sent'), 3)
            self.assertNotIn('job_assigned', read_output(output))

    @contextmanager
    def cli_peer(self, arguments):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(3)
            with tempfile.TemporaryFile(mode='w+') as output, tempfile.TemporaryFile(mode='w+') as errors:
                process = subprocess.Popen([self.cli, 'submit', *arguments, '--coordinator',
                    f'127.0.0.1:{listener.getsockname()[1]}'], stdout=output, stderr=errors)
                try:
                    with listener.accept()[0] as connection:
                        connection.settimeout(2)
                        yield connection, process, output, errors
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.wait(timeout=2)
                    self.assertNotIn('ERROR: AddressSanitizer', read_output(errors))
                    self.assertNotIn('runtime error:', read_output(errors))

    def test_cli_encodes_arguments_and_waits_for_complete_ack(self):
        for task, task_id in (('sleep', 1), ('prime_count', 2), ('fibonacci', 3), ('hash', 4)):
            with self.cli_peer([task, '--args', '1000', '--max-retries', '4294967295']) as (connection, process, output, errors):
                self.assertEqual(receive_frame(connection), (6, struct.pack('!HII', task_id, 0xffffffff, 4) + b'1000'))
                ack = frame(7, struct.pack('!Q', 0xffffffffffffffff))
                for cut in (1, 11, 7):
                    connection.sendall(ack[:cut])
                    ack = ack[cut:]
                    time.sleep(0.03)
                    self.assertIsNone(process.poll())
                    self.assertEqual(read_output(output), '')
                connection.sendall(ack)
                self.assertEqual(process.wait(timeout=2), 0)
                self.assertEqual(read_output(output), 'job_id=18446744073709551615\n')
        with self.cli_peer(['hash', '--args-hex', '00AaFF']) as (connection, process, output, errors):
            self.assertEqual(receive_frame(connection), (6, struct.pack('!HII', 4, 0, 3) + b'\x00\xaa\xff'))
            connection.sendall(frame(7, struct.pack('!Q', 42)))
            self.assertEqual(process.wait(timeout=2), 0)
            self.assertEqual(read_output(output), 'job_id=42\n')

    def test_cli_rejects_bad_options_and_unconfirmed_ack(self):
        bad = [[], ['unknown'], ['hash', '--args'], ['hash', '--args-hex', '0'],
               ['hash', '--args-hex', 'zz'], ['hash', '--args', 'x' * 1025],
               ['hash', '--args-hex', '00' * 1025], ['hash', '--args', 'a', '--args-hex', 'aa'],
               ['hash', '--max-retries', '-1'], ['hash', '--max-retries', '4294967296'],
               ['hash', '--max-retries', '9' * 80], ['hash', '--max-retries', '+1'],
               ['hash', '--max-retries', '1', '--max-retries', '2'], ['hash', '--unknown', 'x']]
        for arguments in bad:
            result = subprocess.run([self.cli, 'submit', *arguments], capture_output=True, text=True, timeout=2)
            self.assertEqual(result.returncode, 1, arguments)
            self.assertEqual(result.stdout, '')
            self.assertIn('Usage:', result.stderr)
        for response in (frame(7, struct.pack('!Q', 0)), frame(2), frame(7, b'1234567'),
                         frame(7, struct.pack('!Q', 42))[:19]):
            with self.cli_peer(['hash']) as (connection, process, output, errors):
                receive_frame(connection)
                connection.sendall(response)
                connection.shutdown(socket.SHUT_WR)
                self.assertEqual(process.wait(timeout=2), 1)
                self.assertEqual(read_output(output), '')
                self.assertIn('submission unconfirmed', read_output(errors))

    def test_cli_ack_uses_one_deadline(self):
        with self.cli_peer(['hash']) as (connection, process, output, errors):
            receive_frame(connection)
            start = time.monotonic()
            time.sleep(3.3)
            connection.sendall(frame(7, struct.pack('!Q', 42))[:12])
            self.assertEqual(process.wait(timeout=2.7), 1)
            self.assertLess(time.monotonic() - start, 5.9)
            self.assertEqual(read_output(output), '')
            self.assertIn('submission unconfirmed', read_output(errors))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=Path('build/debug'))
    parser.add_argument('--port', type=int, default=None)
    arguments = parser.parse_args()
    test_ping.BIN_DIR = arguments.bin_dir
    test_ping.TEST_PORT = arguments.port
    unittest.main(argv=[__file__], verbosity=2)
