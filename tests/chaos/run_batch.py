#!/usr/bin/env python3
"""Run the faultline-chaos-v1 baseline against real coordinator/worker/CLI binaries.

Standard library only. No worker faults or automatic submission retries. Artifacts
survive the run; process ownership ends only after bounded teardown and reaping.
"""

import argparse
from dataclasses import dataclass, field
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time


ROOT = Path(__file__).resolve().parents[2]
CONTRACT = 'faultline-chaos-v1'
CLEANUP_SECONDS = 10
POLL_SECONDS = .2
JOB_COLUMNS = 'JOB_ID TASK STATE WORKER_ID ATTEMPT RETRIES FAILURE RESULT_BYTES'.split()
WORKER_COLUMNS = 'WORKER_ID LIVENESS HEARTBEAT_AGE_MS JOB_ID ATTEMPT'.split()
STATS_FIELDS = '''jobs_submitted_total jobs_queued jobs_assigned jobs_running
jobs_completed_total jobs_failed_total job_attempts_total job_retries_total
completed_latency_avg_ms workers_retained workers_alive workers_expired workers_dead
workers_busy workers_idle session_uptime_ms session_jobs_submitted
session_jobs_completed session_jobs_failed session_job_retries startup_jobs_recovered
startup_interrupted_jobs startup_duration_ms heartbeat_timeout_ms
session_completed_per_second'''.split()
SANITIZER = re.compile(r'AddressSanitizer|LeakSanitizer|UndefinedBehaviorSanitizer|runtime error:')


class RunFailure(Exception):
    """A failed experiment, including uncertain admission and malformed output."""


def require(condition, reason):
    if not condition:
        raise RunFailure(reason)


def unsigned(value, name, maximum=(1 << 64) - 1):
    require(re.fullmatch(r'0|[1-9][0-9]*', value) is not None, f'invalid {name}: {value!r}')
    number = int(value)
    require(number <= maximum, f'{name} out of range')
    return number


def pairs(output, fields):
    lines = output.splitlines()
    require(all('=' in line for line in lines), 'malformed key/value output')
    items = [line.split('=', 1) for line in lines]
    require([item[0] for item in items] == fields, 'missing, duplicate, or unexpected output fields')
    return dict(items)


def table(output, kind):
    lines = output.splitlines()
    header = r'jobs=([0-9]+)' if kind == 'jobs' else r'workers=([0-9]+) heartbeat_timeout_ms=6000'
    match = re.fullmatch(header, lines[0]) if lines else None
    require(match is not None, f'malformed {kind} count/header')
    count = unsigned(match[1], f'{kind} count')
    columns = JOB_COLUMNS if kind == 'jobs' else WORKER_COLUMNS
    empty = 'No retained jobs.' if kind == 'jobs' else 'No retained worker registrations.'
    if count == 0:
        require(lines[1:] == [empty], f'malformed empty {kind} listing')
        return {}
    require(len(lines) == count + 2 and lines[1].split() == columns, f'malformed {kind} table')
    rows = {}
    for line in lines[2:]:
        values = line.split()
        require(len(values) == len(columns), f'malformed {kind} row')
        row = dict(zip(columns, values))
        identity = unsigned(values[0], columns[0])
        require(identity != 0 and identity not in rows, f'zero or duplicate {kind} ID')
        rows[identity] = row
    return rows


@dataclass
class Child:
    process: subprocess.Popen
    role: str
    stdout: Path
    stderr: Path
    argv: list
    started_ms: int
    exit_ms: object = None
    group_retired: bool = False
    shutdown_requested: bool = False
    intentional_crash: bool = False
    signals: list = field(default_factory=list)
    offsets: dict = field(default_factory=dict)
    pending: dict = field(default_factory=dict)


class BatchRun:
    mode = 'baseline'
    task_name = 'sleep'

    def __init__(self, args, directory):
        self.args = args
        self.directory = directory
        self.children = []
        self.workers = {}
        self.pool = {}
        self.all_worker_ids = set()
        self.coordinator = None
        self.ledger = []
        self.observations = {}
        self.terminal = {}
        self.snapshots = {}
        self.drain_eligible = set()
        self.accounting = None
        self.started = None
        self.work_deadline = None
        self.run_deadline = None
        self.stop_signal = None
        self.cleaning = False
        self.cleanup_result = None
        self.cleanup_errors = []
        self.events = (directory / 'events.jsonl').open('x', encoding='utf-8')
        self.binaries = {name: args.bin_dir / name for name in
                         ('faultline', 'faultline-worker', 'faultline-coordinator')}

    def elapsed(self):
        return round((time.monotonic() - self.started) * 1000) if self.started is not None else 0

    def save(self, name, value):
        path = self.directory / name
        temporary = self.directory / (name + '.tmp')
        temporary.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
        temporary.replace(path)

    def event(self, event, **fields):
        self.events.write(json.dumps({**fields, 'elapsed_ms': self.elapsed(), 'event': event}) + '\n')
        self.events.flush()

    def cleanup_event(self, event, **fields):
        # Failure to write evidence must not prevent the next child being stopped.
        try:
            self.event(event, **fields)
        except OSError as error:
            self.cleanup_errors.append(f'event log: {error}')

    def on_signal(self, signum, _frame):
        # Do not raise inside Popen: install the handle in our ledger first.
        if self.stop_signal is None:
            self.stop_signal = signum

    def check(self, deadline=None):
        require(self.stop_signal is None, f'interrupted by signal {self.stop_signal}')
        limit = self.work_deadline if deadline is None else min(deadline, self.work_deadline)
        require(time.monotonic() < limit, 'work or operation deadline exceeded')
        for child in self.children:
            if child.role == 'coordinator' or child.role.startswith('worker-'):
                self.poll(child)
                require(child.process.returncode is None or
                        (child.intentional_crash and child.process.returncode == -signal.SIGKILL),
                        f'unexpected {child.role} exit: {child.process.returncode}')
                self.inspect_runtime(child)

    def pause(self, deadline, seconds=POLL_SECONDS):
        self.check(deadline)
        time.sleep(min(seconds, max(0, min(deadline, self.work_deadline) - time.monotonic())))

    def inspect_runtime(self, child):
        for path in (child.stdout, child.stderr):
            with path.open('rb') as stream:
                stream.seek(child.offsets.get(path, 0))
                data = child.pending.get(path, b'') + stream.read()
                child.offsets[path] = stream.tell()
            complete, _, child.pending[path] = data.rpartition(b'\n')
            text = complete.decode('utf-8', errors='replace')
            require(not SANITIZER.search(text), f'sanitizer diagnostic in {path.name}')
            for line in text.splitlines():
                self.runtime_line(child, line, path)

    def runtime_line(self, child, line, path):
        require(not re.search(r'\[(WARN|ERROR)\]', line), f'unexpected runtime diagnostic in {path.name}')

    def poll(self, child):
        code = child.process.poll()  # waitpid(WNOHANG) reaps a finished direct child.
        if code is not None and child.exit_ms is None:
            child.exit_ms = self.elapsed()
            if (self.cleaning and not child.shutdown_requested and not child.intentional_crash and
                    (child.role == 'coordinator' or child.role.startswith('worker-'))):
                self.cleanup_errors.append(f'unexpected {child.role} exit before shutdown request: {code}')
            self.cleanup_event('exit_reaped', role=child.role, pid=child.process.pid, returncode=code)
        self.group_exists(child)
        return code

    def group_exists(self, child):
        if child.group_retired:
            return False
        try:
            os.killpg(child.process.pid, 0)
            return True
        except ProcessLookupError:
            child.group_retired = True
            return False
        except PermissionError:
            # A probe can be inconclusive while a group is exiting. Keep ownership
            # and wait for ESRCH; never mistake denied access for an empty group.
            return True

    def spawn(self, role, argv):
        require(not self.cleaning, 'spawn attempted during cleanup')
        if self.started is None:
            self.started = time.monotonic()
            self.run_deadline = self.started + self.args.deadline_ms / 1000
            self.work_deadline = self.run_deadline - CLEANUP_SECONDS
        self.check()
        prefix = f'{len(self.children):04d}-{role}'
        stdout, stderr = (self.directory / (prefix + suffix) for suffix in ('.stdout.log', '.stderr.log'))
        with stdout.open('xb') as out, stderr.open('xb') as err:
            process = subprocess.Popen([str(arg) for arg in argv], stdin=subprocess.DEVNULL,
                                       stdout=out, stderr=err, start_new_session=True)
            child = Child(process, role, stdout, stderr, [str(arg) for arg in argv], self.elapsed())
            self.children.append(child)
        self.event('spawn', role=role, pid=process.pid, argv=child.argv,
                   stdout=stdout.name, stderr=stderr.name)
        return child

    def command(self, role, argv, deadline, allow_failure=False):
        self.check(deadline)
        child = self.spawn(role, argv)
        limit = min(deadline, self.work_deadline, time.monotonic() + 8)
        while self.poll(child) is None:
            self.pause(limit, .02)
        self.check(limit)
        require(child.group_retired, f'{role} left descendants behind')
        output = child.stdout.read_text(encoding='utf-8')
        errors = child.stderr.read_text(encoding='utf-8')
        require(not SANITIZER.search(output + errors), f'sanitizer diagnostic in {role}')
        if not allow_failure:
            require(child.process.returncode == 0 and not errors,
                    f'{role} failed (exit {child.process.returncode}); see {child.stderr.name}')
        return output, child.process.returncode

    def cli(self, command, *arguments, deadline=None):
        output, _ = self.command('cli-' + command,
                                 [self.binaries['faultline'], command, *arguments,
                                  '--coordinator', self.endpoint],
                                 self.work_deadline if deadline is None else deadline)
        return output

    def manifest(self):
        # platform.platform() may launch uname/file helpers internally. Use the
        # syscall directly so preflight never creates an untracked subprocess.
        system = os.uname()
        identities = {}
        for name, path in self.binaries.items():
            digest = hashlib.sha256()
            with path.open('rb') as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(chunk)
            identities[name] = dict(path=str(path), sha256=digest.hexdigest(), size=path.stat().st_size)
        return dict(contract=CONTRACT, mode=self.mode, created_utc=datetime.now(timezone.utc).isoformat(),
                    configuration={key: str(value) if isinstance(value, Path) else value
                                   for key, value in vars(self.args).items()},
                    cleanup_reserve_ms=10000, polling_interval_ms=200,
                    heartbeat_interval_ms=2000, heartbeat_timeout_ms=6000,
                    candidate_plan=[], seed_used=False, python=sys.version,
                    platform=dict(system=system.sysname, release=system.release,
                                  version=system.version, machine=system.machine),
                    binaries=identities, build_environment={key: os.environ.get(key) for key in
                    ('CC', 'CFLAGS', 'SANITIZE', 'ASAN_OPTIONS', 'UBSAN_OPTIONS', 'ASAN_SYMBOLIZER_PATH')})

    def start_pool(self):
        # Close the reservation immediately before spawning. We only run CLI clients
        # after this specific child's log proves it won the bind race and is listening.
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        self.endpoint = f'127.0.0.1:{port}'
        self.event('endpoint_selected', endpoint=self.endpoint)
        self.coordinator = self.spawn('coordinator', [self.binaries['faultline-coordinator'],
            '--port', str(port), '--wal', self.directory / 'coordinator.wal', '--init-wal',
            '--heartbeat-timeout-ms', '6000'])
        ready = time.monotonic() + 5
        expected = f'coordinator listening address=127.0.0.1 port={port} heartbeat_timeout_ms=6000 pid={self.coordinator.process.pid} '
        while expected not in self.coordinator.stdout.read_text(encoding='utf-8'):
            self.pause(ready)
        require(self.cli('ping', deadline=ready) == 'PONG\n', 'readiness PING did not return PONG')
        ready = time.monotonic() + 10
        for slot in range(self.args.workers):
            self.spawn_worker(slot, 0)
        while len(self.workers) != len(self.pool):
            self.check(ready)
            for slot, child in self.pool.items():
                if slot in self.workers:
                    continue
                self.read_registration(slot, child, 0)
            if len(self.workers) != len(self.pool):
                self.pause(ready)
        self.verify_workers(table(self.cli('workers', deadline=ready), 'workers'), idle=True)

    def spawn_worker(self, slot, generation):
        child = self.spawn(f'worker-{slot}-g{generation}', [self.binaries['faultline-worker'],
                           '--coordinator', self.endpoint, '--heartbeat-interval-ms', '2000'])
        self.pool[slot] = child
        return child

    def read_registration(self, slot, child, generation):
        registrations = re.findall(r'worker registered worker_id=([0-9]+) coordinator=' +
            re.escape(self.endpoint) + r' heartbeat_interval_ms=2000 pid=' +
            str(child.process.pid) + r' ', child.stdout.read_text(encoding='utf-8'))
        require(len(registrations) <= 1, f'duplicate registration for {child.role}')
        if not registrations:
            return False
        wid = unsigned(registrations[0], 'worker ID', (1 << 32) - 1)
        require(wid and wid not in self.all_worker_ids, 'zero or duplicate worker ID')
        self.workers[slot] = wid
        self.all_worker_ids.add(wid)
        self.event('registered', slot=slot, generation=generation, pid=child.process.pid, worker_id=wid)
        return True

    def verify_workers(self, rows, idle=False):
        require(set(rows) == set(self.workers.values()), 'worker listing does not match the owned pool')
        for row in rows.values():
            require(row['LIVENESS'] == 'ALIVE', 'worker is dead or expired')
            require(unsigned(row['HEARTBEAT_AGE_MS'], 'heartbeat age') < 6000, 'worker heartbeat expired')
            attempt = unsigned(row['ATTEMPT'], 'worker attempt')
            if row['JOB_ID'] == 'none':
                require(attempt == 0, 'idle worker still has an attempt')
            else:
                require(not idle and unsigned(row['JOB_ID'], 'worker job ID') > 0 and attempt == 1,
                        'unexpected worker lease')

    def submit(self):
        deadline = min(self.work_deadline, time.monotonic() + 30)
        for index in range(self.args.jobs):
            entry = dict(index=index, task='sleep', arguments=str(self.args.sleep_ms),
                         max_retries=self.args.max_retries, job_id=None)
            self.ledger.append(entry)
            self.save('submissions.json', self.ledger)
            self.event('submission_attempt', **entry)
            try:
                output = self.cli('submit', 'sleep', '--args', entry['arguments'], '--max-retries',
                                  str(self.args.max_retries), deadline=deadline)
                job_id = unsigned(pairs(output, ['job_id'])['job_id'], 'acknowledged job ID')
                require(job_id and all(item['job_id'] != job_id for item in self.ledger),
                        'zero or duplicate acknowledgment ID')
            except (RunFailure, OSError, UnicodeError) as error:
                raise RunFailure(f'admission uncertain at input {index}; not resubmitted: {error}') from error
            entry['job_id'] = job_id
            self.event('acknowledged', **entry)
            self.save('submissions.json', self.ledger)

    def require_job_ids(self, identities, source):
        expected = {entry['job_id'] for entry in self.ledger}
        require(len(self.ledger) == len(expected) == self.args.jobs and
                all(isinstance(job_id, int) and job_id > 0 for job_id in expected),
                'submission ledger is incomplete or contains duplicate IDs')
        actual = set(identities)
        require(actual == expected, f'{source} differs from acknowledged ID set: '
                f'missing={sorted(expected - actual)}, unexpected={sorted(actual - expected)}')
        return expected

    def observe_jobs(self, rows):
        self.require_job_ids(rows, 'job listing')
        for job_id, row in rows.items():
            require(row['TASK'] == self.task_name and row['FAILURE'] == 'NONE', f'unexpected task/failure for job {job_id}')
            require(row['RETRIES'] == f'0/{self.args.max_retries}', f'job {job_id} consumed a retry in baseline')
            require(row['STATE'] in ('QUEUED', 'ASSIGNED', 'RUNNING', 'DONE'), f'job {job_id} did not succeed')
            if row['STATE'] == 'QUEUED':
                require(row['ATTEMPT'] == '0' and row['WORKER_ID'] == 'none', f'invalid queued job {job_id}')
            else:
                require(row['ATTEMPT'] == '1' and unsigned(row['WORKER_ID'], 'job worker ID') in self.workers.values(),
                        f'unexpected attempt/owner for job {job_id}')
            require(row['RESULT_BYTES'] == str(len(self.expected_result) if row['STATE'] == 'DONE' else 0),
                    f'unexpected result length for job {job_id}')
            if job_id in self.terminal:
                require(row == self.terminal[job_id], f'terminal job {job_id} changed')
            if row['STATE'] == 'DONE':
                self.terminal[job_id] = row
            if row != self.observations.get(job_id):
                self.event('job_observed', job_id=job_id, snapshot=row)
                self.observations[job_id] = row

    @property
    def expected_result(self):
        return f'slept_ms={self.args.sleep_ms}'

    def status(self, job_id, row):
        result = pairs(self.cli('status', str(job_id)),
                       ['job_id', 'state', 'worker_id', 'attempt', 'retries', 'failure', 'result_bytes', 'result'])
        for name in ('job_id', 'state', 'worker_id', 'attempt', 'retries', 'failure', 'result_bytes'):
            require(result[name] == row[name.upper()], f'status/listing disagreement for job {job_id}: {name}')
        require(result['result'] == '"' + self.expected_result + '"', f'wrong exact result for job {job_id}')
        return result

    def verify_stats(self, output):
        result = pairs(output, STATS_FIELDS)
        stats = {key: unsigned(value, key) for key, value in result.items() if key != 'session_completed_per_second'}
        expected = dict.fromkeys(STATS_FIELDS[:-1], 0)
        expected.update(self.expected_stats())
        for key, value in expected.items():
            if key not in ('session_uptime_ms', 'startup_duration_ms', 'completed_latency_avg_ms'):
                require(stats[key] == value, f'stats disagreement: {key}={stats[key]}, expected {value}')
        require(re.fullmatch(r'[0-9]+\.[0-9]{3}', result['session_completed_per_second']) is not None,
                'malformed throughput')
        rate = float(result['session_completed_per_second'])
        expected_rate = stats['session_jobs_completed'] * 1000 / stats['session_uptime_ms'] if stats['session_uptime_ms'] else 0
        require(math.isfinite(rate) and abs(rate - expected_rate) <= .00051, 'inconsistent throughput')
        return {**stats, 'session_completed_per_second': rate}

    def expected_stats(self):
        expected = {key: self.args.jobs for key in ('jobs_submitted_total', 'jobs_completed_total',
                    'job_attempts_total', 'session_jobs_submitted', 'session_jobs_completed')}
        expected.update({key: self.args.workers for key in ('workers_retained', 'workers_alive', 'workers_idle')})
        return {**expected, 'heartbeat_timeout_ms': 6000}

    def after_admission(self):
        """Baseline proceeds directly to drain; chaos overrides this phase."""

    def verify_coverage(self):
        pass

    def verify_job_history(self):
        # Baseline observations already require attempt 1 and zero retries.
        return {job_id: [] for job_id in self.terminal}

    def verify_accounting(self, rows, statuses):
        """Audit identity/history after repeated status, result, and stats checks."""
        submitted = self.require_job_ids(rows, 'final listing')
        self.require_job_ids(statuses, 'final statuses')
        self.require_job_ids(self.terminal, 'terminal observations')
        completed = {job_id for job_id, row in rows.items() if row['STATE'] == 'DONE'}
        failed = {job_id for job_id, row in rows.items() if row['STATE'] == 'FAILED'}
        require(completed.isdisjoint(failed) and (completed | failed) == submitted,
                'terminal ID sets do not partition the submissions')
        require(self.drain_eligible <= completed, 'eligible drain jobs did not all complete')
        history = self.verify_job_history()
        counts = dict(submitted=len(submitted), completed=len(completed), terminally_failed=len(failed))
        require(counts['submitted'] == counts['completed'] + counts['terminally_failed'],
                'submitted != completed + terminally_failed')
        report = dict(verified=True, counts=counts, submitted_ids=sorted(submitted),
                      completed_ids=sorted(completed), terminally_failed_ids=sorted(failed),
                      drain_eligible_ids=sorted(self.drain_eligible),
                      jobs=[dict(submission=entry, terminal_status=statuses[entry['job_id']],
                                 lost_attempts=history[entry['job_id']]) for entry in self.ledger])
        self.save('accounting.json', report)
        self.accounting = report
        self.event('accounting_verified', **counts)

    def coverage(self):
        return dict(injected_faults=0, recovery_demonstrated=False)

    def summary_details(self):
        return {}

    def drain_and_verify(self):
        starting_drain = True
        while True:
            rows = table(self.cli('jobs'), 'jobs')
            self.observe_jobs(rows)
            self.verify_workers(table(self.cli('workers'), 'workers'))
            if starting_drain:
                self.drain_eligible = {job_id for job_id, row in rows.items()
                                       if row['STATE'] not in ('DONE', 'FAILED')}
                self.save('drain-start.json', dict(elapsed_ms=self.elapsed(), jobs=rows,
                                                 eligible_job_ids=sorted(self.drain_eligible)))
                self.event('drain_started', eligible_job_ids=sorted(self.drain_eligible))
                starting_drain = False
            if len(self.terminal) == self.args.jobs:
                break
            self.pause(self.work_deadline)
        statuses = {job_id: self.status(job_id, row) for job_id, row in rows.items()}
        # Re-observe complete terminal records, including results, not only totals.
        final_rows = table(self.cli('jobs'), 'jobs')
        self.observe_jobs(final_rows)
        for job_id, row in final_rows.items():
            require(self.status(job_id, row) == statuses[job_id], f'terminal status changed for job {job_id}')
        workers = table(self.cli('workers'), 'workers')
        self.verify_workers(workers, idle=True)
        stats = self.verify_stats(self.cli('stats'))
        self.check()
        self.snapshots = dict(jobs=final_rows, statuses=statuses, workers=workers, stats=stats)
        self.save('final-snapshots.json', self.snapshots)
        self.verify_accounting(final_rows, statuses)
        self.verify_coverage()
        self.event('verified', submitted=self.args.jobs, completed=stats['jobs_completed_total'],
                   failed=stats['jobs_failed_total'], attempts=stats['job_attempts_total'], retries=stats['job_retries_total'])

    def signal_child(self, child, signum):
        try:
            self.poll(child)
            if not self.group_exists(child):
                return
            if signum == signal.SIGTERM:
                child.shutdown_requested = True
            os.killpg(child.process.pid, signum)
            child.signals.append(signum)
            self.cleanup_event('signal', role=child.role, pid=child.process.pid, signal=signum, phase='cleanup')
            if signum == signal.SIGKILL:
                self.cleanup_errors.append(f'forced kill during cleanup: {child.role} pid={child.process.pid}')
        except ProcessLookupError:
            child.group_retired = True
        except PermissionError as error:
            # macOS can deny a signal while a group is already exiting. Record
            # the denial and retain ownership. Only confirmed disappearance can
            # satisfy cleanup; a persistent denial ends as a remaining-group error.
            self.cleanup_event('signal_denied', role=child.role, pid=child.process.pid,
                               signal=signum, error=str(error))
        except OSError as error:
            self.cleanup_errors.append(f'signal {child.role}: {error}')

    def wait_groups(self, children, deadline):
        while True:
            pending = False
            for child in children:
                try:
                    self.poll(child)
                    pending |= child.process.returncode is None or self.group_exists(child)
                except OSError as error:
                    self.cleanup_errors.append(f'reap {child.role}: {error}')
                    pending = True
            if not pending or time.monotonic() >= deadline:
                return
            time.sleep(min(.02, max(0, deadline - time.monotonic())))

    def cleanup(self):
        if self.cleanup_result is not None:
            return self.cleanup_result
        self.cleaning = True
        start = time.monotonic()
        deadline = min(start + CLEANUP_SECONDS, self.run_deadline) if self.run_deadline else start + CLEANUP_SECONDS
        self.cleanup_event('cleanup_started')
        others = [child for child in self.children if child is not self.coordinator]
        # Resume before requesting exit. A later CONT can cancel the SIGSTOP
        # used by Linux LeakSanitizer's exit-time tracer and leave it waiting
        # forever. Stopped children still need to run to handle TERM.
        for child in others:
            self.signal_child(child, signal.SIGCONT)
            self.signal_child(child, signal.SIGTERM)
        self.wait_groups(others, min(start + 4, deadline))
        for child in others:
            self.signal_child(child, signal.SIGKILL)
        if self.coordinator is not None:
            self.signal_child(self.coordinator, signal.SIGCONT)
            self.signal_child(self.coordinator, signal.SIGTERM)
        self.wait_groups(self.children, min(start + 6, deadline))
        for child in self.children:
            self.signal_child(child, signal.SIGKILL)
        self.wait_groups(self.children, deadline)
        remaining = [dict(role=c.role, pid=c.process.pid) for c in self.children
                     if c.process.returncode is None or not c.group_retired]
        for child in self.children:
            expected_exit = -signal.SIGKILL if child.intentional_crash else 0
            if (child.role == 'coordinator' or child.role.startswith('worker-')) and child.process.returncode not in (None, expected_exit):
                self.cleanup_errors.append(f'{child.role} exited {child.process.returncode}')
        if remaining:
            self.cleanup_errors.append(f'unreaped children or remaining owned groups: {remaining}')
        self.cleanup_result = dict(ok=not self.cleanup_errors, errors=self.cleanup_errors,
                                   remaining=remaining, elapsed_ms=round((time.monotonic() - start) * 1000))
        self.cleanup_event('cleanup_finished', ok=self.cleanup_result['ok'],
                           errors=self.cleanup_errors, remaining=remaining,
                           cleanup_elapsed_ms=self.cleanup_result['elapsed_ms'])
        self.cleanup_result['ok'] = not self.cleanup_errors
        return self.cleanup_result

    def run(self):
        original_handlers = {sig: signal.signal(sig, self.on_signal) for sig in (signal.SIGINT, signal.SIGTERM)}
        failure = None
        manifest = {}
        try:
            manifest = self.manifest()
            self.save('manifest.json', manifest)
            self.start_pool()
            manifest['endpoint'] = self.endpoint
            self.save('manifest.json', manifest)
            # Optional source metadata still uses owned, bounded helper processes.
            manifest['source'] = dict(revision=None, dirty=None)
            if shutil.which('git'):
                revision, code = self.command('git-revision', ['git', '-C', ROOT, 'rev-parse', 'HEAD'], self.work_deadline, True)
                dirty, dirty_code = self.command('git-status', ['git', '-C', ROOT, 'status', '--porcelain'], self.work_deadline, True)
                manifest['source'] = dict(revision=revision.strip() if code == 0 else None,
                                          dirty=bool(dirty) if dirty_code == 0 else None)
            self.save('manifest.json', manifest)
            self.submit()
            self.after_admission()
            self.drain_and_verify()
        except Exception as error:
            failure = f'{type(error).__name__}: {error}'
        finally:
            cleanup = self.cleanup()
            # Include diagnostics emitted during shutdown, after all writers exit.
            try:
                for child in self.children:
                    for path in (child.stdout, child.stderr):
                        text = path.read_text(encoding='utf-8', errors='replace')
                        require(not SANITIZER.search(text), f'sanitizer diagnostic in {path.name}')
                        require('[ERROR]' not in text, f'runtime error in {path.name}')
                if self.started is not None:
                    require(time.monotonic() <= self.run_deadline, 'overall deadline exceeded')
            except Exception as error:
                failure = failure or f'{type(error).__name__}: {error}'
            failure = failure or ('process cleanup failed' if not cleanup['ok'] else None)
            if self.stop_signal is not None:
                failure = failure or f'interrupted by signal {self.stop_signal}'
            code = 128 + self.stop_signal if self.stop_signal else (1 if failure else 0)
            summary = dict(contract=CONTRACT, mode=self.mode, verdict=self.mode.upper() + '_PASS' if code == 0 else 'FAIL',
                           exit_code=code, first_failure=failure, configuration=manifest.get('configuration'),
                           acknowledged=sum(entry['job_id'] is not None for entry in self.ledger),
                           verified_completed=sum(s['state'] == 'DONE' for s in self.snapshots.get('statuses', {}).values()),
                           verified_failed=sum(s['state'] == 'FAILED' for s in self.snapshots.get('statuses', {}).values()),
                           totals=self.snapshots.get('stats'), elapsed_ms=self.elapsed(),
                           accounting=self.accounting,
                           coverage=self.coverage(), cleanup=cleanup, **self.summary_details(),
                           children=[dict(role=c.role, pid=c.process.pid, argv=c.argv,
                                          started_ms=c.started_ms, exit_ms=c.exit_ms, returncode=c.process.returncode,
                                          reaped=c.exit_ms is not None, group_retired=c.group_retired,
                                          intentional_crash=c.intentional_crash, signals=c.signals) for c in self.children])
            try:
                self.save('submissions.json', self.ledger)
                self.event('verdict', verdict=summary['verdict'], exit_code=code, first_failure=failure)
                self.save('summary.json', summary)
            finally:
                self.events.close()
                for sig, handler in original_handlers.items():
                    signal.signal(sig, handler)
        print(f"{summary['verdict']}: acknowledged={summary['acknowledged']} "
              f"verified_completed={summary['verified_completed']} verified_failed={summary['verified_failed']}")
        print(f'Artifacts: {self.directory}')
        if failure:
            print(failure, file=sys.stderr)
        for error in cleanup['errors']:
            print(f'Cleanup: {error}', file=sys.stderr)
        return code


def parse_args(argv=None, *, chaos=False):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir', type=Path, default=ROOT / 'build/debug')
    parser.add_argument('--output-dir', type=Path, help='new directory; refuses an existing path')
    parser.add_argument('--workers', type=int, default=5)
    parser.add_argument('--jobs', type=int, default=100)
    parser.add_argument('--sleep-ms', type=int, default=3000)
    parser.add_argument('--max-retries', type=int, default=3)
    parser.add_argument('--seed', type=int, default=42, help='private generator seed; baseline makes no random choices')
    parser.add_argument('--fault-duration-ms', type=int, default=30000 if chaos else 0,
                        help='fault window; zero selects baseline' if chaos else 'must be zero; use run_chaos.py for faults')
    parser.add_argument('--deadline-ms', type=int, default=180000, help='total budget including 10000 ms cleanup reserve')
    args = parser.parse_args(argv)
    for name, lower, upper in [('workers', 1, 16), ('jobs', 1, 256), ('sleep_ms', 0, 86400000),
                               ('max_retries', 0, (1 << 32) - 1), ('seed', 0, (1 << 32) - 1),
                               ('deadline_ms', 10001, (1 << 31) - 1),
                               ('fault_duration_ms', 0, 20000000 if chaos else 0)]:
        if not lower <= getattr(args, name) <= upper:
            parser.error(f'--{name.replace("_", "-")} must be in {lower}..{upper}')
    if args.deadline_ms <= args.fault_duration_ms + CLEANUP_SECONDS * 1000:
        parser.error('--deadline-ms must exceed the fault window plus the 10000 ms cleanup reserve')
    if os.name != 'posix' or not hasattr(os, 'killpg'):
        parser.error('POSIX process groups are required (macOS or Linux)')
    args.bin_dir = args.bin_dir.resolve()
    for name in ('faultline', 'faultline-worker', 'faultline-coordinator'):
        path = args.bin_dir / name
        if not path.is_file() or not os.access(path, os.X_OK):
            parser.error(f'missing executable: {path}; build it first')
    try:
        if args.output_dir is not None:
            args.output_dir = args.output_dir.absolute()
            args.output_dir.mkdir(parents=True, exist_ok=False)
        else:
            parent = ROOT / 'build/chaos'
            parent.mkdir(parents=True, exist_ok=True)
            args.output_dir = Path(tempfile.mkdtemp(prefix='chaos-' if args.fault_duration_ms else 'baseline-', dir=parent))
    except OSError as error:
        parser.error(f'cannot create fresh output directory: {error}')
    return args


def main():
    args = parse_args()
    print(f'Baseline: {args.workers} workers, {args.jobs} sleep({args.sleep_ms} ms) jobs; artifacts: {args.output_dir}', flush=True)
    try:
        return BatchRun(args, args.output_dir).run()
    except (OSError, RunFailure) as error:
        print(f'harness error: {error}; artifacts: {args.output_dir}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
