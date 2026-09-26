"""Bounded job/worker snapshots, CLI rendering, liveness, and restart behavior."""
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

JOB_ROW = '!QHHIQIIHI'
WORKER_ROW = '!IHQQQ'


def job_row(job, task=4, state=1, worker=0, attempt=0, retry=0, limit=0, failure=0, size=0):
    return struct.pack(JOB_ROW, job, task, state, worker, attempt, retry, limit, failure, size)


def job_list(*rows):
    return frame(16, struct.pack('!I', len(rows)) + b''.join(rows))


def worker_list(*rows, timeout=6000):
    return frame(18, struct.pack('!II', len(rows), timeout) + b''.join(rows))


def cli(command, port):
    return subprocess.run([str((test_ping.BIN_DIR / 'faultline').resolve()), command,
                           '--coordinator', f'127.0.0.1:{port}'], capture_output=True, text=True, timeout=8)


def table(test, result):
    test.assertEqual((result.returncode, result.stderr), (0, ''), result)
    lines = result.stdout.splitlines()
    if lines[1].startswith('No retained '):
        return []
    return [dict(zip(lines[1].split(), line.split())) for line in lines[2:]]


class ListingTests(JobProcessTestCase):
    def wire(self, request_type):
        with self.connect() as connection:
            connection.sendall(frame(request_type))
            kind, payload = receive_frame(connection)
            self.assertEqual(kind, request_type + 1)
            return payload

    def test_empty_lists_default_endpoint_and_read_only_ids(self):
        before = Path(self.wal).read_bytes()
        for name, output in [('jobs', 'jobs=0\nNo retained jobs.\n'),
                             ('workers', 'workers=0 heartbeat_timeout_ms=6000\nNo retained worker registrations.\n')]:
            result = cli(name, self.port)
            self.assertEqual((result.returncode, result.stdout, result.stderr), (0, output, ''))
            if self.port == 9000:
                result = subprocess.run([self.cli, name], capture_output=True, text=True, timeout=8)
                self.assertEqual((result.returncode, result.stdout, result.stderr), (0, output, ''))
        self.assertEqual(Path(self.wal).read_bytes(), before)
        self.assertEqual(self.accepted_id(self.submit_cli()), 1)
        with self.connect() as worker:
            self.assertEqual(self.register_worker(worker), 1)

    def test_mixed_retained_states_and_active_worker_jobs(self):
        with self.connect() as first, self.connect() as second:
            a = self.register_worker(first)
            self.assertEqual(self.accepted_id(self.submit_cli('hash', '--args', 'abc')), 1)
            self.receive_assignment(first, a, 1)
            self.complete(first, 1, a, result=b'a\x00b')
            self.wait_job_event('job_completed', 1)
            self.assertEqual(self.accepted_id(self.submit_cli('hash', '--args', 'abc')), 2)
            self.receive_assignment(first, a, 2)
            first.sendall(frame(11, identity(2, a) + struct.pack('!H', 1)))
            self.wait_job_event('job_failed', 2)
            self.assertEqual(self.accepted_id(self.submit_cli('hash', '--args', 'abc', '--max-retries', '2')), 3)
            self.receive_assignment(first, a, 3)
            first.sendall(frame(9, identity(3, a)))
            self.wait_job_event('job_started', 3)
            b = self.register_worker(second)
            self.assertEqual(self.accepted_id(self.submit_cli('hash', '--args', 'abc')), 4)
            self.receive_assignment(second, b, 4)
            self.assertEqual(self.accepted_id(self.submit_cli('sleep', '--args', '100')), 5)
            before = Path(self.wal).read_bytes()
            expected = job_list(job_row(1, state=4, worker=a, attempt=1, size=3),
                                job_row(2, state=5, worker=a, attempt=1, failure=1),
                                job_row(3, state=3, worker=a, attempt=1, limit=2),
                                job_row(4, state=2, worker=b, attempt=1), job_row(5, task=1))
            self.assertEqual(self.wire(15), expected[12:])
            rows = table(self, cli('jobs', self.port))
            self.assertEqual([row['STATE'] for row in rows], ['DONE', 'FAILED', 'RUNNING', 'ASSIGNED', 'QUEUED'])
            self.assertEqual([row['TASK'] for row in rows], ['hash'] * 4 + ['sleep'])
            self.assertEqual(rows[0]['RESULT_BYTES'], '3')
            workers = table(self, cli('workers', self.port))
            self.assertEqual([row['JOB_ID'] for row in workers], ['3', '4'])
            self.assertTrue(all(row['LIVENESS'] == 'ALIVE' and row['ATTEMPT'] == '1' for row in workers))
            self.assertEqual(Path(self.wal).read_bytes(), before)

    def test_retries_preserve_fifo_and_query_does_not_change_wal(self):
        job = self.accepted_id(self.submit_cli('hash', '--args', 'abc', '--max-retries', '1'))
        with self.connect() as worker:
            worker_id = self.register_worker(worker)
            self.receive_assignment(worker, worker_id, job)
        self.wait_job_event('job_worker_lost', job)
        self.assertEqual(self.accepted_id(self.submit_cli('hash', '--args', 'abc')), 2)
        before = Path(self.wal).read_bytes()
        for _ in range(3):
            jobs = table(self, cli('jobs', self.port))
            self.assertEqual((jobs[0]['STATE'], jobs[0]['WORKER_ID'], jobs[0]['ATTEMPT'], jobs[0]['RETRIES'], jobs[0]['FAILURE']),
                             ('QUEUED', 'none', '1', '1/1', 'WORKER_LOST'))
            workers = table(self, cli('workers', self.port))
            self.assertEqual((workers[0]['LIVENESS'], workers[0]['JOB_ID'], workers[0]['ATTEMPT']), ('DEAD', 'none', '0'))
        self.assertEqual(Path(self.wal).read_bytes(), before)
        with self.connect() as worker:
            worker_id = self.register_worker(worker)
            self.receive_assignment(worker, worker_id, job, attempt=2)
            self.complete(worker, job, worker_id, attempt=2)
            self.receive_assignment(worker, worker_id, 2)

    def test_heartbeat_age_and_reused_slots_are_sorted_by_identity(self):
        with self.connect() as a, self.connect() as b, self.connect() as c:
            self.assertEqual(self.register_worker(a), 1)
            self.assertEqual(self.register_worker(b), 2)
            self.assertEqual(self.register_worker(c), 3)
            time.sleep(.08)
            before = table(self, cli('workers', self.port))
            self.assertTrue(all(int(row['HEARTBEAT_AGE_MS']) >= 60 for row in before))
            self.send_heartbeat_and_ping(c, 3)
            refreshed = table(self, cli('workers', self.port))
            self.assertLess(int(refreshed[2]['HEARTBEAT_AGE_MS']), int(refreshed[1]['HEARTBEAT_AGE_MS']))
            a.shutdown(socket.SHUT_RDWR)
            self.wait_for_worker_event('worker_dead', 1)
            dead = table(self, cli('workers', self.port))
            self.assertEqual(dead[0]['LIVENESS'], 'DEAD')
            with self.connect() as replacement:
                self.assertEqual(self.register_worker(replacement), 4)
                before_wal = Path(self.wal).read_bytes()
                rows = table(self, cli('workers', self.port))
                self.assertEqual([row['WORKER_ID'] for row in rows], ['2', '3', '4'])
                self.assertEqual(Path(self.wal).read_bytes(), before_wal)

    def test_maximum_retained_jobs_and_stable_snapshot(self):
        with self.connect() as client:
            for job in range(1, 257):
                client.sendall(submit(retries=2))
                self.assertEqual(self.receive_ack(client), job)
            before = Path(self.wal).read_bytes()
            client.sendall(frame(15))
            prefix = test_ping.receive_exact(client, 16)
            self.assertEqual(prefix, job_list(*(job_row(job, limit=2) for job in range(1, 257)))[:16])
            # Change a job after the response was encoded but before reading its rows.
            with self.connect() as worker:
                worker_id = self.register_worker(worker)
                self.receive_assignment(worker, worker_id, 1)
                body = test_ping.receive_exact(client, 9728)
                self.assertEqual(body, b''.join(job_row(job, limit=2) for job in range(1, 257)))
                current = table(self, cli('jobs', self.port))
                self.assertEqual(len(current), 256)
                self.assertEqual(current[0]['STATE'], 'ASSIGNED')
                self.assertEqual(current[-1]['JOB_ID'], '256')
                stable = Path(self.wal).read_bytes()
                self.wire(15)
                self.assertEqual(Path(self.wal).read_bytes(), stable)
                self.assertGreater(len(stable), len(before))  # Only registration/assignment wrote records.

    def test_fragmented_coalesced_requests_and_roles(self):
        before = Path(self.wal).read_bytes()
        for kind, expected in [(15, job_list()), (17, worker_list())]:
            with self.connect() as client:
                for cut in range(1, 12):
                    client.sendall(frame(kind)[:cut])
                    self.assert_waiting_for_more(client)
                    client.sendall(frame(kind)[cut:])
                    self.assertEqual(test_ping.receive_exact(client, len(expected)), expected)
                client.sendall(frame(15) + frame(17) + test_ping.PING)
                self.assertEqual(test_ping.receive_exact(client, 48), job_list() + worker_list() + test_ping.PONG)
                client.sendall(test_ping.REGISTER)
                self.assert_closed(client)
            with self.connect() as client:
                client.sendall(struct.pack('!IHHI', 0x464c494e, 1, kind, 1))
                self.assert_closed(client)
        self.assertEqual(Path(self.wal).read_bytes(), before)
        with self.connect() as client:
            client.sendall(frame(17) + submit() + frame(15))
            self.assertEqual(test_ping.receive_exact(client, 20), worker_list())
            self.assertEqual(self.receive_ack(client), 1)
            self.assertEqual(test_ping.receive_exact(client, 54), job_list(job_row(1)))
        for kind in (15, 17):
            with self.connect() as worker:
                worker_id = self.register_worker(worker)
                if kind == 15:
                    self.receive_assignment(worker, worker_id, 1)
                worker.sendall(frame(kind))
                self.assert_closed(worker)

    def test_real_worker_switches_from_busy_to_idle(self):
        job = self.accepted_id(self.submit_cli('sleep', '--args', '1000'))
        with self.worker_process(interval_ms=80) as (worker, output, errors):
            worker_id = self.wait_for_registration(worker, output, errors)
            self.wait_job_event('job_started', job)
            active = table(self, cli('workers', self.port))[0]
            self.assertEqual((active['WORKER_ID'], active['LIVENESS'], active['JOB_ID']), (str(worker_id), 'ALIVE', str(job)))
            self.wait_job_event('job_completed', job)
            self.assertEqual(table(self, cli('workers', self.port))[0]['JOB_ID'], 'none')
            self.assertEqual(table(self, cli('jobs', self.port))[0]['RESULT_BYTES'], '13')
            self.stop_worker(worker, errors)


class ListingExpiryTests(JobProcessTestCase):
    COORDINATOR_ARGUMENTS = ('--heartbeat-timeout-ms', '500')

    def test_listing_polling_does_not_postpone_worker_expiry(self):
        job = self.accepted_id(self.submit_cli('hash', '--args', 'abc', '--max-retries', '1'))
        with self.connect() as worker:
            worker_id = self.register_worker(worker)
            self.receive_assignment(worker, worker_id, job)
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                result = cli('workers', self.port)
                self.assertIn('heartbeat_timeout_ms=500', result.stdout)
                row = table(self, result)[0]
                if row['LIVENESS'] == 'DEAD':
                    break
                self.assertIn(row['LIVENESS'], ('ALIVE', 'EXPIRED'))
                time.sleep(.02)
            else:
                self.fail('Listing queries postponed expiry')
            self.assertEqual(row['JOB_ID'], 'none')
            self.assertEqual(self.wait_for_worker_event('worker_dead', worker_id)['reason'], 'heartbeat_timeout')
            self.assertEqual(table(self, cli('jobs', self.port))[0]['FAILURE'], 'WORKER_LOST')


class ListingRestartTests(persistence.PersistenceTestCase):
    def test_jobs_recover_but_worker_history_does_not(self):
        server = self.server().ready()
        worker = self.connect(server)
        worker_id = self.register(worker)
        self.assertEqual(self.submit(server), 1)
        self.assigned(worker, 1, worker_id)
        self.complete(worker, 1, worker_id, result=b'saved')
        server.event('job_completed job_id=1 ')
        self.assertEqual(self.submit(server), 2)
        self.assigned(worker, 2, worker_id)
        before = table(self, cli('jobs', server.port))
        server.stop(crash=True)
        server = self.server(initialize=False, port=server.port).ready()
        saved = self.path.read_bytes()
        after = table(self, cli('jobs', server.port))
        self.assertEqual(after[0], before[0])
        self.assertEqual((after[1]['STATE'], after[1]['RETRIES'], after[1]['FAILURE']), ('QUEUED', '1/1', 'WORKER_LOST'))
        self.assertEqual(table(self, cli('workers', server.port)), [])
        self.assertEqual(self.path.read_bytes(), saved)
        replacement = self.connect(server)
        self.assertEqual(self.register(replacement), worker_id + 1)
        self.assigned(replacement, 2, worker_id + 1, attempt=2)
        self.assertEqual(table(self, cli('workers', server.port))[0]['WORKER_ID'], str(worker_id + 1))
        server.stop()


class ListingCliTests(unittest.TestCase):
    @contextmanager
    def peer(self, command):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); listener.listen(1); listener.settimeout(3)
            with tempfile.TemporaryFile(mode='w+') as output, tempfile.TemporaryFile(mode='w+') as errors:
                process = subprocess.Popen([str((test_ping.BIN_DIR / 'faultline').resolve()), command,
                    '--coordinator', f'127.0.0.1:{listener.getsockname()[1]}'], stdout=output, stderr=errors)
                try:
                    with listener.accept()[0] as connection:
                        connection.settimeout(2)
                        self.assertEqual(test_ping.receive_exact(connection, 12), frame(15 if command == 'jobs' else 17))
                        yield connection, process, output, errors
                finally:
                    if process.poll() is None: process.kill()
                    process.wait(timeout=2)
                    self.assertNotIn('AddressSanitizer', read_output(errors))
                    self.assertNotIn('runtime error:', read_output(errors))

    def test_maximum_lists_fragmented_and_integer_boundaries(self):
        jobs = job_list(*(job_row(2**64 - 256 + i, task=i % 4 + 1, state=4, worker=2**32 - 1,
                                 attempt=2**32, retry=2**32 - 1, limit=2**32 - 1, size=1024) for i in range(256)))
        workers = worker_list(*(struct.pack(WORKER_ROW, i + 1, 2 if i % 3 == 0 else 1,
                                2**64 - 1 if i % 3 == 0 else 6000 if i % 3 == 1 else 0, 0, 0) for i in range(64)))
        for name, message, count in [('jobs', jobs, 256), ('workers', workers, 64)]:
            with self.peer(name) as (connection, process, output, errors):
                offset = 0
                for end in (1, 11, 12, 15, 16, 19, 20, len(message) - 1):
                    connection.sendall(message[offset:end]); offset = end
                    time.sleep(.02)
                    self.assertIsNone(process.poll())
                    self.assertEqual(read_output(output), '')
                connection.sendall(message[offset:])
                self.assertEqual(process.wait(timeout=2), 0, read_output(errors))
                lines = read_output(output).splitlines()
                self.assertEqual(len(lines), count + 2)
                if name == 'jobs':
                    self.assertIn('18446744073709551615', lines[-1])
                    self.assertIn('4294967296', lines[-1])
                    self.assertTrue(all(task in read_output(output) for task in ('sleep', 'prime_count', 'fibonacci', 'hash')))
                else:
                    self.assertEqual([line.split()[1] for line in lines[2:5]], ['DEAD', 'EXPIRED', 'ALIVE'])
                    self.assertIn('18446744073709551615', lines[2])

    def test_malformed_counts_rows_and_wrong_response_types(self):
        for name, response_kind, stride, prefix in [('jobs', 16, 38, 4), ('workers', 18, 30, 8)]:
            valid = job_list(job_row(1)) if name == 'jobs' else worker_list(struct.pack(WORKER_ROW, 1, 1, 1, 0, 0))
            malformed = [frame(2), frame(response_kind, b'x' * (prefix - 1))[:12],
                         struct.pack('!IHHI', 0x464c494e, 1, response_kind, 10000),
                         valid[:12] + struct.pack('!I', 0xffffffff) + valid[16:12 + prefix],
                         valid[:12] + struct.pack('!I', 0) + valid[16:12 + prefix]]
            if name == 'jobs':
                malformed += [job_list(job_row(0)), job_list(job_row(1, task=0)), job_list(job_row(1, state=6)),
                              job_list(job_row(1), job_row(1)), job_list(job_row(2), job_row(1))]
            else:
                malformed += [worker_list(timeout=0), worker_list(struct.pack(WORKER_ROW, 0, 1, 0, 0, 0)),
                              worker_list(struct.pack(WORKER_ROW, 1, 3, 0, 0, 0)),
                              worker_list(struct.pack(WORKER_ROW, 1, 2, 0, 42, 1)),
                              worker_list(struct.pack(WORKER_ROW, 1, 1, 0, 42, 0)),
                              worker_list(struct.pack(WORKER_ROW, 1, 1, 0, 0, 0), struct.pack(WORKER_ROW, 1, 1, 0, 0, 0))]
            for message in malformed:
                with self.subTest(name=name, message=message.hex()), self.peer(name) as (connection, process, output, errors):
                    connection.sendall(message)
                    self.assertEqual(process.wait(timeout=2), 1)
                    self.assertEqual(read_output(output), '')
                    self.assertIn('faultline:', read_output(errors))

    def test_invalid_options_truncated_replies_and_refused_connection(self):
        for name in ('jobs', 'workers'):
            for options in (['1'], ['--coordinator'], ['--unknown', 'x'], ['--coordinator', 'localhost:9000'],
                            ['--coordinator', '127.0.0.1:0'], ['--coordinator', '127.0.0.1:65536'],
                            ['--coordinator', '127.0.0.1:9000', '--coordinator', '127.0.0.1:9000']):
                result = subprocess.run([str((test_ping.BIN_DIR / 'faultline').resolve()), name, *options],
                                        capture_output=True, text=True, timeout=2)
                self.assertEqual((result.returncode, result.stdout), (1, ''))
                self.assertIn('Usage:', result.stderr)
            message = job_list(job_row(1)) if name == 'jobs' else worker_list(struct.pack(WORKER_ROW, 1, 1, 0, 0, 0))
            for cut in (0, 1, 11, 12, 15, 16, 19, 20, len(message) - 1):
                with self.peer(name) as (connection, process, output, errors):
                    connection.sendall(message[:cut]); connection.shutdown(socket.SHUT_WR)
                    self.assertEqual(process.wait(timeout=2), 1)
                    self.assertEqual(read_output(output), '')
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                result = cli(name, reservation.getsockname()[1])
                self.assertEqual((result.returncode, result.stdout), (1, ''))
                self.assertIn('connect', result.stderr)

    def test_one_deadline_for_entire_listing(self):
        for name in ('jobs', 'workers'):
            message = job_list(job_row(1)) if name == 'jobs' else worker_list(struct.pack(WORKER_ROW, 1, 1, 0, 0, 0))
            prefix = 16 if name == 'jobs' else 20
            with self.peer(name) as (connection, process, output, errors):
                start = time.monotonic()
                time.sleep(2); connection.sendall(message[:12])
                time.sleep(2); connection.sendall(message[12:prefix])
                self.assertEqual(process.wait(timeout=1.9), 1)
                self.assertLess(time.monotonic() - start, 5.9)
                self.assertEqual(read_output(output), '')
                self.assertIn(f'{name} response incomplete', read_output(errors))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=Path('build/debug'))
    parser.add_argument('--port', type=int, default=None)
    args, remaining = parser.parse_known_args()
    test_ping.BIN_DIR = persistence.BIN_DIR = args.bin_dir
    test_ping.TEST_PORT = args.port
    unittest.main(argv=[__file__, *remaining], verbosity=2)
