"""Recover every job state and fence old connections after coordinator restart."""
import argparse
from pathlib import Path
import struct
import unittest

import test_persistence as persistence
import test_ping
from test_scheduling import frame, identity


class StartupRecoveryTests(persistence.PersistenceTestCase):
    def restart(self, server):
        # Reuse the exact endpoint; changing the port would not test old TCP connections.
        port = server.port
        server.stop(crash=True)
        return self.server(initialize=False, port=port).ready()

    def fail_task(self, worker, job, worker_id, attempt=1):
        worker.sendall(frame(11, identity(job, worker_id, attempt) + struct.pack('!H', 1)))

    def test_mixed_history_preserves_terminal_jobs_and_orders_recovered_retries(self):
        server = self.server().ready()
        first = self.connect(server)
        a = self.register(first)
        self.assertEqual(self.submit(server), 1)
        self.assigned(first, 1, a)
        result = b'completed\x00\xffresult'
        self.complete(first, 1, a, result)
        server.event('job_completed job_id=1')
        self.assertEqual(self.submit(server, retries=0), 2)
        self.assigned(first, 2, a)
        self.fail_task(first, 2, a)
        server.event('job_failed job_id=2')

        # Each active state appears with zero, remaining, and exhausted allowance.
        cases = [(3, False, 0, 0), (4, True, 0, 0),
                 (5, False, 2, 0), (6, True, 2, 1),
                 (7, False, 1, 1), (8, True, 1, 1)]
        for job, running, budget, used in cases:
            worker = first if job == 3 else self.connect(server)
            worker_id = a if job == 3 else self.register(worker)
            self.assertEqual(self.submit(server, retries=budget), job)
            self.assigned(worker, job, worker_id)
            if used:
                self.fail_task(worker, job, worker_id)
                self.assigned(worker, job, worker_id, attempt=2)
            if running:
                worker.sendall(frame(9, identity(job, worker_id, used + 1)))
                server.event(f'job_started job_id={job} ')
        self.assertEqual(self.submit(server, retries=0), 9)
        self.assertEqual(self.submit(server), 10)
        before_records, _ = persistence.read_wal(self.path)
        before = self.latest()
        self.assertEqual([before[j]['state'] for j in range(1, 11)], [4, 5, 2, 3, 2, 3, 2, 3, 1, 1])
        base = max(record['updated'] for record in before.values())

        server = self.restart(server)
        self.assertIn('interrupted=6', server.output)
        records, _ = persistence.read_wal(self.path)
        changes = records[len(before_records):]
        self.assertEqual([(r['job'], r['kind']) for r in changes], [(3, 7), (4, 7), (5, 6), (6, 6), (7, 7), (8, 7)])
        after = self.latest()
        for job in (1, 2, 9, 10):
            self.assertEqual(after[job], before[job])  # Every field, payload and record sequence.
        for job, _, budget, used in cases:
            recovered, old = after[job], before[job]
            self.assertEqual((recovered['attempt'], recovered['max_retries'], recovered['failure']), (used + 1, budget, 2))
            self.assertEqual((recovered['arguments'], recovered['created']), (old['arguments'], old['created']))
            self.assertEqual(recovered['updated'], base)
            if used < budget:
                self.assertEqual((recovered['state'], recovered['retry'], recovered['worker']), (1, used + 1, 0))
                self.assertEqual([recovered[t] for t in ('assigned', 'started', 'finished')], [2**64 - 1] * 3)
            else:
                self.assertEqual((recovered['state'], recovered['retry'], recovered['worker']), (5, used, old['worker']))
                self.assertEqual((recovered['assigned'], recovered['started'], recovered['finished']),
                                 (old['assigned'], old['started'], base))
        self.assertNotIn('job_assigned', server.output)
        saved = self.path.read_bytes()
        server = self.restart(server)
        self.assertIn('interrupted=0', server.output)
        self.assertEqual(self.path.read_bytes(), saved)

        worker = self.connect(server)
        fresh_id = self.register(worker)
        self.assertEqual(fresh_id, 7)
        for job, attempt in [(9, 1), (10, 1), (5, 2), (6, 3)]:
            self.assigned(worker, job, fresh_id, attempt=attempt)
            self.complete(worker, job, fresh_id, attempt=attempt)
        server.event('job_completed job_id=6 ')
        self.assertEqual(self.latest()[1]['result'], result)
        for job in (2, 3, 4, 7, 8):
            self.assertEqual(self.latest()[job]['state'], 5)
        server.stop()

    def test_same_endpoint_requires_new_tcp_connection_and_registration(self):
        server = self.server().ready()
        old_connection = self.connect(server)
        old_id = self.register(old_connection)
        job = self.submit(server)
        self.assigned(old_connection, job, old_id)
        old_connection.sendall(frame(9, identity(job, old_id)))
        server.event('job_started job_id=1')
        server = self.restart(server)
        self.closed(old_connection)
        before = self.path.read_bytes()
        # Knowing the old identity on a new TCP connection grants no authority.
        stale_frames = [test_ping.heartbeat(old_id),
                        frame(10, identity(job, old_id) + struct.pack('!I', 3) + b'old')]
        for stale in stale_frames:
            connection = self.connect(server)
            connection.sendall(stale)
            self.closed(connection)
            self.assertEqual(self.path.read_bytes(), before)
        self.assertNotIn('job_assigned', server.output)
        replacement = self.connect(server)
        fresh_id = self.register(replacement)
        self.assertEqual(fresh_id, old_id + 1)
        self.assigned(replacement, job, fresh_id, attempt=2)
        self.complete(replacement, job, fresh_id, result=b'fresh', attempt=2)
        server.event('job_completed job_id=1')
        saved = self.latest()[job]
        self.assertEqual((saved['state'], saved['worker'], saved['attempt'], saved['retry'], saved['result']),
                         (4, fresh_id, 2, 1, b'fresh'))
        server.stop()

    def exhaust_by_coordinator_crashes(self, running):
        server = self.server().ready()
        worker = self.connect(server)
        worker_id = self.register(worker)
        self.assertEqual(self.submit(server, retries=2), 1)
        for attempt in range(1, 4):
            self.assertEqual(worker_id, attempt)
            self.assigned(worker, 1, worker_id, attempt=attempt)
            if running:
                worker.sendall(frame(9, identity(1, worker_id, attempt)))
                server.event('job_started job_id=1 ')
            server = self.restart(server)
            saved = self.latest()[1]
            self.assertEqual((saved['state'], saved['attempt'], saved['retry'], saved['failure']),
                             (1 if attempt < 3 else 5, attempt, min(attempt, 2), 2))
            before = self.path.read_bytes()
            server = self.restart(server)  # No new attempt: no extra retry or record.
            self.assertEqual(self.path.read_bytes(), before)
            worker = self.connect(server)
            worker_id = self.register(worker)
        self.assertEqual(worker_id, 4)
        worker.sendall(test_ping.PING)
        self.assertEqual(test_ping.receive_exact(worker, 12), test_ping.PONG)
        self.assertEqual(self.submit(server, retries=0), 2)
        self.assigned(worker, 2, worker_id)
        self.complete(worker, 2, worker_id)
        server.event('job_completed job_id=2')
        self.assertEqual((self.latest()[1]['state'], self.latest()[1]['retry'], self.latest()[1]['attempt']), (5, 2, 3))
        server.stop()

    def test_repeated_assigned_interruptions_exhaust_retry_allowance(self):
        self.exhaust_by_coordinator_crashes(running=False)

    def test_repeated_running_interruptions_exhaust_retry_allowance(self):
        self.exhaust_by_coordinator_crashes(running=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=persistence.BIN_DIR)
    args, remaining = parser.parse_known_args()
    persistence.BIN_DIR = args.bin_dir
    unittest.main(argv=[__file__, *remaining], verbosity=2)
