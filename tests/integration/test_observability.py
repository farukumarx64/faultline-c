"""Phase review: compare CLI snapshots, lifecycle logs, and durable history."""
import argparse
from datetime import datetime, timezone
from pathlib import Path
import re
import socket
import struct
import subprocess
import unittest

import test_listings as listings
import test_persistence as persistence
import test_ping
import test_stats as stats
import test_status as status
from test_scheduling import JobProcessTestCase, frame, identity
from test_worker import read_output

STATES = {1: 'QUEUED', 2: 'ASSIGNED', 3: 'RUNNING', 4: 'DONE', 5: 'FAILED'}
TASKS = {1: 'sleep', 2: 'prime_count', 3: 'fibonacci', 4: 'hash'}
FAILURES = {0: 'NONE', 1: 'TASK', 2: 'WORKER_LOST'}


def escaped(data):
    return ''.join(chr(b) if 32 <= b <= 126 and b not in (34, 92) else f'\\x{b:02x}' for b in data)


def records(test, output, pid=None):
    result = []
    for line in output.splitlines(keepends=True):
        if not line.endswith('\n'):  # A live FILE may still be writing the last record.
            continue
        match = re.fullmatch(r'(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z) \[(INFO|WARN|ERROR)\] '
                             r'(coordinator|worker) (\w+) (.*)\n', line)
        test.assertIsNotNone(match, line)
        date = datetime.fromisoformat(match[1].replace('Z', '+00:00'))
        test.assertEqual(date.utcoffset(), timezone.utc.utcoffset(date))
        fields = dict(re.findall(r'(\w+)=("[^"]*"|\S+)', match[5]))
        test.assertGreater(int(fields['pid']), 0)
        test.assertGreaterEqual(int(fields['monotonic_ms']), 0)
        if pid is not None: test.assertEqual(int(fields['pid']), pid)
        result.append(dict(event=match[4], level=match[2], component=match[3], **fields))
    return result


def event(test, output, name, **fields):
    found = [r for r in records(test, output) if r['event'] == name and
             all(r.get(key) == str(value) for key, value in fields.items())]
    test.assertTrue(found, (name, fields, output))
    return found[-1]


def inspect(test, port, path):
    before = Path(path).read_bytes()
    wal, _ = persistence.read_wal(path)
    latest = {r['job']: r for r in wal if r['kind'] != 1}
    rows = listings.table(test, listings.cli('jobs', port))
    workers = listings.table(test, listings.cli('workers', port))
    totals = stats.parsed(test, stats.cli(port))
    test.assertEqual([int(row['JOB_ID']) for row in rows], sorted(latest))
    for row in rows:
        job = latest[int(row['JOB_ID'])]
        expected = dict(job_id=str(job['job']), state=STATES[job['state']],
                        worker_id=str(job['worker']) if job['worker'] else 'none',
                        attempt=str(job['attempt']), retries=f"{job['retry']}/{job['max_retries']}",
                        failure=FAILURES[job['failure']], result_bytes=str(job['result_size']))
        if job['state'] == 4:
            expected['result'] = '"' + escaped(job['result']) + '"'
        test.assertEqual(status.fields(test, status.status_cli(job['job'], port)), expected)
        test.assertEqual(row, dict(JOB_ID=expected['job_id'], TASK=TASKS[job['task']], STATE=expected['state'],
                                  WORKER_ID=expected['worker_id'], ATTEMPT=expected['attempt'],
                                  RETRIES=expected['retries'], FAILURE=expected['failure'],
                                  RESULT_BYTES=expected['result_bytes']))
    for number, key in [(1, 'jobs_queued'), (2, 'jobs_assigned'), (3, 'jobs_running'),
                        (4, 'jobs_completed_total'), (5, 'jobs_failed_total')]:
        test.assertEqual(totals[key], sum(r['state'] == number for r in latest.values()), key)
    test.assertEqual(totals['jobs_submitted_total'], len(latest))
    test.assertEqual(totals['job_attempts_total'], sum(r['attempt'] for r in latest.values()))
    test.assertEqual(totals['job_retries_total'], sum(r['retry'] for r in latest.values()))
    test.assertEqual(totals['completed_latency_avg_ms'], stats.latency_from_wal(path))
    test.assertEqual(totals['workers_retained'], len(workers))
    for state in ('ALIVE', 'EXPIRED', 'DEAD'):
        test.assertEqual(totals['workers_' + state.lower()], sum(r['LIVENESS'] == state for r in workers))
    busy = 0
    for worker in workers:
        if worker['JOB_ID'] != 'none':
            job = latest[int(worker['JOB_ID'])]
            test.assertIn(job['state'], (2, 3))
            test.assertEqual((int(worker['WORKER_ID']), int(worker['ATTEMPT'])), (job['worker'], job['attempt']))
            if worker['LIVENESS'] == 'ALIVE': busy += 1
        elif worker['LIVENESS'] == 'DEAD':
            test.assertEqual(worker['ATTEMPT'], '0')
    test.assertEqual(totals['workers_busy'], busy)
    test.assertEqual(totals['workers_idle'], totals['workers_alive'] - busy)
    test.assertEqual(Path(path).read_bytes(), before, 'Inspection changed durable state')
    return latest, totals


class ObservabilitySnapshots(persistence.PersistenceTestCase):
    def ready(self):
        return self.server(extra=('--heartbeat-timeout-ms', '60000')).ready()

    def test_all_states_and_rejected_report_match_commands_and_wal(self):
        server = self.ready()
        inspect(self, server.port, self.path)
        worker = self.connect(server); a = self.register(worker)
        self.submit(server, retries=0); self.assigned(worker, 1, a)
        result = b'line\n"\x00\x1b\\\xff'
        self.complete(worker, 1, a, result=result)
        server.event('job_completed job_id=1 ')
        self.submit(server, retries=0); self.assigned(worker, 2, a)
        worker.sendall(frame(11, identity(2, a) + struct.pack('!H', 1)))
        server.event('job_failed job_id=2 ')
        self.submit(server); self.assigned(worker, 3, a)
        worker.sendall(frame(9, identity(3, a))); server.event('job_started job_id=3 ')
        second = self.connect(server); b = self.register(second)
        self.submit(server, retries=0); self.assigned(second, 4, b)
        self.submit(server)
        latest, totals = inspect(self, server.port, self.path)
        self.assertEqual([r['state'] for r in latest.values()], [4, 5, 3, 2, 1])
        self.assertEqual(totals['session_jobs_submitted'], 5)
        unknown = status.status_cli(999, server.port)
        self.assertEqual((unknown.returncode, unknown.stdout), (2, ''))
        completed = event(self, server.output, 'job_completed', job_id=1)
        self.assertEqual((completed['outcome'], completed['failure'], completed['result']),
                         ('COMPLETED', 'NONE', '"' + escaped(result) + '"'))
        completed_record = persistence.read_wal(self.path)[0][int(completed['wal_sequence']) - 1]
        self.assertEqual((completed_record['job'], completed_record['state'], completed_record['result']), (1, 4, result))
        # A report on the wrong lease must not overwrite job 3. Closing its sender
        # independently fails job 4, whose retry limit is zero.
        second.sendall(frame(10, identity(3, b, 99) + struct.pack('!I', 4) + b'fake'))
        self.closed(second); server.event('job_worker_lost job_id=4 ')
        rejected = event(self, server.output, 'job_report_rejected', job_id=3)
        self.assertEqual((rejected['report_attempt'], rejected['current_attempt'], rejected['current_worker_id']),
                         ('99', '1', str(a)))
        after, _ = inspect(self, server.port, self.path)
        self.assertEqual(after[3], latest[3])
        self.assertEqual(after[4]['state'], 5)
        self.assertNotIn('job_completed job_id=3 ', server.output)
        server.stop()
        shutdown = event(self, server.output, 'shutdown')
        self.assertEqual(shutdown['active_attempts'], '1')
        self.assertEqual(event(self, server.output, 'stopped')['exit_code'], '0')
        records(self, server.output, server.process.pid)

    def test_worker_loss_retry_and_terminal_task_failure_are_distinct(self):
        server = self.ready()
        self.submit(server)
        first = self.connect(server); a = self.register(first); self.assigned(first, 1, a)
        first.close(); server.event('job_worker_lost job_id=1 ')
        latest, totals = inspect(self, server.port, self.path)
        self.assertEqual((latest[1]['state'], totals['job_retries_total'], totals['jobs_failed_total']), (1, 1, 0))
        loss = event(self, server.output, 'job_worker_lost', job_id=1)
        self.assertEqual((loss['outcome'], loss['failure'], loss['worker_id'], loss['previous_worker_id']),
                         ('REQUEUED', 'WORKER_LOST', '0', str(a)))
        replacement = self.connect(server); b = self.register(replacement)
        self.assigned(replacement, 1, b, attempt=2)
        replacement.sendall(frame(11, identity(1, b, 2) + struct.pack('!H', 1)))
        server.event('job_failed job_id=1 ')
        latest, totals = inspect(self, server.port, self.path)
        self.assertEqual((latest[1]['state'], totals['jobs_failed_total'], totals['job_retries_total']), (5, 1, 1))
        failed = event(self, server.output, 'job_failed', job_id=1)
        self.assertEqual((failed['level'], failed['outcome'], failed['failure'], failed['max_retries']),
                         ('WARN', 'FAILED', 'TASK', '1'))
        self.assertEqual(failed['durable'], '1')
        assigned = event(self, server.output, 'job_assigned', job_id=1, attempt=2)
        self.assertGreaterEqual(int(assigned['monotonic_ms']), int(loss['monotonic_ms']))
        self.assertEqual(self.submit(server), 2)
        self.assigned(replacement, 2, b)
        replacement.sendall(frame(11, identity(2, b) + struct.pack('!H', 1)))
        self.assigned(replacement, 2, b, attempt=2)
        retry = event(self, server.output, 'job_failed', job_id=2)
        self.assertEqual((retry['state'], retry['outcome'], retry['failure'], retry['previous_worker_id']),
                         ('QUEUED', 'REQUEUED', 'TASK', str(b)))
        self.complete(replacement, 2, b, attempt=2)
        server.event('job_completed job_id=2 ')
        _, totals = inspect(self, server.port, self.path)
        self.assertEqual((totals['jobs_completed_total'], totals['jobs_failed_total'], totals['job_retries_total']),
                         (1, 1, 2))
        server.stop()

    def test_recovered_snapshots_are_not_new_completion_events(self):
        server = self.ready()
        worker = self.connect(server); a = self.register(worker)
        self.submit(server, retries=0); self.assigned(worker, 1, a)
        self.complete(worker, 1, a, result=b'saved\0\n')
        server.event('job_completed job_id=1 ')
        self.submit(server, retries=0); self.assigned(worker, 2, a)
        worker.sendall(frame(11, identity(2, a) + struct.pack('!H', 1)))
        server.event('job_failed job_id=2 ')
        self.submit(server); self.assigned(worker, 3, a)
        worker.sendall(frame(9, identity(3, a))); server.event('job_started job_id=3 ')
        second = self.connect(server); b = self.register(second)
        self.submit(server, retries=0); self.assigned(second, 4, b)
        self.submit(server)
        before, _ = inspect(self, server.port, self.path)
        server.stop(crash=True)
        server = self.server(initialize=False, extra=('--heartbeat-timeout-ms', '60000')).ready()
        after, totals = inspect(self, server.port, self.path)
        self.assertEqual((after[1], after[2]), (before[1], before[2]))
        self.assertEqual((after[3]['state'], after[4]['state'], after[5]['state']), (1, 5, 1))
        self.assertEqual((totals['startup_jobs_recovered'], totals['startup_interrupted_jobs']), (5, 2))
        self.assertTrue(all(totals[k] == 0 for k in ('session_jobs_submitted', 'session_jobs_completed',
                                                   'session_jobs_failed', 'session_job_retries', 'workers_retained')))
        restored = [r for r in records(self, server.output, server.process.pid) if r['event'] == 'job_recovered']
        self.assertEqual(len(restored), 5)
        for entry in restored:
            job = after[int(entry['job_id'])]
            self.assertEqual((entry['outcome'], entry['state'], entry['failure']),
                             ('RESTORED', STATES[job['state']], FAILURES[job['failure']]))
            self.assertEqual(entry['durable'], '1')
        self.assertNotIn('job_completed ', server.output)
        self.assertNotIn('job_failed ', server.output)
        replacement = self.connect(server); c = self.register(replacement)
        self.assigned(replacement, 5, c); self.complete(replacement, 5, c)
        self.assigned(replacement, 3, c, attempt=2); self.complete(replacement, 3, c, attempt=2)
        server.event('job_completed job_id=3 ')
        _, totals = inspect(self, server.port, self.path)
        self.assertEqual((totals['jobs_completed_total'], totals['session_jobs_completed']), (3, 2))
        server.stop()

    def test_paths_and_runtime_errors_are_single_line_records(self):
        path = self.directory / 'space "\n\x1b\\.wal'
        server = self.server(path=path).ready()
        with server.connect() as client:
            client.sendall(b'NOPE' + bytes(8)); self.closed(client)
        server.stop()
        ready = event(self, server.output, 'wal_ready')
        self.assertEqual(ready['path'], '"' + escaped(str(path).encode()) + '"')
        self.assertNotIn('\x1b', server.output)
        self.assertEqual(event(self, server.output, 'client_closed', reason='invalid_header')['level'], 'WARN')
        records(self, server.output, server.process.pid)
        missing = self.server(initialize=False)
        missing.wait(1)
        self.assertEqual(event(self, missing.output, 'persistence_failed')['level'], 'ERROR')
        self.assertEqual(event(self, missing.output, 'stopped')['exit_code'], '1')
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            result = subprocess.run([str((test_ping.BIN_DIR / 'faultline-worker').resolve()),
                '--coordinator', f'127.0.0.1:{reservation.getsockname()[1]}'], capture_output=True, text=True, timeout=8)
            self.assertEqual((result.returncode, result.stdout), (1, ''))
            failure = event(self, result.stderr, 'system_error')
            self.assertEqual((failure['level'], failure['operation']), ('ERROR', '"connect"'))
            self.assertGreater(int(failure['errno']), 0)


class ObservabilityExpiry(JobProcessTestCase):
    COORDINATOR_ARGUMENTS = ('--heartbeat-timeout-ms', '500')

    def test_heartbeat_failure_has_matching_logs_and_command_state(self):
        job = self.accepted_id(self.submit_cli('hash', '--args', 'abc', '--max-retries', '1'))
        with self.connect() as worker:
            wid = self.register_worker(worker); self.receive_assignment(worker, wid, job)
            self.wait_for_worker_event('worker_dead', wid)
            latest, totals = inspect(self, self.port, self.wal)
            self.assertEqual((latest[job]['state'], totals['workers_dead'], totals['session_job_retries']), (1, 1, 1))
            timeout = event(self, read_output(self.log), 'heartbeat_timeout', worker_id=wid)
            self.assertEqual(timeout['level'], 'WARN')
            self.assertGreaterEqual(int(timeout['silence_ms']), int(timeout['timeout_ms']))
            loss = event(self, read_output(self.log), 'job_worker_lost', job_id=job)
            self.assertEqual((loss['failure'], loss['outcome'], loss['previous_worker_id']),
                             ('WORKER_LOST', 'REQUEUED', str(wid)))


class ObservabilityExecution(JobProcessTestCase):
    def test_real_execution_failure_and_cancellation_are_inspectable(self):
        with self.worker_process(interval_ms=80) as (worker, output, errors):
            wid = self.wait_for_registration(worker, output, errors)
            done = self.accepted_id(self.submit_cli('sleep', '--args', '0'))
            self.wait_job_event('job_completed', done)
            failed = self.accepted_id(self.submit_cli('fibonacci', '--args', '94'))
            self.wait_job_event('job_failed', failed)
            active = self.accepted_id(self.submit_cli('sleep', '--args', '60000', '--max-retries', '1'))
            self.wait_job_event('job_started', active)
            latest, totals = inspect(self, self.port, self.wal)
            self.assertEqual((latest[done]['state'], latest[failed]['state'], latest[active]['state']), (4, 5, 3))
            self.assertEqual((totals['workers_busy'], totals['jobs_completed_total'], totals['jobs_failed_total']), (1, 1, 1))
            self.stop_worker(worker, errors)
            self.wait_for_worker_event('worker_dead', wid)
            latest, totals = inspect(self, self.port, self.wal)
            self.assertEqual((latest[active]['state'], totals['job_retries_total']), (1, 1))
            sent = event(self, read_output(output), 'job_completed_sent', job_id=done)
            self.assertEqual(sent['acceptance'], 'unconfirmed')
            sent_failure = event(self, read_output(output), 'job_failed_sent', job_id=failed)
            self.assertEqual((sent_failure['acceptance'], sent_failure['task_status']), ('unconfirmed', 'invalid_arguments'))
            event(self, read_output(output), 'job_started_sent', job_id=active, acceptance='unconfirmed')
            event(self, read_output(output), 'task_thread_started', job_id=active)
            event(self, read_output(output), 'task_joined', job_id=active, terminal_report_sent=0)
            records(self, read_output(output), worker.pid)
            self.assertEqual(read_output(errors), '')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=Path('build/debug'))
    parser.add_argument('--port', type=int, default=None)
    args, remaining = parser.parse_known_args()
    test_ping.BIN_DIR = persistence.BIN_DIR = args.bin_dir
    test_ping.TEST_PORT = args.port
    unittest.main(argv=[__file__, *remaining], verbosity=2)
