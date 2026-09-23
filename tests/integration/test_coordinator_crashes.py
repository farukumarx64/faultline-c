"""SIGKILL at WAL boundaries, then recovery with the ordinary coordinator."""
import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import select
import signal
import struct
import subprocess
import tempfile
import unittest

import test_persistence as persistence
import test_ping
from test_scheduling import frame, identity, submit
from test_worker import read_output

POINTS = ('before_write', 'partial_header', 'partial_payload', 'before_sync', 'after_sync')
COMPLETE_POINTS = ('before_sync', 'after_sync')


class CoordinatorCrashTests(persistence.PersistenceTestCase):
    def crash_server(self, kind, point, occurrence=1, **options):
        environment = dict(os.environ, FAULTLINE_TEST_WAL_KIND=str(kind),
                           FAULTLINE_TEST_WAL_POINT=point,
                           FAULTLINE_TEST_WAL_OCCURRENCE=str(occurrence))
        executable = (persistence.BIN_DIR / 'tests/crash-coordinator').resolve()
        server = persistence.Server(executable, self.directory, self.path,
                                    initialize=False, env=environment, **options)
        self.addCleanup(server.close)
        return server

    def boundary(self, server, kind, point, occurrence=1):
        server.event(f'[TEST] crash_boundary kind={kind} point={point} occurrence={occurrence}\n')

    def restart(self, server):
        server.stop(crash=True)
        return self.server(initialize=False, port=server.port).ready()

    def fail_task(self, worker, job, worker_id):
        worker.sendall(frame(11, identity(job, worker_id) + struct.pack('!H', 1)))

    def seed_terminals(self, queued, retries):
        server = self.server().ready()
        worker = self.connect(server)
        self.assertEqual(self.register(worker), 1)
        self.assertEqual(self.submit(server), 1)
        self.assigned(worker, 1, 1)
        self.complete(worker, 1, 1, bytes(range(256)) * 4)
        server.event('job_completed job_id=1 ')
        self.assertEqual(self.submit(server, retries=0), 2)
        self.assigned(worker, 2, 1)
        self.fail_task(worker, 2, 1)
        server.event('job_failed job_id=2 ')
        worker.close()
        server.event('worker_dead worker_id=1 ')
        if queued:
            self.assertEqual(self.submit(server, retries=retries), 3)
        saved = self.latest()
        prefix = self.path.read_bytes()
        server.stop(crash=True)
        return server.port, saved, prefix

    def live_matrix(self, kind):
        for point in POINTS:
            with self.subTest(point=point):
                self.path = self.directory / f'{kind}-{point}.wal'
                retries = 0 if kind == 7 else 2
                port, saved, prefix = self.seed_terminals(queued=kind >= 3, retries=retries)
                server = self.crash_server(kind, point, port=port).ready()
                connection = self.connect(server)
                if kind == 1:
                    connection.sendall(test_ping.REGISTER)
                elif kind == 2:
                    connection.sendall(submit(retries=retries))
                else:
                    self.assertEqual(self.register(connection), 2)
                    if kind >= 4:
                        self.assigned(connection, 3, 2)
                        connection.sendall(frame(9, identity(3, 2)))
                    if kind >= 5:
                        server.event('job_started job_id=3 ')
                        if kind == 5:
                            result = b'new\x00\xffresult'
                            connection.sendall(frame(10, identity(3, 2) + struct.pack('!I', len(result)) + result))
                        else:
                            self.fail_task(connection, 3, 2)
                self.boundary(server, kind, point)
                # The marker is inside the write/flush, before public state,
                # ACK, assignment, accepted-result logging, or retry dispatch.
                self.assertEqual(select.select([connection], [], [], 0)[0], [])
                event = {1: 'worker_registered worker_id=2 ', 2: 'job_submitted job_id=3 ',
                         3: 'job_assigned job_id=3 ', 4: 'job_started job_id=3 ',
                         5: 'job_completed job_id=3 ', 6: 'job_failed job_id=3 ',
                         7: 'job_failed job_id=3 '}[kind]
                self.assertNotIn(event, server.output)
                frozen = self.path.read_bytes()
                self.assertTrue(frozen.startswith(prefix))
                records, tail = persistence.read_wal(self.path, allow_tail=True)
                complete = point in COMPLETE_POINTS
                if point == 'partial_header':
                    self.assertEqual(tail, 7)
                elif point == 'partial_payload':
                    # A whole header and all but the final payload byte.
                    payload_size, = struct.unpack_from('!I', frozen, len(frozen) - tail + 8)
                    self.assertEqual(tail, 32 + payload_size - 1)
                else:
                    self.assertEqual(tail, 0)
                if complete:
                    self.assertEqual(records[-1]['kind'], kind)
                server = self.restart(server)
                self.assertIn(f'repaired_bytes={tail}', server.output)
                self.assertTrue(self.path.read_bytes().startswith(frozen[:-tail] if tail else frozen))
                recovered = self.latest()
                for job in (1, 2):
                    self.assertEqual(recovered[job], saved[job])
                if kind == 1 or (kind == 2 and not complete):
                    self.assertNotIn(3, recovered)
                else:
                    job = recovered[3]
                    terminal = (kind == 5 and complete) or kind == 7
                    expected_state = (4 if kind == 5 else 5) if terminal else 1
                    attempt = 0 if kind == 2 or (kind == 3 and not complete) else 1
                    retry = 1 if attempt and not terminal else 0
                    self.assertEqual((job['state'], job['attempt'], job['retry'], job['max_retries']),
                                     (expected_state, attempt, retry, retries))
                    self.assertEqual(job['arguments'], b'abc')
                    if terminal and complete:
                        self.assertEqual(job, records[-1])  # Exact terminal metadata and binary result.
                    elif attempt:
                        self.assertEqual(job['failure'], 1 if kind == 6 and complete else 2)

                replacement = self.connect(server)
                worker_id = self.register(replacement)
                self.assertEqual(worker_id, 2 if kind == 2 or (kind == 1 and not complete) else 3)
                if 3 in recovered and recovered[3]['state'] == 1:
                    attempt = recovered[3]['attempt'] + 1
                    self.assigned(replacement, 3, worker_id, attempt=attempt)
                    self.complete(replacement, 3, worker_id, attempt=attempt)
                    server.event('job_completed job_id=3 ')
                # A new job proves allocator continuity and that terminal jobs
                # were not accidentally dispatched to the replacement worker.
                next_id = 4 if 3 in recovered else 3
                self.assertEqual(self.submit(server, retries=0), next_id)
                self.assigned(replacement, next_id, worker_id)
                self.complete(replacement, next_id, worker_id)
                server.event(f'job_completed job_id={next_id} ')
                final = self.path.read_bytes()
                server = self.restart(server)
                self.assertEqual(self.path.read_bytes(), final)
                self.assertIn('interrupted=0', server.output)
                server.stop()

    def test_worker_identity_crash_boundaries(self):
        self.live_matrix(1)

    def test_submission_crash_boundaries(self):
        self.live_matrix(2)

    def test_assignment_crash_boundaries(self):
        self.live_matrix(3)

    def test_started_crash_boundaries(self):
        self.live_matrix(4)

    def test_completion_crash_boundaries(self):
        self.live_matrix(5)

    def test_retry_crash_boundaries(self):
        self.live_matrix(6)

    def test_terminal_failure_crash_boundaries(self):
        self.live_matrix(7)

    def test_crash_during_first_and_second_startup_reconciliation(self):
        for occurrence in (1, 2):
            for point in POINTS:
                with self.subTest(occurrence=occurrence, point=point):
                    self.path = self.directory / f'recovery-{occurrence}-{point}.wal'
                    server = self.server().ready()
                    for job in (1, 2, 3):
                        worker = self.connect(server)
                        self.assertEqual(self.register(worker), job)
                        self.assertEqual(self.submit(server, retries=0 if job == 3 else 2), job)
                        self.assigned(worker, job, job)
                        if job != 2:
                            worker.sendall(frame(9, identity(job, job)))
                            server.event(f'job_started job_id={job} ')
                    self.assertEqual(self.submit(server), 4)
                    server.stop(crash=True)
                    server = self.crash_server(6, point, occurrence, port=server.port)
                    self.boundary(server, 6, point, occurrence)
                    self.assertNotIn('coordinator listening', server.output)
                    with self.assertRaises(ConnectionRefusedError):
                        with server.connect():
                            self.fail('Recovery opened a listener before reconciliation finished')
                    records, tail = persistence.read_wal(self.path, allow_tail=True)
                    frozen = self.path.read_bytes()
                    self.assertEqual(sum(r['kind'] == 6 for r in records),
                                     occurrence - 1 + (point in COMPLETE_POINTS))
                    server = self.restart(server)
                    self.assertIn(f'repaired_bytes={tail}', server.output)
                    self.assertTrue(self.path.read_bytes().startswith(frozen[:-tail] if tail else frozen))
                    records, _ = persistence.read_wal(self.path)
                    self.assertEqual([(r['job'], r['kind']) for r in records if r['kind'] in (6, 7)],
                                     [(1, 6), (2, 6), (3, 7)])
                    for job in (1, 2):
                        self.assertEqual((self.latest()[job]['state'], self.latest()[job]['retry']), (1, 1))
                    self.assertEqual((self.latest()[3]['state'], self.latest()[3]['retry']), (5, 0))
                    saved = self.path.read_bytes()
                    server = self.restart(server)
                    self.assertEqual(self.path.read_bytes(), saved)
                    replacement = self.connect(server)
                    self.assertEqual(self.register(replacement), 4)
                    for job, attempt in ((4, 1), (1, 2), (2, 2)):
                        self.assigned(replacement, job, 4, attempt=attempt)
                        self.complete(replacement, job, 4, attempt=attempt)
                    server.event('job_completed job_id=2 ')
                    server.stop()

    def cli_submit(self, server, task, arguments, retries=1):
        result = subprocess.run([str((persistence.BIN_DIR / 'faultline').resolve()), 'submit', task,
                                 '--args', arguments, '--max-retries', str(retries),
                                 '--coordinator', f'127.0.0.1:{server.port}'],
                                cwd=self.directory, capture_output=True, text=True, timeout=8)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, '')
        self.assertRegex(result.stdout, r'^job_id=[1-9][0-9]*\n$')
        return int(result.stdout.split('=')[1])

    @contextmanager
    def real_worker(self, server):
        with tempfile.TemporaryFile(mode='w+') as output:
            process = subprocess.Popen([str((persistence.BIN_DIR / 'faultline-worker').resolve()),
                                        '--coordinator', f'127.0.0.1:{server.port}',
                                        '--heartbeat-interval-ms', '80'],
                                       cwd=self.directory, stdout=output, stderr=subprocess.STDOUT)
            try:
                yield process
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                try:
                    process.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
                    self.fail('Worker did not stop: ' + read_output(output))
                self.assertNotIn('AddressSanitizer', read_output(output))
                self.assertNotIn('runtime error:', read_output(output))

    def test_cli_acknowledged_jobs_survive_and_execute_all_builtin_tasks(self):
        server = self.server().ready()
        cases = [('sleep', '0', b'slept_ms=0'), ('prime_count', '100', b'25'),
                 ('fibonacci', '10', b'55'), ('hash', 'hello', b'a430d84680aabd0b')]
        for job, (task, arguments, _) in enumerate(cases, 1):
            self.assertEqual(self.cli_submit(server, task, arguments, retries=0), job)
        saved = self.latest()
        server = self.restart(server)
        self.assertEqual(self.latest(), saved)
        with self.real_worker(server):
            server.event('job_completed job_id=4 ')
            for job, (_, _, expected) in enumerate(cases, 1):
                row = self.latest()[job]
                self.assertEqual((row['state'], row['attempt'], row['retry'], row['result']), (4, 1, 0, expected))
            final = self.path.read_bytes()
            server = self.restart(server)
            self.assertEqual(self.path.read_bytes(), final)
        with self.real_worker(server):
            self.assertEqual(self.cli_submit(server, 'fibonacci', '11'), 5)
            server.event('job_completed job_id=5 ')
            self.assertEqual((self.latest()[5]['worker'], self.latest()[5]['result']), (2, b'89'))
            self.assertNotIn('job_assigned job_id=1 ', server.output)
        server.stop()

    def test_running_real_task_resumes_on_fresh_worker_after_coordinator_sigkill(self):
        server = self.server().ready()
        self.assertEqual(self.cli_submit(server, 'sleep', '1500'), 1)
        with self.real_worker(server) as original:
            server.event('job_started job_id=1 ')
            server.stop(crash=True)
            self.assertEqual(self.latest()[1]['state'], 3)
            self.assertEqual(original.wait(timeout=6), 1)  # Lost coordinator, task cancelled.
        server = self.server(initialize=False, port=server.port).ready()
        row = self.latest()[1]
        self.assertEqual((row['state'], row['attempt'], row['retry']), (1, 1, 1))
        with self.real_worker(server):
            server.event('job_completed job_id=1 ')
            row = self.latest()[1]
            self.assertEqual((row['state'], row['worker'], row['attempt'], row['retry'], row['result']),
                             (4, 2, 2, 1, b'slept_ms=1500'))
            final = self.path.read_bytes()
            server = self.restart(server)
            self.assertEqual(self.path.read_bytes(), final)
        server.stop()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=persistence.BIN_DIR)
    args, remaining = parser.parse_known_args()
    persistence.BIN_DIR = args.bin_dir
    unittest.main(argv=[__file__, *remaining], verbosity=2)
