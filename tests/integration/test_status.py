"""Read-only status queries through real CLI/coordinator processes and TCP peers."""

import argparse
from contextlib import contextmanager
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import unittest

import test_persistence as persistence
import test_ping
from test_scheduling import JobProcessTestCase, frame, identity, receive_frame, submit
from test_worker import read_output


def request(job):
    return frame(12, struct.pack('!Q', job))


def response(job, state=1, worker=0, attempt=0, retries=0, limit=2, failure=0, result=b''):
    return frame(13, struct.pack('!QHIQIIHI', job, state, worker, attempt,
                                retries, limit, failure, len(result)) + result)


def missing(job):
    return frame(14, struct.pack('!Q', job))


def status_cli(job, port):
    return subprocess.run([str((test_ping.BIN_DIR / 'faultline').resolve()), 'status', str(job),
                           '--coordinator', f'127.0.0.1:{port}'],
                          capture_output=True, text=True, timeout=8)


def fields(test, result):
    test.assertEqual(result.returncode, 0, result.stderr)
    test.assertEqual(result.stderr, '')
    return dict(line.split('=', 1) for line in result.stdout.splitlines())


class StatusTests(JobProcessTestCase):
    def test_queued_unknown_and_default_endpoint_are_read_only(self):
        job = self.accepted_id(self.submit_cli('hash', '--max-retries', '2'))
        before = Path(self.wal).read_bytes()
        for _ in range(3):
            result = status_cli(job, self.port)
            self.assertEqual(fields(self, result), dict(job_id='1', state='QUEUED', worker_id='none',
                attempt='0', retries='0/2', failure='NONE', result_bytes='0'))
            result = status_cli(2**64 - 1, self.port)
            self.assertEqual((result.returncode, result.stdout, result.stderr),
                             (2, '', 'faultline: job 18446744073709551615 not found\n'))
        if self.port == 9000:
            result = subprocess.run([self.cli, 'status', '1'], capture_output=True, text=True, timeout=8)
            self.assertEqual(fields(self, result)['state'], 'QUEUED')
        self.assertEqual(Path(self.wal).read_bytes(), before)
        self.assertEqual(self.accepted_id(self.submit_cli()), 2)
        with self.connect() as worker:
            worker_id = self.register_worker(worker)
            self.assertEqual(worker_id, 1)
            self.receive_assignment(worker, worker_id, job, b'')

    def test_assigned_running_done_and_binary_result(self):
        job = self.accepted_id(self.submit_cli('hash', '--args', 'abc'))
        with self.connect() as worker:
            worker_id = self.register_worker(worker)
            self.receive_assignment(worker, worker_id, job)
            before = Path(self.wal).read_bytes()
            assigned = fields(self, status_cli(job, self.port))
            self.assertEqual((assigned['state'], assigned['worker_id'], assigned['attempt']),
                             ('ASSIGNED', str(worker_id), '1'))
            self.assertEqual(Path(self.wal).read_bytes(), before)
            worker.sendall(frame(9, identity(job, worker_id)))
            self.wait_job_event('job_started', job)
            before = Path(self.wal).read_bytes()
            self.assertEqual(fields(self, status_cli(job, self.port))['state'], 'RUNNING')
            self.assertEqual(Path(self.wal).read_bytes(), before)
            data = b'OK\x00\n\r\x1b"\\\xff' + b'a' * 1015
            self.assertEqual(len(data), 1024)
            worker.sendall(frame(10, identity(job, worker_id) + struct.pack('!I', len(data)) + data))
            self.wait_job_event('job_completed', job)
            before = Path(self.wal).read_bytes()
            with self.connect() as client:
                client.sendall(request(job))
                self.assertEqual(test_ping.receive_exact(client, 1072),
                                 response(job, 4, worker_id, 1, limit=0, result=data))
            done = fields(self, status_cli(job, self.port))
            self.assertEqual(done['result'], '"OK\\x00\\x0a\\x0d\\x1b\\x22\\x5c\\xff' + 'a' * 1015 + '"')
            self.assertEqual((done['state'], done['result_bytes'], done['failure']), ('DONE', '1024', 'NONE'))
            self.assertEqual(Path(self.wal).read_bytes(), before)
        self.wait_for_worker_event('worker_dead', worker_id)
        self.assertEqual(fields(self, status_cli(job, self.port)), done)

    def test_task_retry_and_terminal_failure_reasons(self):
        first = self.accepted_id(self.submit_cli('hash', '--args', 'abc', '--max-retries', '1'))
        second = self.accepted_id(self.submit_cli('hash', '--args', 'abc'))
        with self.connect() as worker:
            worker_id = self.register_worker(worker)
            self.receive_assignment(worker, worker_id, first)
            worker.sendall(frame(11, identity(first, worker_id) + struct.pack('!H', 1)))
            self.receive_assignment(worker, worker_id, second)
            retry = fields(self, status_cli(first, self.port))
            self.assertEqual((retry['state'], retry['worker_id'], retry['attempt'], retry['retries'], retry['failure']),
                             ('QUEUED', 'none', '1', '1/1', 'TASK'))
            worker.sendall(frame(11, identity(second, worker_id) + struct.pack('!H', 1)))
            self.receive_assignment(worker, worker_id, first, attempt=2)
            failed = fields(self, status_cli(second, self.port))
            self.assertEqual((failed['state'], failed['failure'], failed['retries']), ('FAILED', 'TASK', '0/0'))
            self.assertNotIn('result', failed)
        self.wait_job_event('job_worker_lost', first)
        failed = fields(self, status_cli(first, self.port))
        self.assertEqual((failed['state'], failed['attempt'], failed['retries'], failed['failure']),
                         ('FAILED', '2', '1/1', 'WORKER_LOST'))

    def test_worker_loss_retry_and_empty_success(self):
        job = self.accepted_id(self.submit_cli('hash', '--args', 'abc', '--max-retries', '2'))
        with self.connect() as worker:
            old_id = self.register_worker(worker)
            self.receive_assignment(worker, old_id, job)
        self.wait_job_event('job_worker_lost', job)
        retry = fields(self, status_cli(job, self.port))
        self.assertEqual((retry['state'], retry['failure'], retry['attempt'], retry['retries']),
                         ('QUEUED', 'WORKER_LOST', '1', '1/2'))
        with self.connect() as worker:
            worker_id = self.register_worker(worker)
            self.receive_assignment(worker, worker_id, job, attempt=2)
            self.complete(worker, job, worker_id, attempt=2, result=b'')
            self.wait_job_event('job_completed', job)
            done = fields(self, status_cli(job, self.port))
            self.assertEqual((done['worker_id'], done['failure'], done['result'], done['result_bytes']),
                             (str(worker_id), 'NONE', '""', '0'))

    def test_fragmented_and_coalesced_queries_and_submissions(self):
        job = self.accepted_id(self.submit_cli('hash', '--max-retries', '2'))
        before = Path(self.wal).read_bytes()
        with self.connect() as client:
            for cut in range(1, 20):
                client.sendall(request(job)[:cut])
                self.assert_waiting_for_more(client)
                client.sendall(request(job)[cut:])
                self.assertEqual(test_ping.receive_exact(client, 48), response(job))
            client.sendall(request(999) + request(job) + test_ping.PING)
            self.assertEqual(test_ping.receive_exact(client, 80), missing(999) + response(job) + test_ping.PONG)
            self.assertEqual(Path(self.wal).read_bytes(), before)
            client.sendall(submit() + request(2))
            self.assertEqual(self.receive_ack(client), 2)
            self.assertEqual(test_ping.receive_exact(client, 48), response(2, limit=0))
            client.sendall(test_ping.REGISTER)
            self.assert_closed(client)

    def test_invalid_requests_and_connection_roles(self):
        before = Path(self.wal).read_bytes()
        invalid = [request(0), frame(12, b'')[:12], frame(12, b'x' * 7)[:12], frame(12, b'x' * 9)[:12],
                   struct.pack('!IHHI', 0x464c494e, 1, 12, 0xffffffff)]
        for message in invalid:
            with self.subTest(message=message.hex()), self.connect() as client:
                client.sendall(message)
                self.assert_closed(client)
        for cut in (1, 11, 12, 13, 19):
            with self.connect() as client:
                client.sendall(request(42)[:cut])
                client.shutdown(socket.SHUT_WR)
                self.assert_closed(client)
        with self.connect() as client:
            client.sendall(request(42))
            self.assertEqual(test_ping.receive_exact(client, 20), missing(42))
            client.sendall(test_ping.REGISTER)
            self.assert_closed(client)
        self.assertEqual(Path(self.wal).read_bytes(), before)
        with self.connect() as worker:
            self.assertEqual(self.register_worker(worker), 1)
            before = Path(self.wal).read_bytes()
            worker.sendall(request(42)[:12])  # Reject a worker query without waiting for its ID.
            self.assert_closed(worker)
            self.assertEqual(Path(self.wal).read_bytes(), before)
        self.assertEqual(self.accepted_id(self.submit_cli()), 1)

    def test_full_store_still_supports_read_only_queries(self):
        with self.connect() as client:
            for job in range(1, 257):
                client.sendall(submit())
                self.assertEqual(self.receive_ack(client), job)
            before = Path(self.wal).read_bytes()
            client.sendall(request(1) + request(256) + request(257))
            self.assertEqual(test_ping.receive_exact(client, 116),
                             response(1, limit=0) + response(256, limit=0) + missing(257))
            self.assertEqual(Path(self.wal).read_bytes(), before)
        with self.connect() as worker:
            worker_id = self.register_worker(worker)
            self.receive_assignment(worker, worker_id, 1)

    def test_real_builtin_results_and_invalid_task_input(self):
        with self.worker_process(interval_ms=100) as (worker, output, errors):
            worker_id = self.wait_for_registration(worker, output, errors)
            for task, argument, expected in (('sleep', '0', 'slept_ms=0'), ('prime_count', '100', '25'),
                                              ('fibonacci', '10', '55'), ('hash', 'hello', 'a430d84680aabd0b')):
                job = self.accepted_id(self.submit_cli(task, '--args', argument))
                self.wait_job_event('job_completed', job)
                done = fields(self, status_cli(job, self.port))
                self.assertEqual((done['state'], done['worker_id'], done['result']),
                                 ('DONE', str(worker_id), f'"{expected}"'))
            job = self.accepted_id(self.submit_cli('fibonacci', '--args', '94'))
            self.wait_job_event('job_failed', job)
            self.assertEqual(fields(self, status_cli(job, self.port))['failure'], 'TASK')
            self.stop_worker(worker, errors)


class StatusHeartbeatTests(JobProcessTestCase):
    COORDINATOR_ARGUMENTS = ('--heartbeat-timeout-ms', '500')

    def test_queries_do_not_keep_a_silent_worker_alive(self):
        job = self.accepted_id(self.submit_cli('hash', '--args', 'abc'))
        with self.connect() as worker:
            worker_id = self.register_worker(worker)
            self.receive_assignment(worker, worker_id, job)
            worker.sendall(frame(9, identity(job, worker_id)))
            self.wait_job_event('job_started', job)
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                snapshot = fields(self, status_cli(job, self.port))
                if snapshot['state'] == 'FAILED':
                    break
                self.assertEqual(snapshot['state'], 'RUNNING')
                time.sleep(.02)
            else:
                self.fail('Status queries postponed heartbeat expiry')
            self.assertEqual(snapshot['failure'], 'WORKER_LOST')
            self.assertEqual(self.wait_for_worker_event('worker_dead', worker_id)['reason'], 'heartbeat_timeout')
            self.assert_closed(worker)


class StatusRestartTests(persistence.PersistenceTestCase):
    def test_cli_reads_replayed_terminal_queued_and_interrupted_jobs(self):
        server = self.server().ready()
        worker = self.connect(server)
        worker_id = self.register(worker)
        self.assertEqual(self.submit(server), 1)
        self.assigned(worker, 1, worker_id)
        self.complete(worker, 1, worker_id, result=b'saved\x00result')
        server.event('job_completed job_id=1 ')
        self.assertEqual(self.submit(server, retries=0), 2)
        self.assigned(worker, 2, worker_id)
        worker.sendall(frame(11, identity(2, worker_id) + struct.pack('!H', 1)))
        server.event('job_failed job_id=2 ')
        self.assertEqual(self.submit(server), 3)
        self.assigned(worker, 3, worker_id)
        worker.sendall(frame(9, identity(3, worker_id)))
        server.event('job_started job_id=3 ')
        self.assertEqual(self.submit(server), 4)
        previous = {job: fields(self, status_cli(job, server.port)) for job in (1, 2, 4)}
        server.stop(crash=True)
        restarted = self.server(initialize=False, port=server.port).ready()
        before = self.path.read_bytes()
        for job in (1, 2, 4):
            self.assertEqual(fields(self, status_cli(job, restarted.port)), previous[job])
        retry = fields(self, status_cli(3, restarted.port))
        self.assertEqual((retry['state'], retry['worker_id'], retry['attempt'], retry['retries'], retry['failure']),
                         ('QUEUED', 'none', '1', '1/1', 'WORKER_LOST'))
        self.assertEqual(status_cli(999, restarted.port).returncode, 2)
        self.assertEqual(self.path.read_bytes(), before)
        restarted.stop()


class CliStatusTests(unittest.TestCase):
    @contextmanager
    def peer(self, job='42'):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(3)
            with tempfile.TemporaryFile(mode='w+') as output, tempfile.TemporaryFile(mode='w+') as errors:
                process = subprocess.Popen([str((test_ping.BIN_DIR / 'faultline').resolve()), 'status', job,
                    '--coordinator', f'127.0.0.1:{listener.getsockname()[1]}'], stdout=output, stderr=errors)
                try:
                    with listener.accept()[0] as connection:
                        connection.settimeout(2)
                        self.assertEqual(test_ping.receive_exact(connection, 20), request(int(job)))
                        yield connection, process, output, errors
                finally:
                    if process.poll() is None:
                        process.kill()
                    process.wait(timeout=2)
                    self.assertNotIn('AddressSanitizer', read_output(errors))
                    self.assertNotIn('runtime error:', read_output(errors))

    def test_id_boundaries_and_fragmented_responses(self):
        for job in ('0001', '4294967296', '18446744073709551615'):
            with self.peer(job) as (connection, process, output, errors):
                message = response(int(job), 4, 0xffffffff, 2**32, 0xffffffff, 0xffffffff, result=b'x' * 1024)
                offset = 0
                for end in (1, 11, 12, 20, 26, 44, 48, len(message) - 1):
                    connection.sendall(message[offset:end])
                    offset = end
                    time.sleep(.02)
                    self.assertIsNone(process.poll())
                    self.assertEqual(read_output(output), '')
                connection.sendall(message[offset:])
                self.assertEqual(process.wait(timeout=2), 0, read_output(errors))
                self.assertIn(f'job_id={int(job)}\n', read_output(output))
                self.assertIn('attempt=4294967296\nretries=4294967295/4294967295\n', read_output(output))
                self.assertIn('result="' + 'x' * 1024 + '"\n', read_output(output))
        with self.peer() as (connection, process, output, errors):
            connection.sendall(missing(42)[:19])
            time.sleep(.03)
            self.assertIsNone(process.poll())
            connection.sendall(missing(42)[19:])
            self.assertEqual(process.wait(timeout=2), 2)
            self.assertEqual(read_output(output), '')
            self.assertEqual(read_output(errors), 'faultline: job 42 not found\n')

    def test_invalid_arguments(self):
        invalid = [[], [''], ['0'], ['0000'], ['-1'], ['+1'], [' 1'], ['1 '], ['0x2a'], ['1.0'],
                   ['18446744073709551616'], ['9' * 100], ['1', 'extra'], ['1', '--coordinator'],
                   ['1', '--coordinator', 'localhost:9000'], ['1', '--coordinator', '127.0.0.1:0'],
                   ['1', '--coordinator', '127.0.0.1:65536'], ['1', '--unknown', 'x'],
                   ['1', '--coordinator', '127.0.0.1:9000', '--coordinator', '127.0.0.1:9000']]
        for arguments in invalid:
            with self.subTest(arguments=arguments):
                result = subprocess.run([str((test_ping.BIN_DIR / 'faultline').resolve()), 'status', *arguments],
                                        capture_output=True, text=True, timeout=2)
                self.assertEqual((result.returncode, result.stdout), (1, ''))
                self.assertIn('Usage:', result.stderr)
                self.assertNotIn('connect', result.stderr)

    def test_malformed_replies_are_rejected_without_waiting_for_body(self):
        invalid = [frame(2), frame(12, b'12345678')[:12], frame(14, b'1234567')[:12],
                   frame(13, b'x' * 35)[:12], frame(13, b'x' * 1061)[:12],
                   b'NOPE' + response(42)[4:12],
                   struct.pack('!IHHI', 0x464c494e, 2, 13, 36),
                   response(42, state=0), response(42, worker=1), response(42, attempt=1),
                   response(42, result=b'x')[:48], response(0), missing(0), response(43), missing(43),
                   response(42, 4, 1, 1, result=b'xx')[:44] + struct.pack('!I', 3)]
        for message in invalid:
            with self.subTest(message=message.hex()), self.peer() as (connection, process, output, errors):
                connection.sendall(message)
                self.assertEqual(process.wait(timeout=2), 1, read_output(errors))
                self.assertEqual(read_output(output), '')
                self.assertIn('faultline:', read_output(errors))
                self.assertNotIn('not found', read_output(errors))

    def test_truncation_and_connection_refusal(self):
        message = response(42, 4, 1, 1, result=b'hello')
        for cut in (0, 1, 11, 12, 19, 20, 43, 44, 47, 48, len(message) - 1):
            with self.subTest(cut=cut), self.peer() as (connection, process, output, errors):
                connection.sendall(message[:cut])
                connection.shutdown(socket.SHUT_WR)
                self.assertEqual(process.wait(timeout=2), 1)
                self.assertEqual(read_output(output), '')
                self.assertIn('status response incomplete', read_output(errors))
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))  # Bound but not listening: no port-reuse race.
            result = status_cli(42, reservation.getsockname()[1])
            self.assertEqual((result.returncode, result.stdout), (1, ''))
            self.assertIn('connect', result.stderr)

    def test_header_prefix_and_result_share_one_deadline(self):
        with self.peer() as (connection, process, output, errors):
            message = response(42, 4, 1, 1, result=b'hello')
            start = time.monotonic()
            time.sleep(2)
            connection.sendall(message[:12])
            time.sleep(2)
            connection.sendall(message[12:48])
            self.assertEqual(process.wait(timeout=1.9), 1)
            self.assertLess(time.monotonic() - start, 5.9)
            self.assertEqual(read_output(output), '')
            self.assertIn('status response incomplete', read_output(errors))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=Path('build/debug'))
    parser.add_argument('--port', type=int, default=None)
    args, remaining = parser.parse_known_args()
    test_ping.BIN_DIR = persistence.BIN_DIR = args.bin_dir
    test_ping.TEST_PORT = args.port
    unittest.main(argv=[__file__, *remaining], verbosity=2)
