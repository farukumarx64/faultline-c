"""Stats scopes, read-only queries, restart baselines, and CLI framing."""
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

FIELDS = (
    'jobs_submitted_total',
    'jobs_queued',
    'jobs_assigned',
    'jobs_running',
    'jobs_completed_total',
    'jobs_failed_total',
    'job_attempts_total',
    'job_retries_total',
    'completed_latency_avg_ms',
    'workers_retained',
    'workers_alive',
    'workers_expired',
    'workers_dead',
    'workers_busy',
    'workers_idle',
    'session_uptime_ms',
    'session_jobs_submitted',
    'session_jobs_completed',
    'session_jobs_failed',
    'session_job_retries',
    'startup_jobs_recovered',
    'startup_interrupted_jobs',
    'startup_duration_ms',
    'heartbeat_timeout_ms',
)


def stats_reply(**changes):
    values = dict.fromkeys(FIELDS, 0)
    values['heartbeat_timeout_ms'] = 6000
    values.update(changes)
    return frame(20, struct.pack('!24Q', *(values[name] for name in FIELDS)))


def cli(port):
    return subprocess.run([str((test_ping.BIN_DIR / 'faultline').resolve()), 'stats',
                           '--coordinator', f'127.0.0.1:{port}'], capture_output=True, text=True, timeout=8)


def parsed(test, result):
    test.assertEqual((result.returncode, result.stderr), (0, ''), result)
    pairs = [line.split('=', 1) for line in result.stdout.splitlines()]
    test.assertEqual([pair[0] for pair in pairs], [*FIELDS, 'session_completed_per_second'])
    values = {key: float(value) if key == 'session_completed_per_second' else int(value) for key, value in pairs}
    rate = values['session_jobs_completed'] * 1000 / values['session_uptime_ms'] if values['session_uptime_ms'] else 0
    test.assertAlmostEqual(values['session_completed_per_second'], rate, delta=.00051)
    return values


def receive_stats(test, connection):
    kind, payload = receive_frame(connection)
    test.assertEqual((kind, len(payload)), (20, 192))
    return dict(zip(FIELDS, struct.unpack('!24Q', payload)))


def latency_from_wal(path):
    latest = {r['job']: r for r in persistence.read_wal(path)[0] if r['kind'] != 1}
    latencies = [r['finished'] - r['created'] for r in latest.values() if r['state'] == 4]
    return sum(latencies) // len(latencies) if latencies else 0


class StatsTests(JobProcessTestCase):
    def test_empty_default_endpoint_and_read_only_queries(self):
        before = Path(self.wal).read_bytes()
        s = parsed(self, cli(self.port))
        for name in FIELDS:
            if name not in ('session_uptime_ms', 'startup_duration_ms', 'heartbeat_timeout_ms'):
                self.assertEqual(s[name], 0, name)
        self.assertEqual(s['heartbeat_timeout_ms'], 6000)
        time.sleep(.04)
        self.assertGreater(parsed(self, cli(self.port))['session_uptime_ms'], s['session_uptime_ms'])
        if self.port == 9000:
            parsed(self, subprocess.run([self.cli, 'stats'], capture_output=True, text=True, timeout=8))
        self.assertEqual(Path(self.wal).read_bytes(), before)
        self.assertEqual(self.accepted_id(self.submit_cli()), 1)
        with self.connect() as worker:
            self.assertEqual(self.register_worker(worker), 1)

    def test_all_job_states_and_worker_activity(self):
        with self.connect() as a, self.connect() as b, self.connect() as idle:
            first = self.register_worker(a)
            self.assertEqual(self.accepted_id(self.submit_cli('hash', '--args', 'abc')), 1)
            self.receive_assignment(a, first, 1)
            self.complete(a, 1, first)
            self.wait_job_event('job_completed', 1)
            self.assertEqual(self.accepted_id(self.submit_cli('hash', '--args', 'abc')), 2)
            self.receive_assignment(a, first, 2)
            a.sendall(frame(11, identity(2, first) + struct.pack('!H', 1)))
            self.wait_job_event('job_failed', 2)
            self.assertEqual(self.accepted_id(self.submit_cli('hash', '--args', 'abc')), 3)
            self.receive_assignment(a, first, 3)
            a.sendall(frame(9, identity(3, first)))
            self.wait_job_event('job_started', 3)
            second = self.register_worker(b)
            self.assertEqual(self.accepted_id(self.submit_cli('hash', '--args', 'abc')), 4)
            self.receive_assignment(b, second, 4)
            self.assertEqual(self.accepted_id(self.submit_cli('hash', '--args', 'abc')), 5)
            s = parsed(self, cli(self.port))
            self.assertEqual([s[k] for k in FIELDS[:8]], [5, 1, 1, 1, 1, 1, 4, 0])
            self.assertEqual((s['workers_alive'], s['workers_busy'], s['workers_idle']), (2, 2, 0))
            # Finish one active job: the oldest queued job takes its place.
            self.complete(b, 4, second)
            self.receive_assignment(b, second, 5)
            self.complete(b, 5, second)
            self.wait_job_event('job_completed', 5)
            third = self.register_worker(idle)
            before = Path(self.wal).read_bytes()
            s = parsed(self, cli(self.port))
            self.assertEqual((s['workers_retained'], s['workers_busy'], s['workers_idle']), (3, 1, 2))
            self.assertEqual((s['session_jobs_submitted'], s['session_jobs_completed'], s['session_jobs_failed']), (5, 3, 1))
            self.assertEqual(s['completed_latency_avg_ms'], latency_from_wal(self.wal))
            self.assertEqual(Path(self.wal).read_bytes(), before)
            idle.shutdown(socket.SHUT_RDWR)
            self.wait_for_worker_event('worker_dead', third)
            s = parsed(self, cli(self.port))
            self.assertEqual((s['workers_retained'], s['workers_alive'], s['workers_dead']), (3, 2, 1))
            with self.connect() as replacement:
                self.assertGreater(self.register_worker(replacement), third)
                s = parsed(self, cli(self.port))
                self.assertEqual((s['workers_retained'], s['workers_alive'], s['workers_dead']), (3, 3, 0))

    def test_retries_and_terminal_failures_have_distinct_counts(self):
        with self.connect() as worker:
            wid = self.register_worker(worker)
            job = self.accepted_id(self.submit_cli('hash', '--args', 'abc', '--max-retries', '1'))
            self.receive_assignment(worker, wid, job)
            worker.sendall(frame(11, identity(job, wid) + struct.pack('!H', 1)))
            self.receive_assignment(worker, wid, job, attempt=2)
            s = parsed(self, cli(self.port))
            self.assertEqual((s['job_attempts_total'], s['job_retries_total'], s['jobs_failed_total']), (2, 1, 0))
            self.assertEqual((s['session_job_retries'], s['session_jobs_failed']), (1, 0))
            self.complete(worker, job, wid, attempt=2)
            self.wait_job_event('job_completed', job)
            lost = self.accepted_id(self.submit_cli('hash', '--args', 'abc'))
            self.receive_assignment(worker, wid, lost)
        self.wait_job_event('job_worker_lost', lost)
        s = parsed(self, cli(self.port))
        self.assertEqual((s['jobs_completed_total'], s['jobs_failed_total'], s['job_attempts_total'], s['job_retries_total']), (1, 1, 3, 1))
        self.assertEqual((s['session_jobs_failed'], s['session_job_retries']), (1, 1))

    def test_full_store_and_rejected_submission(self):
        with self.connect() as client:
            for job in range(1, 257):
                client.sendall(submit())
                self.assertEqual(self.receive_ack(client), job)
            client.sendall(submit())
            self.assert_closed(client)
        before = Path(self.wal).read_bytes()
        s = parsed(self, cli(self.port))
        self.assertEqual((s['jobs_submitted_total'], s['jobs_queued'], s['session_jobs_submitted']), (256, 256, 256))
        self.assertEqual(Path(self.wal).read_bytes(), before)

    def test_fragmented_coalesced_requests_and_connection_roles(self):
        before = Path(self.wal).read_bytes()
        with self.connect() as client:
            for cut in range(1, 12):
                client.sendall(frame(19)[:cut])
                self.assert_waiting_for_more(client)
                client.sendall(frame(19)[cut:])
                self.assertEqual(receive_stats(self, client)['jobs_submitted_total'], 0)
            client.sendall(frame(19) + test_ping.PING + frame(19))
            receive_stats(self, client)
            self.assertEqual(test_ping.receive_exact(client, 12), test_ping.PONG)
            receive_stats(self, client)
            client.sendall(test_ping.REGISTER)
            self.assert_closed(client)
        for header in (frame(19, b'x')[:12], frame(20, b'x' * 192)[:12]):
            with self.connect() as client:
                client.sendall(header); self.assert_closed(client)
        self.assertEqual(Path(self.wal).read_bytes(), before)
        with self.connect() as client:
            client.sendall(submit() + frame(19))
            self.assertEqual(self.receive_ack(client), 1)
            self.assertEqual(receive_stats(self, client)['jobs_submitted_total'], 1)
        with self.connect() as worker:
            wid = self.register_worker(worker)
            self.receive_assignment(worker, wid, 1)
            worker.sendall(frame(19)); self.assert_closed(worker)


class StatsExpiryTests(JobProcessTestCase):
    COORDINATOR_ARGUMENTS = ('--heartbeat-timeout-ms', '500')

    def test_stats_polling_does_not_renew_heartbeat(self):
        job = self.accepted_id(self.submit_cli('hash', '--args', 'abc', '--max-retries', '1'))
        with self.connect() as worker:
            wid = self.register_worker(worker)
            self.receive_assignment(worker, wid, job)
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                s = parsed(self, cli(self.port))
                self.assertEqual(s['heartbeat_timeout_ms'], 500)
                if s['workers_dead'] == 1:
                    break
                time.sleep(.02)
            else:
                self.fail('Stats polling postponed expiry')
            self.assertEqual((s['workers_alive'], s['workers_busy'], s['jobs_queued'], s['session_job_retries']), (0, 0, 1, 1))
            self.assertEqual(self.wait_for_worker_event('worker_dead', wid)['reason'], 'heartbeat_timeout')


class StatsRestartTests(persistence.PersistenceTestCase):
    def test_recovery_baselines_terminal_outcomes_and_resumed_work(self):
        server = self.server().ready()
        worker = self.connect(server); a = self.register(worker)
        self.assertEqual(self.submit(server, retries=0), 1)
        self.assigned(worker, 1, a); self.complete(worker, 1, a)
        server.event('job_completed job_id=1 ')
        self.assertEqual(self.submit(server, retries=0), 2)
        self.assigned(worker, 2, a)
        worker.sendall(frame(11, identity(2, a) + struct.pack('!H', 1)))
        server.event('job_failed job_id=2 ')
        self.assertEqual(self.submit(server), 3)
        self.assigned(worker, 3, a); worker.sendall(frame(9, identity(3, a)))
        server.event('job_started job_id=3 ')
        second = self.connect(server); b = self.register(second)
        self.assertEqual(self.submit(server, retries=0), 4); self.assigned(second, 4, b)
        self.assertEqual(self.submit(server), 5)
        before = parsed(self, cli(server.port))
        server.stop(crash=True)
        time.sleep(.1)
        server = self.server(initialize=False).ready()
        saved = self.path.read_bytes()
        after = parsed(self, cli(server.port))
        self.assertEqual([after[k] for k in FIELDS[:8]], [5, 2, 0, 0, 1, 2, 4, 1])
        self.assertEqual((after['startup_jobs_recovered'], after['startup_interrupted_jobs']), (5, 2))
        self.assertEqual(after['completed_latency_avg_ms'], before['completed_latency_avg_ms'])
        self.assertTrue(all(after[k] == 0 for k in FIELDS[9:15] + FIELDS[16:20]))
        self.assertEqual(self.path.read_bytes(), saved)
        # A second startup must not charge the same interrupted attempts again.
        server.stop(crash=True); server = self.server(initialize=False).ready()
        again = parsed(self, cli(server.port))
        self.assertEqual([again[k] for k in FIELDS[:9]], [after[k] for k in FIELDS[:9]])
        self.assertEqual(again['startup_interrupted_jobs'], 0)
        replacement = self.connect(server); c = self.register(replacement)
        self.assertGreater(c, b)
        self.assigned(replacement, 5, c); self.complete(replacement, 5, c)
        self.assigned(replacement, 3, c, attempt=2); self.complete(replacement, 3, c, attempt=2)
        server.event('job_completed job_id=3 ')
        self.assertEqual(self.submit(server), 6); self.assigned(replacement, 6, c)
        resumed = parsed(self, cli(server.port))
        self.assertEqual((resumed['jobs_submitted_total'], resumed['jobs_completed_total'], resumed['jobs_failed_total']), (6, 3, 2))
        self.assertEqual((resumed['session_jobs_submitted'], resumed['session_jobs_completed'],
                          resumed['session_jobs_failed'], resumed['session_job_retries']), (1, 2, 0, 0))
        self.assertEqual(resumed['completed_latency_avg_ms'], latency_from_wal(self.path))
        server.stop()


class StatsCliTests(unittest.TestCase):
    @contextmanager
    def peer(self):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); listener.listen(1); listener.settimeout(3)
            with tempfile.TemporaryFile(mode='w+') as output, tempfile.TemporaryFile(mode='w+') as errors:
                process = subprocess.Popen([str((test_ping.BIN_DIR / 'faultline').resolve()), 'stats',
                    '--coordinator', f'127.0.0.1:{listener.getsockname()[1]}'], stdout=output, stderr=errors)
                try:
                    with listener.accept()[0] as connection:
                        connection.settimeout(2)
                        self.assertEqual(test_ping.receive_exact(connection, 12), frame(19))
                        yield connection, process, output, errors
                finally:
                    if process.poll() is None: process.kill()
                    process.wait(timeout=2)
                    self.assertNotIn('AddressSanitizer', read_output(errors))
                    self.assertNotIn('runtime error:', read_output(errors))

    def test_fragmented_response_large_counters_and_zero_uptime(self):
        message = stats_reply(jobs_submitted_total=1, jobs_completed_total=1, job_attempts_total=2**32,
                              job_retries_total=2**32-1, completed_latency_avg_ms=2**63-1,
                              session_jobs_submitted=1, session_jobs_completed=1, session_job_retries=2**32-1,
                              session_uptime_ms=2**63-1)
        for response in (message, stats_reply()):
            with self.peer() as (connection, process, output, errors):
                offset = 0
                for end in (1, 11, 12, 13, 80, 203):
                    connection.sendall(response[offset:end]); offset = end; time.sleep(.02)
                    self.assertIsNone(process.poll()); self.assertEqual(read_output(output), '')
                connection.sendall(response[offset:])
                self.assertEqual(process.wait(timeout=2), 0, read_output(errors))
                result = subprocess.CompletedProcess([], 0, read_output(output), read_output(errors))
                s = parsed(self, result)
                if response == message:
                    self.assertEqual((s['job_attempts_total'], s['completed_latency_avg_ms']), (2**32, 2**63-1))
                else: self.assertEqual(s['session_completed_per_second'], 0)

    def test_invalid_replies_fail_without_partial_output(self):
        malformed = [frame(2), frame(18, b'\0' * 8), frame(20, b'x' * 191)[:12],
                     frame(20, b'x' * 193)[:12], stats_reply(heartbeat_timeout_ms=0),
                     stats_reply(jobs_submitted_total=2**64-1), stats_reply(jobs_queued=1),
                     stats_reply(job_attempts_total=1), stats_reply(job_retries_total=2**64-1),
                     stats_reply(workers_alive=1), stats_reply(workers_retained=1),
                     stats_reply(workers_retained=1, workers_alive=1, workers_busy=1),
                     stats_reply(session_jobs_completed=1), stats_reply(session_job_retries=1),
                     stats_reply(startup_interrupted_jobs=1), stats_reply(completed_latency_avg_ms=1)]
        for message in malformed:
            with self.subTest(message=message.hex()), self.peer() as (connection, process, output, errors):
                connection.sendall(message)
                self.assertEqual(process.wait(timeout=2), 1)
                self.assertEqual(read_output(output), '')
                self.assertIn('stats response', read_output(errors))

    def test_options_truncation_and_connection_refusal(self):
        for options in (['1'], ['--coordinator'], ['--unknown', 'x'], ['--coordinator', 'localhost:9000'],
                        ['--coordinator', '127.0.0.1:0'], ['--coordinator', '127.0.0.1:65536'],
                        ['--coordinator', '127.0.0.1:9000', '--coordinator', '127.0.0.1:9000']):
            result = subprocess.run([str((test_ping.BIN_DIR / 'faultline').resolve()), 'stats', *options],
                                    capture_output=True, text=True, timeout=2)
            self.assertEqual((result.returncode, result.stdout), (1, ''))
            self.assertIn('Usage:', result.stderr)
        for cut in (0, 1, 11, 12, 13, 80, 203):
            with self.peer() as (connection, process, output, errors):
                connection.sendall(stats_reply()[:cut]); connection.shutdown(socket.SHUT_WR)
                self.assertEqual(process.wait(timeout=2), 1)
                self.assertEqual(read_output(output), '')
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            result = cli(reservation.getsockname()[1])
            self.assertEqual((result.returncode, result.stdout), (1, ''))
            self.assertIn('connect', result.stderr)

    def test_one_deadline_covers_header_and_counters(self):
        with self.peer() as (connection, process, output, errors):
            start = time.monotonic()
            time.sleep(2); connection.sendall(stats_reply()[:12])
            time.sleep(2); connection.sendall(stats_reply()[12:80])
            self.assertEqual(process.wait(timeout=1.9), 1)
            self.assertLess(time.monotonic() - start, 5.9)
            self.assertEqual(read_output(output), '')
            self.assertIn('stats response incomplete', read_output(errors))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=Path('build/debug'))
    parser.add_argument('--port', type=int, default=None)
    args, remaining = parser.parse_known_args()
    test_ping.BIN_DIR = persistence.BIN_DIR = args.bin_dir
    test_ping.TEST_PORT = args.port
    unittest.main(argv=[__file__, *remaining], verbosity=2)
