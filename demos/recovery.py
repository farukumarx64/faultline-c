#!/usr/bin/env python3
"""Show one acknowledged job surviving SIGKILL on an already-connected worker.

The real CLI output is recorded as an asciicast v2 terminal replay. Runtime
ownership, deadlines, diagnostics, and teardown reuse the existing chaos harness.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/chaos'))
from run_batch import BatchRun, RunFailure, pairs, require, table  # noqa: E402
from run_chaos import ChaosRun, log_record  # noqa: E402

STATUS_FIELDS = ['job_id', 'state', 'worker_id', 'attempt', 'retries', 'failure', 'result_bytes']
CLEAR = '\x1b[2J\x1b[H'


class Recording:
    """Record actual displayed output; every screen is also readable as plain text."""

    def __init__(self, directory):
        self.started = time.monotonic()
        self.cast = (directory / 'recovery.cast').open('x', encoding='utf-8')
        self.transcript = (directory / 'transcript.log').open('x', encoding='utf-8')
        self.cast.write(json.dumps(dict(version=2, width=112, height=30,
            timestamp=int(time.time()), title='Faultline: one job survives a worker crash',
            env={'TERM': 'xterm-256color', 'SHELL': '/bin/sh'})) + '\n')
        self.cast.flush()

    def show(self, title, body):
        screen = 'FAULTLINE / RECOVERY DEMO\n' + title + '\n' + '-' * 76 + '\n\n' + body.rstrip() + '\n'
        require(len(screen.splitlines()) <= 30 and all(len(line) <= 112 for line in screen.splitlines()),
                'demo screen exceeds the recorded terminal dimensions')
        elapsed = round(time.monotonic() - self.started, 6)
        self.cast.write(json.dumps([elapsed, 'o', CLEAR + screen.replace('\n', '\r\n')]) + '\n')
        self.cast.flush()
        self.transcript.write(f'\n--- {elapsed:.3f}s ---\n' + screen)
        self.transcript.flush()
        print((CLEAR if sys.stdout.isatty() else '\n') + screen, end='', flush=True)

    def close(self):
        # Preserve the final real reading pause in an ordinary asciicast player.
        try:
            self.cast.write(json.dumps([round(time.monotonic() - self.started, 6), 'o', '']) + '\n')
        finally:
            self.cast.close()
            self.transcript.close()


class RecoveryDemo(ChaosRun):
    mode = 'recovery_demo'

    def __init__(self, args, directory, recording):
        super().__init__(args, directory)
        self.recording = recording
        self.transitions = []
        self.target = None
        self.survivor_id = None
        self.final_output = None

    def manifest(self):
        return {**BatchRun.manifest(self), 'demo_contract': 'faultline-recovery-demo-v1',
                'demo_source_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'scenario': 'SIGKILL; two existing workers; one job; no replacements',
                'recording': 'recovery.cast', 'sleep_ms': self.args.sleep_ms}

    def hold(self):
        until = time.monotonic() + self.args.hold_ms / 1000
        while time.monotonic() < until:
            self.pause(self.work_deadline, min(.05, until - time.monotonic()))

    def display_command(self, command, *args):
        binary = self.binaries['faultline']
        try:
            binary = './' + str(binary.relative_to(ROOT))
        except ValueError:
            binary = str(binary)
        return '$ ' + shlex.join([binary, command, *args]) + ' \\\n    --coordinator ' + self.endpoint

    def start_pool(self):
        super().start_pool()
        output = self.cli('workers')
        self.recording.show('1 / TWO WORKERS READY', self.display_command('workers') + '\n' + output + '\n' +
            '\n'.join(f'# worker_id={wid} belongs to demo-owned PID {self.pool[slot].process.pid}'
                      for slot, wid in self.workers.items()) +
            '\n# Both workers are connected before submission. No replacement is started.')
        self.hold()

    def runtime_line(self, child, line, path):
        super().runtime_line(child, line, path)
        if child is self.coordinator:
            _, _, event, fields = log_record(line)
            if event.startswith('job_'):
                self.transitions.append(dict(event=event, **fields))

    def wait_status(self, attempt, state):
        deadline = min(self.work_deadline, time.monotonic() + 10)
        job_id = self.ledger[0]['job_id']
        while True:
            output = self.cli('status', str(job_id), deadline=deadline)
            fields = STATUS_FIELDS + (['result'] if '\nresult=' in output else [])
            status = pairs(output, fields)
            require(status['job_id'] == str(job_id) and status['state'] != 'FAILED', 'job identity lost or failed')
            if status['attempt'] == str(attempt) and status['state'] == state:
                return output, status
            require(status['state'] != 'DONE', 'job finished before the intended demo action')
            self.pause(deadline, .05)

    def after_admission(self):
        job_id = self.ledger[0]['job_id']
        output, status = self.wait_status(1, 'RUNNING')
        wid = int(status['worker_id'])
        require(status['retries'] == '0/1' and wid in self.workers.values(), 'invalid first attempt')
        slot, = [slot for slot, worker in self.workers.items() if worker == wid]
        survivor_slot, = [key for key in self.workers if key != slot]
        self.survivor_id = self.workers[survivor_slot]
        self.target = dict(job_id=job_id, worker_id=wid, pid=self.pool[slot].process.pid,
                           survivor_id=self.survivor_id, survivor_pid=self.pool[survivor_slot].process.pid)
        submit_args = ['sleep', '--args', str(self.args.sleep_ms), '--max-retries', '1']
        # The acknowledgment is read from the actual submission process's stdout.
        acknowledgment = next(c for c in self.children if c.role == 'cli-submit').stdout.read_text()
        self.recording.show('2 / ONE ACKNOWLEDGED JOB IS RUNNING',
            self.display_command('submit', *submit_args) + '\n' + acknowledgment + '\n' +
            self.display_command('status', str(job_id)) + '\n' + output +
            '\n# Remember this job ID. Its retry allowance is exactly one.')
        self.hold()
        # Refresh ownership immediately before sending the signal; never kill an
        # idle worker just because it owned the job in an earlier snapshot.
        _, current = self.wait_status(1, 'RUNNING')
        require(current == status, 'first attempt changed before SIGKILL')
        self.check()
        child = self.pool[slot]
        action = dict(candidate=0, slot=slot, generation=0, worker_id=wid, pid=child.process.pid,
                      fd=self.worker_fds[wid], observed=dict(job_id=job_id, attempt=1, state='RUNNING'),
                      loss=None, death=None, closed=None, replacement=None)
        require(child.process.poll() is None, 'selected worker exited before SIGKILL')
        child.process.kill()
        child.intentional_crash = True
        child.signals.append(signal.SIGKILL)
        action['signal_elapsed_ms'] = self.elapsed()
        self.actions.append(action)
        self.crashes[wid] = action
        self.event('fault_signal', worker_id=wid, pid=child.process.pid, signal=signal.SIGKILL, job_id=job_id)
        self.recording.show('3 / HARD CRASH THE BUSY WORKER',
            f'$ kill -KILL {child.process.pid}\n\n' +
            f'# Sent SIGKILL to worker_id={wid}, the owner of job_id={job_id}, attempt=1.\n' +
            f'# Worker {self.survivor_id} was already connected and remains alive.\n' +
            '# Waiting for the coordinator to reassign the same job...')
        limit = min(self.work_deadline, time.monotonic() + 5)
        while True:
            self.check(limit)
            if (child.process.returncode == -signal.SIGKILL and child.group_retired and
                    action['loss'] is not None and action['death'] is not None and action['closed'] is not None):
                break
            self.pause(limit, .02)
        self.account_transport_warnings(action)
        self.save('fault-actions.json', self.actions)
        self.hold()
        output, retried = self.wait_status(2, 'RUNNING')
        require(retried['worker_id'] == str(self.survivor_id) and retried['retries'] == '1/1',
                'retry did not run on the already-connected survivor')
        self.recording.show('4 / SAME JOB, ANOTHER WORKER',
            self.display_command('status', str(job_id)) + '\n' + output + '\n' +
            f'# job_id={job_id} is unchanged. Worker {wid} -> worker {self.survivor_id}.\n' +
            '# attempt=2; retries=1/1. The sleep task starts again from the beginning.\n' +
            '# Waiting for its real completion; no time is fast-forwarded.')

    def verify_workers(self, rows, idle=False):
        if not self.actions:
            return super().verify_workers(rows, idle)
        require(set(rows) == self.all_worker_ids, 'worker ID set changed during the demo')
        crashed = self.target['worker_id']
        for wid, row in rows.items():
            if wid == crashed:
                require(row['LIVENESS'] == 'DEAD' and row['JOB_ID'] == 'none' and row['ATTEMPT'] == '0',
                        'crashed worker still owns a lease')
            else:
                require(row['LIVENESS'] == 'ALIVE' and int(row['HEARTBEAT_AGE_MS']) < 6000,
                        'surviving worker is not alive')
                if row['JOB_ID'] == 'none':
                    require(row['ATTEMPT'] == '0', 'idle survivor still has an attempt')
                else:
                    require(not idle and row['JOB_ID'] == str(self.target['job_id']) and row['ATTEMPT'] == '2',
                            'survivor has an unexpected lease')

    def expected_stats(self):
        return {**super().expected_stats(), 'workers_alive': 1, 'workers_idle': 1, 'workers_dead': 1}

    def verify_job_history(self):
        history = super().verify_job_history()
        target = self.target
        expected = [
            ('job_submitted', 'QUEUED', '0', '0', '0'),
            ('job_assigned', 'ASSIGNED', str(target['worker_id']), '1', '0'),
            ('job_started', 'RUNNING', str(target['worker_id']), '1', '0'),
            ('job_worker_lost', 'QUEUED', '0', '1', '1'),
            ('job_assigned', 'ASSIGNED', str(self.survivor_id), '2', '1'),
            ('job_started', 'RUNNING', str(self.survivor_id), '2', '1'),
            ('job_completed', 'DONE', str(self.survivor_id), '2', '1')]
        actual = [tuple(e[key] for key in ('event', 'state', 'worker_id', 'attempt', 'retry_count'))
                  for e in self.transitions]
        require(actual == expected, 'missing, duplicate, or invalid durable recovery transitions')
        require(all(e['job_id'] == str(target['job_id']) and e['durable'] == '1' for e in self.transitions),
                'transition identity/durability mismatch')
        sequences = [int(e['wal_sequence']) for e in self.transitions]
        require(all(a < b for a, b in zip(sequences, sequences[1:])), 'WAL sequence did not advance')
        require(self.transitions[-1]['result'] == self.expected_result, 'wrong logged result')
        self.save('recovery-history.json', self.transitions)
        return history

    def verify_coverage(self):
        super().verify_coverage()
        require(len(self.actions) == 1 and len(self.all_worker_ids) == 2, 'not exactly one crash and two workers')
        survivor, = [c for c in self.pool.values() if c.process.pid == self.target['survivor_pid']]
        require(survivor.process.poll() is None and len(self.pool) == 2, 'survivor was replaced or exited')
        row = self.terminal[self.target['job_id']]
        require((row['STATE'], row['WORKER_ID'], row['ATTEMPT'], row['RETRIES']) ==
                ('DONE', str(self.survivor_id), '2', '1/1'), 'recovery did not finish correctly')

    def drain_and_verify(self):
        super().drain_and_verify()
        self.final_output = self.cli('status', str(self.target['job_id']))
        require(pairs(self.final_output, STATUS_FIELDS + ['result']) ==
                self.snapshots['statuses'][self.target['job_id']], 'final displayed status changed')

    def summary_details(self):
        return dict(demo_contract='faultline-recovery-demo-v1',
                    target=self.target, transitions=self.transitions, fault_actions=self.actions,
                    recording='recovery.cast', recording_time_compressed=False)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir', type=Path, default=ROOT / 'build/debug')
    parser.add_argument('--output-dir', type=Path, help='fresh evidence directory; existing paths are rejected')
    parser.add_argument('--sleep-ms', type=int, default=6000, help='job duration, default 6000')
    parser.add_argument('--hold-ms', type=int, default=2500, help='reading pauses, default 2500; zero for checks')
    parser.add_argument('--deadline-ms', type=int, default=60000, help='total process deadline, includes 10 s cleanup')
    args = parser.parse_args(argv)
    if not 0 <= args.hold_ms <= 5000 or not args.hold_ms + 1000 <= args.sleep_ms <= 30000:
        parser.error('hold must be 0..5000 ms; sleep must be hold+1000..30000 ms')
    if not 10001 <= args.deadline_ms <= 120000:
        parser.error('deadline must be 10001..120000 ms, including a 10000 ms cleanup reserve')
    if os.name != 'posix' or not hasattr(os, 'killpg'):
        parser.error('POSIX process groups are required (macOS or Linux)')
    args.bin_dir = args.bin_dir.resolve()
    for name in ('faultline', 'faultline-worker', 'faultline-coordinator'):
        path = args.bin_dir / name
        if not path.is_file() or not os.access(path, os.X_OK):
            parser.error(f'build the missing executable first: {path}')
    try:
        if args.output_dir is None:
            parent = ROOT / 'build/demos'
            parent.mkdir(parents=True, exist_ok=True)
            args.output_dir = Path(tempfile.mkdtemp(prefix='recovery-', dir=parent))
        else:
            args.output_dir = args.output_dir.absolute()
            args.output_dir.mkdir(parents=True, exist_ok=False)
    except OSError as error:
        parser.error(f'cannot create fresh evidence directory: {error}')
    args.workers, args.jobs, args.max_retries = 2, 1, 1
    return args


def main():
    args = parse_args()
    recording = Recording(args.output_dir)
    try:
        demo = RecoveryDemo(args, args.output_dir, recording)
        code = demo.run()
        if code == 0:
            recording.show('5 / VERIFIED: THE SAME JOB COMPLETED',
                demo.display_command('status', str(demo.target['job_id'])) + '\n' + demo.final_output + '\n' +
                '# PASS: one submission, two attempts, one retry, one accepted result.\n' +
                f'# Worker {demo.target["worker_id"]} was killed; existing worker {demo.survivor_id} completed the job.\n' +
                '# Cleanup passed: all owned processes reaped; no process groups left.')
            time.sleep(args.hold_ms / 1000)
        else:
            recording.show('DEMO FAILED', '# Verification failed. No successful recovery is claimed.\n'
                           '# See summary.json and the retained logs for the failure and cleanup outcome.')
        return code
    except (OSError, RunFailure) as error:
        print(f'demo error: {error}; evidence: {args.output_dir}', file=sys.stderr)
        return 1
    finally:
        recording.close()


if __name__ == '__main__':
    sys.exit(main())
