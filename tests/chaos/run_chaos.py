#!/usr/bin/env python3
"""Seeded, bounded worker crashes with replacements and coordinator evidence."""

import errno
import random
import re
import shlex
import signal
import sys
import time

from run_batch import BatchRun, RunFailure, pairs, parse_args, require, table, unsigned


def candidate_plan(seed, workers, duration_ms):
    generator = random.Random(seed)
    plan = []
    for index in range(duration_ms // 2000):
        gap = generator.randint(2000, 10000)
        slots = list(range(workers))
        generator.shuffle(slots)
        plan.append(dict(index=index, gap_ms=gap, slots=slots))
    return plan


def log_record(line):
    match = re.fullmatch(r'\S+ \[(INFO|WARN|ERROR)\] (\w+) (\w+) (.*)', line)
    require(match is not None, 'malformed coordinator log record')
    items = [item.split('=', 1) for item in shlex.split(match[4])]
    require(all(len(item) == 2 for item in items), 'malformed coordinator log fields')
    fields = dict(items)
    require(len(fields) == len(items), 'duplicate coordinator log field')
    return match[1], match[2], match[3], fields


class ChaosRun(BatchRun):
    mode = 'chaos'

    def __init__(self, args, directory):
        super().__init__(args, directory)
        self.plan = []
        self.actions = []
        self.crashes = {}  # Coordinator-issued ID -> one actual signal action.
        self.generations = {}
        self.active_leases = {}  # Coordinator log history, independent of CLI snapshots.
        self.worker_fds = {}
        self.transport_warnings = []
        self.window = None
        self.unused_plan_from = None
        self.drain_eligible = set()
        self.insufficient_coverage = False

    def manifest(self):
        # Prepare the entire plan once, before any process starts. No RNG in polls.
        self.plan = candidate_plan(self.args.seed, self.args.workers, self.args.fault_duration_ms)
        return {**super().manifest(), 'candidate_plan': self.plan, 'seed_used': True,
                'random_generator': 'random.Random / MT19937; randint then shuffle per candidate'}

    def runtime_line(self, child, line, path):
        if child is not self.coordinator:
            return super().runtime_line(child, line, path)
        level, component, event, fields = log_record(line)
        require(component == 'coordinator' and fields.get('pid') == str(child.process.pid),
                'coordinator log process mismatch')
        require(level != 'ERROR', f'unexpected runtime diagnostic in {path.name}')
        if event == 'worker_registered':
            self.worker_fds[unsigned(fields['worker_id'], 'registered worker')] = fields['fd']
        elif event in ('job_assigned', 'job_started', 'job_completed'):
            wid = unsigned(fields['worker_id'], 'lease worker')
            lease = dict(job_id=unsigned(fields['job_id'], 'lease job'),
                         attempt=unsigned(fields['attempt'], 'lease attempt'), state=fields['state'])
            if event == 'job_completed':
                active = self.active_leases.pop(wid, None)
                require(active and (active['job_id'], active['attempt']) == (lease['job_id'], lease['attempt']),
                        'completion without matching logged lease')
            else:
                if event == 'job_assigned':
                    require(wid not in self.active_leases, 'worker assigned two active jobs')
                else:
                    active = self.active_leases.get(wid)
                    require(active and (active['job_id'], active['attempt']) == (lease['job_id'], lease['attempt']),
                            'STARTED without matching logged lease')
                self.active_leases[wid] = lease
        elif event == 'job_worker_lost':
            wid = unsigned(fields['previous_worker_id'], 'lost worker')
            action = self.crashes.get(wid)
            require(action is not None and action['loss'] is None and action['death'] is None,
                    'unplanned or duplicate job loss')
            active = self.active_leases.pop(wid, None)
            job_id, attempt = unsigned(fields['job_id'], 'lost job'), unsigned(fields['attempt'], 'lost attempt')
            require(active and (active['job_id'], active['attempt']) == (job_id, attempt),
                    'job loss does not match coordinator lease history')
            retrying = attempt <= self.args.max_retries
            require(fields['outcome'] == ('REQUEUED' if retrying else 'FAILED') and
                    fields['state'] == ('QUEUED' if retrying else 'FAILED') and
                    fields['worker_id'] == ('0' if retrying else str(wid)) and
                    fields['retry_count'] == str(attempt if retrying else self.args.max_retries) and
                    fields['max_retries'] == str(self.args.max_retries) and
                    fields['failure'] == 'WORKER_LOST' and fields['result_bytes'] == '0' and
                    fields['durable'] == '1', 'invalid worker-loss retry transition')
            action['loss'] = dict(job_id=job_id, attempt=attempt, outcome=fields['outcome'],
                                  wal_sequence=unsigned(fields['wal_sequence'], 'loss WAL sequence'))
            self.event('coordinator_job_loss', worker_id=wid, **action['loss'])
        elif event in ('worker_dead', 'client_closed'):
            wid = unsigned(fields['worker_id'], 'closed worker')
            action = self.crashes.get(wid)
            if action is None:
                require(event == 'client_closed' and wid == 0 and fields['reason'] == 'eof',
                        'unplanned worker death or connection closure')
            else:
                require(fields['fd'] == action['fd'] and
                        fields['reason'] in ('eof', 'recv_error', 'send_error', 'send_closed', 'truncated_message'),
                        'unexpected crash connection close reason')
                key = 'death' if event == 'worker_dead' else 'closed'
                require(action[key] is None, 'duplicate worker close event')
                require(wid not in self.active_leases, 'worker death omitted an active job-loss transition')
                action[key] = fields
                self.event('coordinator_' + event, worker_id=wid, fields=fields)
        elif level == 'WARN':
            # SIGKILL can end a TCP stream between reads or cause a reset during
            # send/recv. These diagnostics lack worker IDs; match them to the
            # specific crash's close record before allowing that cycle to finish.
            require(bool(self.crashes), f'unexpected runtime diagnostic in {path.name}')
            if event == 'system_error':
                require(fields['operation'] in ('recv', 'send') and
                        unsigned(fields['errno'], 'transport errno') in (errno.ECONNRESET, errno.EPIPE),
                        'unexpected transport diagnostic')
            else:
                require(event == 'truncated_message' and unsigned(fields['bytes'], 'truncated bytes') > 0,
                        f'unexpected runtime diagnostic in {path.name}')
            self.transport_warnings.append(dict(event=event, fields=fields, matched=False))

    def account_transport_warnings(self, action):
        close = action['closed']
        reason = close['reason']
        matches = []
        for warning in self.transport_warnings:
            if warning['matched']:
                continue
            fields = warning['fields']
            if warning['event'] == 'system_error':
                compatible = reason == fields['operation'] + '_error'
            else:
                compatible = reason == 'truncated_message' and fields['fd'] == action['fd']
            # Both timestamps come from this coordinator's clock. Stream reads
            # can be reordered, but the diagnostic precedes the close in C.
            delta = int(close['monotonic_ms']) - int(fields['monotonic_ms'])
            if compatible and 0 <= delta <= 5000:
                matches.append(warning)
        needed = reason in ('recv_error', 'send_error', 'truncated_message')
        require(len(matches) == int(needed), 'missing or ambiguous crash transport diagnostic')
        for warning in matches:
            warning['matched'] = True
            self.event('crash_transport_diagnostic', worker_id=action['worker_id'], diagnostic=warning)
        require(all(w['matched'] for w in self.transport_warnings), 'unattributed transport warning')

    def verify_workers(self, rows, idle=False):
        require(set(rows) == set(self.workers.values()), 'worker listing does not match the replaced pool')
        for row in rows.values():
            require(row['LIVENESS'] == 'ALIVE' and unsigned(row['HEARTBEAT_AGE_MS'], 'heartbeat age') < 6000,
                    'worker is dead or expired')
            attempt = unsigned(row['ATTEMPT'], 'worker attempt')
            if row['JOB_ID'] == 'none':
                require(attempt == 0, 'idle worker still has an attempt')
            else:
                require(not idle and unsigned(row['JOB_ID'], 'worker job') > 0 and
                        1 <= attempt <= self.args.max_retries + 1, 'unexpected worker lease')

    def observe_jobs(self, rows):
        expected = {entry['job_id'] for entry in self.ledger}
        require(len(self.ledger) == self.args.jobs and None not in expected and set(rows) == expected,
                'job listing differs from acknowledged ID set')
        for job_id, row in rows.items():
            state = row['STATE']
            require(row['TASK'] == 'sleep' and state in ('QUEUED', 'ASSIGNED', 'RUNNING', 'DONE', 'FAILED'),
                    'unexpected job task/state')
            attempt = unsigned(row['ATTEMPT'], 'job attempt')
            retries = row['RETRIES'].split('/')
            require(len(retries) == 2 and retries[1] == str(self.args.max_retries), 'retry allowance changed')
            retry = unsigned(retries[0], 'retry count', self.args.max_retries)
            require(attempt <= self.args.max_retries + 1, 'attempt allowance exceeded')
            if state == 'QUEUED':
                require(row['WORKER_ID'] == 'none' and attempt == retry and
                        row['FAILURE'] == ('NONE' if attempt == 0 else 'WORKER_LOST'), 'invalid queued retry')
            else:
                require(attempt == retry + 1 and unsigned(row['WORKER_ID'], 'job worker') in self.all_worker_ids,
                        'invalid job attempt/owner')
                if state == 'FAILED':
                    require(retry == self.args.max_retries and row['FAILURE'] == 'WORKER_LOST' and
                            job_id not in self.drain_eligible, 'unexpected terminal job failure')
                else:
                    require(row['FAILURE'] == 'NONE', 'unexpected job failure reason')
            require(row['RESULT_BYTES'] == str(len(self.expected_result) if state == 'DONE' else 0),
                    'unexpected result length')
            if job_id in self.terminal:
                require(row == self.terminal[job_id], f'terminal job {job_id} changed')
            if state in ('DONE', 'FAILED'):
                self.terminal[job_id] = row
            if row != self.observations.get(job_id):
                self.event('job_observed', job_id=job_id, snapshot=row)
                self.observations[job_id] = row

    def status(self, job_id, row):
        if row['STATE'] == 'DONE':
            return super().status(job_id, row)
        result = pairs(self.cli('status', str(job_id)),
                       ['job_id', 'state', 'worker_id', 'attempt', 'retries', 'failure', 'result_bytes'])
        require(row['STATE'] == 'FAILED' and all(result[key] == row[key.upper()] for key in result),
                'failed status/listing disagreement')
        return result

    def expected_stats(self):
        done = sum(row['STATE'] == 'DONE' for row in self.terminal.values())
        failed = sum(row['STATE'] == 'FAILED' for row in self.terminal.values())
        attempts = sum(int(row['ATTEMPT']) for row in self.terminal.values())
        retries = sum(int(row['RETRIES'].split('/')[0]) for row in self.terminal.values())
        return {**super().expected_stats(), 'jobs_completed_total': done, 'jobs_failed_total': failed,
                'job_attempts_total': attempts, 'job_retries_total': retries,
                'session_jobs_completed': done, 'session_jobs_failed': failed, 'session_job_retries': retries}

    def wait_until(self, deadline):
        while time.monotonic() < deadline:
            self.pause(self.work_deadline, min(.2, max(0, deadline - time.monotonic())))
        self.check()

    def inject(self, candidate, window_deadline):
        rows = table(self.cli('jobs'), 'jobs')
        self.observe_jobs(rows)
        workers = table(self.cli('workers'), 'workers')
        self.verify_workers(workers)
        for slot in candidate['slots']:
            wid = self.workers[slot]
            worker = workers[wid]
            if worker['JOB_ID'] != 'none':
                break
        else:
            self.event('fault_skipped', candidate=candidate['index'], reason='no_busy_worker')
            return
        child = self.pool[slot]
        generation = self.generations.get(slot, 0)
        observed_id, observed_attempt = int(worker['JOB_ID']), int(worker['ATTEMPT'])
        observed_row = rows[observed_id]
        state = observed_row['STATE'] if (observed_row['WORKER_ID'] == str(wid) and
                observed_row['ATTEMPT'] == str(observed_attempt) and
                observed_row['STATE'] in ('ASSIGNED', 'RUNNING')) else 'ASSIGNED_OR_RUNNING'
        self.check()
        action = dict(candidate=candidate['index'], slot=slot, generation=generation, worker_id=wid,
                      pid=child.process.pid, fd=self.worker_fds[wid],
                      observed=dict(job_id=observed_id, attempt=observed_attempt, state=state),
                      job_snapshot=observed_row, loss=None, death=None, closed=None, replacement=None)
        self.event('fault_selected', **action)
        # Check after snapshot reads and evidence writes. Neither can extend the
        # fault window. Popen.kill uses the owned handle, not a saved numeric PID.
        self.check()
        require(child.process.poll() is None, 'selected worker exited before SIGKILL')
        if time.monotonic() >= window_deadline:
            self.event('fault_skipped', candidate=candidate['index'], reason='window_elapsed_during_selection')
            return
        child.process.kill()
        child.intentional_crash = True
        child.signals.append(signal.SIGKILL)
        action['signal_elapsed_ms'] = self.elapsed()
        self.actions.append(action)
        self.crashes[wid] = action
        self.event('fault_signal', worker_id=wid, pid=child.process.pid, signal=signal.SIGKILL,
                   candidate=candidate['index'], slot=slot, generation=generation)
        limit = min(self.work_deadline, time.monotonic() + 5)
        while True:
            self.check(limit)
            if (child.process.returncode == -signal.SIGKILL and child.group_retired and
                    action['death'] is not None and action['closed'] is not None):
                break
            self.pause(limit, .02)
        self.account_transport_warnings(action)
        self.event('crash_accounted', worker_id=wid, lost_lease=action['loss'], observed=action['observed'])
        replacement = self.spawn_worker(slot, generation + 1)
        self.generations[slot] = generation + 1
        limit = min(self.work_deadline, time.monotonic() + 5)
        while not self.read_registration(slot, replacement, generation + 1):
            self.pause(limit, .02)
        self.check(limit)
        action['replacement'] = dict(worker_id=self.workers[slot], pid=replacement.process.pid,
                                     generation=generation + 1)
        self.verify_workers(table(self.cli('workers', deadline=limit), 'workers'))
        action['completed_elapsed_ms'] = self.elapsed()
        self.event('replacement_ready', previous_worker_id=wid, slot=slot, **action['replacement'])
        self.save('fault-actions.json', self.actions)

    def after_admission(self):
        self.verify_workers(table(self.cli('workers'), 'workers'))
        start = time.monotonic()
        deadline = start + self.args.fault_duration_ms / 1000
        require(deadline < self.work_deadline, 'full fault window does not fit remaining work budget')
        self.window = dict(start_elapsed_ms=self.elapsed(), duration_ms=self.args.fault_duration_ms)
        self.event('fault_window_started', **self.window)
        for candidate in self.plan:
            gap_start = start if candidate['index'] == 0 else time.monotonic()
            due = gap_start + candidate['gap_ms'] / 1000
            self.event('fault_scheduled', candidate=candidate['index'], gap_ms=candidate['gap_ms'])
            if due >= deadline:
                self.unused_plan_from = candidate['index']
                self.event('unused_plan', from_index=self.unused_plan_from, reason='next_gap_exceeds_window')
                break
            self.wait_until(due)
            self.inject(candidate, deadline)
        # A final in-progress cycle may finish after the window. No new kills
        # occur then. All its loss accounting completes before drain eligibility.
        if time.monotonic() < deadline:
            self.wait_until(deadline)
        self.window['settled_elapsed_ms'] = self.elapsed()
        self.event('fault_window_settled', **self.window)
        rows = table(self.cli('jobs'), 'jobs')
        self.observe_jobs(rows)
        self.verify_workers(table(self.cli('workers'), 'workers'))
        self.drain_eligible = {job_id for job_id, row in rows.items() if row['STATE'] not in ('DONE', 'FAILED')}
        self.event('drain_started', eligible_job_ids=sorted(self.drain_eligible))

    def coverage(self):
        lost = [action['loss'] for action in self.actions if action['loss']]
        resumed = sorted({loss['job_id'] for loss in lost if loss['job_id'] in self.terminal and
                          self.terminal[loss['job_id']]['STATE'] == 'DONE' and
                          int(self.terminal[loss['job_id']]['ATTEMPT']) > loss['attempt']})
        exhausted = sorted({loss['job_id'] for loss in lost if loss['outcome'] == 'FAILED' and
                            self.terminal.get(loss['job_id'], {}).get('STATE') == 'FAILED'})
        return dict(injected_faults=len(self.actions), interrupted_attempts=len(lost),
                    recovered_job_ids=resumed, exhausted_job_ids=exhausted,
                    recovery_demonstrated=bool(resumed),
                    sufficient=bool(self.actions and lost and (resumed if self.args.max_retries else exhausted)))

    def verify_coverage(self):
        # Account for every retry and terminal failure, not only one lucky job.
        for job_id, row in self.terminal.items():
            losses = [a['loss'] for a in self.actions if a['loss'] and a['loss']['job_id'] == job_id]
            requeues = [loss for loss in losses if loss['outcome'] == 'REQUEUED']
            require(len(requeues) == int(row['RETRIES'].split('/')[0]), 'retry without attributed worker loss')
            require({loss['attempt'] for loss in requeues} == set(range(1, len(requeues) + 1)),
                    'lost-attempt history has gaps or duplicates')
            require(sum(loss['outcome'] == 'FAILED' for loss in losses) == int(row['STATE'] == 'FAILED'),
                    'terminal failure without attributed worker loss')
            require(all(loss['attempt'] == int(row['ATTEMPT']) for loss in losses if loss['outcome'] == 'FAILED'),
                    'terminal failure has a different interrupted attempt')
        self.insufficient_coverage = not self.coverage()['sufficient']
        require(not self.insufficient_coverage, 'INSUFFICIENT_COVERAGE: no qualifying interrupted attempt outcome')

    def summary_details(self):
        return dict(seed=self.args.seed, fault_window=self.window, unused_plan_from=self.unused_plan_from,
                    fault_actions=self.actions, drain_eligible_job_ids=sorted(self.drain_eligible),
                    coverage_status='INSUFFICIENT_COVERAGE' if self.insufficient_coverage else
                                    ('SUFFICIENT' if self.coverage()['sufficient'] else 'INCOMPLETE'))


def main():
    args = parse_args(chaos=True)
    run_class = ChaosRun if args.fault_duration_ms else BatchRun
    print(f'{run_class.mode}: seed={args.seed}, workers={args.workers}, jobs={args.jobs}, '
          f'fault_window_ms={args.fault_duration_ms}; artifacts: {args.output_dir}', flush=True)
    try:
        return run_class(args, args.output_dir).run()
    except (OSError, RunFailure) as error:
        print(f'harness error: {error}; artifacts: {args.output_dir}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
