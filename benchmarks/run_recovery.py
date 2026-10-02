"""Controlled recovery samples and paired metrics; invoked by run_baseline.py.

One event loop owns admission, inspection, fault timers, and every process. No
background thread can outlive cleanup or issue a signal after its deadline.
"""

import errno
import json
import math
import os
import re
import signal
import statistics
import time

import run_baseline as base
from run_batch import BatchRun, RunFailure, SANITIZER, pairs, require, table, unsigned
from run_chaos import ChaosRun, log_record

PROFILE = base.ROOT / 'benchmarks/profiles/recovery-v1.json'
SCENARIOS = ('no_fault', 'sigkill', 'heartbeat_expiry')


def load_profile():
    profile = json.loads(PROFILE.read_text())
    expected = dict(contract='faultline-benchmarks-v1', profile='recovery-sleep-v1', task='sleep',
        arguments='10000', expected_result='slept_ms=10000', expected_result_bytes=14,
        jobs=32, workers=4, max_retries=1, fault_target='first_acknowledged_job_attempt_1',
        fault_delay_after_started_observation_ms=500,
        replacement_policy='one_replacement_after_matching_durable_worker_loss',
        heartbeat_interval_ms=2000, heartbeat_timeout_ms=6000, poll_interval_ms=200,
        cooldown_ms=30000, run_deadline_ms=600000, cleanup_reserve_ms=10000,
        campaign_deadline_ms=7200000, warmup_order=list(SCENARIOS))
    require(all(profile.get(k) == v for k, v in expected.items()), 'unsupported recovery profile')
    require(profile['scenarios'] == dict(no_fault=dict(faults_per_run=0),
        sigkill=dict(faults_per_run=1, signal='SIGKILL'), heartbeat_expiry=dict(faults_per_run=1,
        signal='SIGSTOP', pause_duration_ms=10000, resume_signal='SIGCONT')), 'unsupported fault policy')
    require(profile['build'] == dict(cc='clang', cflags='-O2 -g -Werror', sanitize=0,
        cpu_tuning='compiler_default', lto=False), 'unsupported build settings')
    require(profile['measured_rounds'] == [list(SCENARIOS), ['sigkill','heartbeat_expiry','no_fault'],
        ['heartbeat_expiry','no_fault','sigkill'], ['heartbeat_expiry','sigkill','no_fault'],
        ['sigkill','no_fault','heartbeat_expiry']], 'unsupported recovery sample plan')
    return profile


def sample_plan(profile):
    order = [('warmup', None, s) for s in profile['warmup_order']]
    order += [('measured', number, s) for number, scenarios in enumerate(profile['measured_rounds'], 1)
              for s in scenarios]
    return [dict(campaign_index=i, stage=stage, measured_round=number, scenario=scenario,
                 workers=profile['workers']) for i, (stage, number, scenario) in enumerate(order)]


def describe(values):
    require(values and all(math.isfinite(v) for v in values), 'invalid aggregate values')
    return dict(values=values, median=statistics.median(values), minimum=min(values), maximum=max(values))


def aggregate(samples, profile):
    plan = sample_plan(profile)
    require(len(samples) == len(plan), 'incomplete recovery campaign')
    for sample, expected in zip(samples, plan):
        require(all(sample.get(k) == v for k, v in expected.items()), 'recovery sample order/configuration mismatch')
        require(sample['exit_code'] == 0 and sample['timing_valid'], 'invalid recovery sample')
        faults = int(expected['scenario'] != 'no_fault')
        require(sample['accounting']['counts'] == dict(submitted=profile['jobs'], completed=profile['jobs'],
            terminally_failed=0), 'incomplete recovery accounting')
        metrics = sample['metrics']
        require(metrics['retries'] == metrics['actual_faults'] == faults and metrics['terminal_failures'] == 0
                and metrics['attempts'] == profile['jobs'] + faults, 'wrong recovery counts')
        for key, value in metrics.items():
            require(value is None or (math.isfinite(value) and value >= 0), f'invalid recovery metric {key}')
        require(metrics['batch_elapsed_ms'] > 0, 'nonpositive batch time')
        for key in ('observed_detection_ms', 'observed_reassignment_ms', 'observed_recovery_to_completion_ms',
                    'coordinator_loss_to_assignment_ms', 'coordinator_loss_to_completion_ms'):
            require((metrics[key] is not None) == bool(faults), 'missing or fabricated recovery timing')
    grouped = {}
    controls = {s['measured_round']:s for s in samples
                if s['stage'] == 'measured' and s['scenario'] == 'no_fault'}
    for scenario in SCENARIOS:
        selected = [s for s in samples if s['stage'] == 'measured' and s['scenario'] == scenario]
        require(len(selected) == 5, 'need five matching measured rounds')
        metrics = {key: describe([s['metrics'][key] for s in selected])
                   for key, value in selected[0]['metrics'].items() if value is not None}
        paired = []
        if scenario != 'no_fault':
            for s in selected:
                control = controls[s['measured_round']]
                delta = s['metrics']['batch_elapsed_ms'] - control['metrics']['batch_elapsed_ms']
                paired.append(dict(measured_round=s['measured_round'], fault_index=s['campaign_index'],
                    control_index=control['campaign_index'], extra_completion_ms=delta,
                    overhead_percent=100 * delta / control['metrics']['batch_elapsed_ms']))
            metrics['extra_completion_ms'] = describe([p['extra_completion_ms'] for p in paired])
            metrics['overhead_percent'] = describe([p['overhead_percent'] for p in paired])
        grouped[scenario] = dict(sample_indices=[s['campaign_index'] for s in selected], metrics=metrics, pairs=paired)
    return grouped


def recovery_history(transitions, ids, worker_ids, target, scenario, expected, max_retries):
    """Validate every durable transition before deriving coordinator-clock latency."""
    require(set(transitions) == set(ids), 'log job IDs differ from acknowledged IDs')
    interrupted = scenario != 'no_fault'
    target_id = target['job_id']
    ordinary = {j:e for j,e in transitions.items() if not interrupted or j != target_id}
    latency = (base.latency_report(ordinary, set(ordinary), worker_ids, 'sleep', expected, max_retries)
               if ordinary else dict(jobs=[]))
    history = {j:[] for j in ids}
    if interrupted:
        events = transitions[target_id]
        require(len(events) == 7, 'missing or duplicate recovery transitions')
        old = str(target['worker_id'])
        new = events[4]['worker_id']
        require(new != old and int(new) in worker_ids, 'retry assigned to invalid or stale worker')
        sequence = [
            ('job_submitted','QUEUED','ACCEPTED','0','0','0','NONE','0'),
            ('job_assigned','ASSIGNED','ASSIGNED',old,'1','0','NONE','0'),
            ('job_started','RUNNING','RUNNING',old,'1','0','NONE','0'),
            ('job_worker_lost','QUEUED','REQUEUED','0','1','1','WORKER_LOST',old),
            ('job_assigned','ASSIGNED','ASSIGNED',new,'2','1','NONE','0'),
            ('job_started','RUNNING','RUNNING',new,'2','1','NONE','0'),
            ('job_completed','DONE','COMPLETED',new,'2','1','NONE','0')]
        for e, values in zip(events, sequence):
            required = dict(zip(('event','state','outcome','worker_id','attempt','retry_count','failure',
                                 'previous_worker_id'), values))
            required.update(task='sleep', max_retries=str(max_retries), durable='1',
                            result_bytes=str(len(expected) if e['event']=='job_completed' else 0))
            require(all(e.get(k) == v for k,v in required.items()), 'invalid interrupted-job lifecycle')
        stamps = [unsigned(e['monotonic_ms'], 'coordinator timestamp') for e in events]
        seqs = [unsigned(e['wal_sequence'], 'WAL sequence') for e in events]
        require(stamps == sorted(stamps) and all(a < b for a,b in zip(seqs,seqs[1:])), 'invalid recovery log ordering')
        require(events[-1].get('result') == expected, 'incorrect recovered result')
        latency['jobs'].append(dict(job_id=target_id, submitted_ms=stamps[0], completed_ms=stamps[-1],
                                    accepted_latency_ms=stamps[-1]-stamps[0]))
        history[target_id] = [dict(worker_id=int(old), job_id=target_id, attempt=1,
                                   outcome='REQUEUED', wal_sequence=seqs[3])]
    values = sorted(e['accepted_latency_ms'] for e in latency['jobs'])
    latency.update(mean_accepted_latency_ms=statistics.mean(values),
                   p95_accepted_latency_ms=values[math.ceil(.95*len(values))-1])
    latency['jobs'].sort(key=lambda e:e['job_id'])
    return latency, history


def recovery_delays(fault):
    """Keep observer intervals and coordinator intervals in separate clocks."""
    f = fault
    observed = [f['signal_before_ns'], f['signal_return_ns'], f['loss']['observed_ns'],
                f['reassigned']['observed_ns'], f['restarted']['observed_ns'], f['completed']['observed_ns']]
    require(observed == sorted(observed), 'invalid observed recovery ordering')
    logs = [int(f[k]['monotonic_ms']) for k in ('loss','reassigned','completed')]
    require(logs == sorted(logs), 'invalid coordinator recovery ordering')
    replacement = [observed[2], f['replacement_launch_before_ns'], f['replacement_launch_return_ns'],
                   f['replacement_registered_ns']]
    require(replacement == sorted(replacement), 'invalid replacement timing')
    return dict(observed_detection_ms=(observed[2]-observed[0])/1e6,
        observed_reassignment_ms=(observed[3]-observed[2])/1e6,
        observed_recovery_to_completion_ms=(observed[-1]-observed[0])/1e6,
        coordinator_loss_to_assignment_ms=logs[1]-logs[0], coordinator_loss_to_completion_ms=logs[2]-logs[0],
        signal_syscall_ms=(observed[1]-observed[0])/1e6,
        replacement_launch_delay_ms=(replacement[1]-replacement[0])/1e6,
        replacement_registration_ms=(replacement[-1]-replacement[1])/1e6)


class RecoveryRun(base.BaselineRun):
    mode = 'benchmark_recovery_sample'

    def __init__(self, *args, scenario, **kwargs):
        super().__init__(*args, **kwargs)
        require(scenario in SCENARIOS, 'unknown recovery scenario')
        self.scenario = scenario
        self.target = None
        self.fault = dict(scenario=scenario)
        self.fault_child = self.replacement = None
        self.worker_fds = {}
        self.active = {}
        self.transport_warnings = []
        self._ticking = False
        self.query = None
        self.query_started = self.next_query = None
        self.last_rows = None
        self.retained_workers = None

    def manifest(self):
        return {**super().manifest(), 'scenario':self.scenario, 'reportable_recovery_campaign':False}

    def expected_runtime_exit(self, child):
        return (super().expected_runtime_exit(child) or (child is self.fault_child and
            self.scenario == 'heartbeat_expiry' and 'resume_return_ns' in self.fault and
            child.process.returncode == 1))

    def expected_cleanup_exit(self, child):
        return 1 if self.expected_runtime_exit(child) and self.scenario == 'heartbeat_expiry' else super().expected_cleanup_exit(child)

    def verify_old_worker_error(self, child, line):
        require(child is self.fault_child and self.scenario == 'heartbeat_expiry' and
                'resume_return_ns' in self.fault and 'closed' in self.fault, 'unattributed worker diagnostic')
        level, component, event, fields = log_record(line)
        require(level == 'ERROR' and component == 'worker' and fields.get('pid') == str(child.process.pid),
                'wrong old-worker diagnostic identity')
        allowed = (event == 'runtime_error' and fields.get('message') == 'worker: coordinator disconnected')
        allowed |= (event == 'system_error' and fields.get('operation') in
                    ('send job report', 'send heartbeat', 'coordinator connection') and
                    fields.get('errno') in (str(errno.EPIPE), str(errno.ECONNRESET)))
        allowed |= (event == 'stopped' and fields.get('worker_id') == str(self.target['worker_id']) and
                    fields.get('exit_code') == '1' and fields.get('reason') == 'error' and fields.get('signal') == '0')
        require(allowed, 'unexpected old-worker diagnostic')

    def verify_final_log(self, child, path, text):
        for line in text.splitlines():
            if '[ERROR]' in line:
                self.verify_old_worker_error(child, line)
        # Shutdown warnings are allowed only for owned, idle workers after a
        # recorded termination request. Workload warnings were checked live.
        if child is self.coordinator:
            for line in text.splitlines():
                if '[WARN]' not in line:
                    continue
                _, _, event, fields = log_record(line)
                if event == 'worker_dead' and fields.get('reason') == 'eof' and self.cleaning:
                    wid = int(fields['worker_id'])
                    if wid in self.workers.values() and self.t_done is not None:
                        slot = next(s for s,w in self.workers.items() if w == wid)
                        require(self.pool[slot].shutdown_requested, 'unrequested worker shutdown')
                        continue
                require(any(e.get('event') == event and e.get('fields') == fields
                            for e in self.fault.get('accepted_warnings', [])), 'unattributed final warning')

    def runtime_line(self, child, line, path):
        if child is not self.coordinator:
            if '[ERROR]' in line:
                self.verify_old_worker_error(child, line)
            else:
                BatchRun.runtime_line(self, child, line, path)
            return
        level, component, name, fields = log_record(line)
        require(component == 'coordinator' and fields.get('pid') == str(child.process.pid) and level != 'ERROR',
                'unexpected coordinator diagnostic or identity')
        now = time.monotonic_ns()
        if name == 'worker_registered':
            self.worker_fds[int(fields['worker_id'])] = fields['fd']
        if name.startswith('job_'):
            require(name in ('job_submitted','job_assigned','job_started','job_completed','job_worker_lost'),
                    'unexpected job transition')
            job_id, wid, attempt = (unsigned(fields[k],k) for k in ('job_id','worker_id','attempt'))
            sequence = unsigned(fields['wal_sequence'], 'WAL sequence')
            require(sequence > self.last_sequence, 'duplicate/non-increasing WAL sequence')
            self.last_sequence = sequence
            event = dict(fields, event=name, observed_ns=now)
            self.transitions.setdefault(job_id, []).append(event)
            self.event('coordinator_transition', fields=event)
            if name == 'job_assigned':
                require(wid not in self.active, 'worker assigned simultaneous jobs')
                self.active[wid] = (job_id, attempt)
            elif name in ('job_started','job_completed'):
                require(self.active.get(wid) == (job_id,attempt), 'transition lacks matching lease')
                if name == 'job_completed': del self.active[wid]
            elif name == 'job_worker_lost':
                require(self.target and self.fault.get('signal_return_ns') and
                    job_id == self.target['job_id'] and fields['previous_worker_id'] == str(self.target['worker_id'])
                    and attempt == 1 and 'loss' not in self.fault and fields['durable'] == '1'
                    and fields['state'] == 'QUEUED' and fields['outcome'] == 'REQUEUED', 'unplanned or invalid loss')
                require(self.active.pop(self.target['worker_id'], None) == (job_id,1), 'loss has wrong lease')
                self.fault['loss'] = event
            if self.target and job_id == self.target['job_id'] and attempt == 2:
                key = dict(job_assigned='reassigned', job_started='restarted', job_completed='completed').get(name)
                if key:
                    require(key not in self.fault, 'duplicate recovery observation')
                    self.fault[key] = event
        elif name in ('heartbeat_timeout','worker_dead','client_closed'):
            wid = unsigned(fields['worker_id'], 'closed worker')
            if name == 'client_closed' and wid == 0 and fields['reason'] == 'eof':
                return
            require(self.target and wid == self.target['worker_id'] and self.fault.get('signal_return_ns') and
                    fields['fd'] == self.target['fd'], 'unexpected worker loss')
            if name == 'heartbeat_timeout':
                require(self.scenario == 'heartbeat_expiry' and fields['timeout_ms'] == '6000' and
                        int(fields['silence_ms']) >= 6000 and 'resume_return_ns' not in self.fault,
                        'wrong or premature heartbeat detection')
                key = 'timeout'
            else:
                allowed = ('heartbeat_timeout',) if self.scenario == 'heartbeat_expiry' else (
                    'eof','recv_error','send_error','send_closed','truncated_message')
                require(fields['reason'] in allowed and self.target['worker_id'] not in self.active,
                        'wrong detector or missing loss transition')
                key = 'death' if name == 'worker_dead' else 'closed'
            require(key not in self.fault, 'duplicate worker loss record')
            self.fault[key] = dict(fields, observed_ns=now)
        elif level == 'WARN':
            require(self.scenario == 'sigkill' and self.fault.get('signal_return_ns'), 'unexpected warning')
            require((name == 'system_error' and fields.get('operation') in ('recv','send') and
                     fields.get('errno') in (str(errno.EPIPE),str(errno.ECONNRESET))) or
                    (name == 'truncated_message' and fields.get('fd') == self.target['fd']),
                    'unexpected transport warning')
            self.transport_warnings.append(dict(event=name, fields=fields, matched=False))
        if level == 'WARN':
            self.fault.setdefault('accepted_warnings', []).append(dict(event=name, fields=fields))

    def check(self, deadline=None):
        super().check(deadline)
        if not self.cleaning and not self._ticking:
            self._ticking = True
            try:
                self.advance_fault()
            finally:
                self._ticking = False

    def advance_fault(self):
        now = time.monotonic_ns()
        if self.target is None and self.ledger and self.ledger[0]['job_id']:
            job_id = self.ledger[0]['job_id']
            starts = [e for e in self.transitions.get(job_id, []) if e['event'] == 'job_started']
            if starts:
                require(len(starts) == 1 and starts[0]['attempt'] == '1', 'missed initial target attempt')
                e = starts[0]
                wid = int(e['worker_id'])
                slot = next(s for s,w in self.workers.items() if w == wid)
                self.target = dict(job_id=job_id, worker_id=wid, attempt=1, slot=slot,
                                   pid=self.pool[slot].process.pid, fd=self.worker_fds[wid])
                self.fault.update(target=self.target, started_observed_ns=e['observed_ns'],
                    scheduled_ns=e['observed_ns'] + self.profile['fault_delay_after_started_observation_ms']*1000000)
                self.event('intervention_scheduled', **self.fault)
        if self.target is None:
            return
        if 'checkpoint_ns' not in self.fault and now >= self.fault['scheduled_ns']:
            require(self.active.get(self.target['worker_id']) == (self.target['job_id'],1), 'target no longer running')
            child = self.pool[self.target['slot']]
            require(child.process.poll() is None, 'target exited before intervention')
            self.fault['checkpoint_ns'] = time.monotonic_ns()
            if self.scenario != 'no_fault':
                self.fault_child = child
                sig = signal.SIGKILL if self.scenario == 'sigkill' else signal.SIGSTOP
                self.fault['signal_before_ns'] = time.monotonic_ns()
                child.process.send_signal(sig)
                self.fault['signal_return_ns'] = time.monotonic_ns()
                child.signals.append(sig)
                child.intentional_crash = self.scenario == 'sigkill'
            self.event('intervention', **self.fault)
        if self.scenario == 'no_fault' or 'signal_return_ns' not in self.fault:
            return
        if self.scenario == 'heartbeat_expiry':
            if 'stopped_observed_ns' not in self.fault:
                pid, status = os.waitpid(self.fault_child.process.pid, os.WNOHANG | os.WUNTRACED)
                if pid:
                    if os.WIFEXITED(status) or os.WIFSIGNALED(status):
                        self.fault_child.process.returncode = os.waitstatus_to_exitcode(status)
                    require(os.WIFSTOPPED(status) and os.WSTOPSIG(status) == signal.SIGSTOP, 'worker did not stop')
                    self.fault['stopped_observed_ns'] = time.monotonic_ns()
                require(now-self.fault['signal_return_ns'] < 2000000000 or 'stopped_observed_ns' in self.fault,
                        'stop confirmation deadline exceeded')
            due = self.fault['signal_return_ns'] + self.profile['scenarios']['heartbeat_expiry']['pause_duration_ms']*1000000
            if 'resume_return_ns' not in self.fault:
                require(self.fault_child.process.poll() is None, 'paused worker exited before resume')
                if now >= due:
                    self.fault['resume_before_ns'] = time.monotonic_ns()
                    self.fault_child.process.send_signal(signal.SIGCONT)
                    self.fault['resume_return_ns'] = time.monotonic_ns()
                    self.fault_child.signals.append(signal.SIGCONT)
                    self.event('old_worker_resumed', **self.fault)
                    require(all(k in self.fault for k in ('timeout','loss','death','closed','stopped_observed_ns')),
                            'INSUFFICIENT_COVERAGE: heartbeat expiry not observed before resume')
        if 'loss' in self.fault and self.replacement is None:
            self.fault['replacement_launch_before_ns'] = time.monotonic_ns()
            del self.workers[self.target['slot']]
            self.replacement = self.spawn_worker(self.target['slot'], 1)
            self.fault['replacement_launch_return_ns'] = time.monotonic_ns()
        if self.replacement and 'replacement_registered_ns' not in self.fault:
            require(now-self.fault['replacement_launch_before_ns'] < 10000000000, 'replacement registration deadline')
            if self.read_registration(self.target['slot'], self.replacement, 1):
                self.fault.update(replacement_registered_ns=time.monotonic_ns(),
                    replacement_worker_id=self.workers[self.target['slot']], replacement_pid=self.replacement.process.pid)
                self.event('replacement_ready', **self.fault)

    def read_cli(self, child, started_ns):
        require(time.monotonic_ns()-started_ns < 8000000000, f'{child.role} deadline exceeded')
        if self.poll(child) is None:
            return None
        require(child.group_retired, 'CLI left descendants')
        output, error = child.stdout.read_text(), child.stderr.read_text()
        require(not SANITIZER.search(output+error), 'sanitizer diagnostic in CLI')
        require(child.process.returncode == 0 and not error, f'{child.role} failed; see {child.stderr.name}')
        return output

    def inspect_jobs(self):
        now = time.monotonic_ns()
        if self.t_done is not None:
            return
        if self.query:
            output = self.read_cli(self.query, self.query_started)
            if output is None:
                return
            rows = table(output, 'jobs')
            end = time.monotonic_ns()
            done = self.t_ack is not None and set(rows) == {e['job_id'] for e in self.ledger} and all(
                row['STATE'] == 'DONE' for row in rows.values())
            if done: self.t_done = end
            self.inspections.append(dict(start_ns=self.query_started, end_ns=end, during_admission=self.t_ack is None))
            self.event('inspection', **self.inspections[-1], all_done=done)
            if self.t_ack is not None and (not self.query_during_admission or
                                          set(rows) == {e['job_id'] for e in self.ledger}):
                self.observe_jobs(rows)
                if self.last_rows is None:
                    self.drain_eligible = {j for j,r in rows.items() if r['STATE'] != 'DONE'}
                self.last_rows = rows
            self.next_query = max(self.query_started+self.profile['poll_interval_ms']*1000000, end)
            self.query = None
        if self.t_done is None and self.t0 is not None and (self.next_query is None or now >= self.next_query):
            self.query_started = time.monotonic_ns()
            self.query_during_admission = self.t_ack is None
            self.query = self.spawn('cli-jobs', [self.binaries['faultline'], 'jobs', '--coordinator', self.endpoint])

    def submit(self):
        deadline = min(self.work_deadline, time.monotonic()+60)
        for index in range(self.args.jobs):
            entry = dict(index=index, task='sleep', arguments=self.profile['arguments'], max_retries=self.args.max_retries, job_id=None)
            self.ledger.append(entry)
            self.save('submissions.json', self.ledger)
            self.event('submission_attempt', **entry)
            try:
                before = time.monotonic_ns()
                if self.t0 is None: self.t0 = before
                child = self.spawn('cli-submit', [self.binaries['faultline'], 'submit', 'sleep', '--args',
                    entry['arguments'], '--max-retries', str(self.args.max_retries), '--coordinator', self.endpoint])
                while True:
                    self.check(deadline)
                    output = self.read_cli(child, before)
                    if output is not None: break
                    self.inspect_jobs()
                    self.pause(deadline, .01)
                job_id = unsigned(pairs(output, ['job_id'])['job_id'], 'acknowledged ID')
                require(job_id > 0 and all(e['job_id'] != job_id for e in self.ledger), 'duplicate/zero ACK')
            except (RunFailure, OSError, UnicodeError) as error:
                raise RunFailure(f'admission uncertain at input {index}; not resubmitted: {error}') from error
            entry.update(job_id=job_id, request_ns=before, ack_observed_ns=time.monotonic_ns())
            if index == self.args.jobs-1: self.t_ack = entry['ack_observed_ns']
            self.event('acknowledged', **entry)
            self.save('submissions.json', self.ledger)
            self.inspect_jobs()

    def observe_jobs(self, rows):
        require(all(r['STATE'] != 'FAILED' for r in rows.values()), 'terminal failure invalidates controlled sample')
        ChaosRun.observe_jobs(self, rows)
        for job_id, row in rows.items():
            retries = int(row['RETRIES'].split('/')[0])
            require(retries == 0 or (self.target and job_id == self.target['job_id'] and
                    self.scenario != 'no_fault' and 'loss' in self.fault), 'unattributed retry')

    def verify_workers(self, rows, idle=False):
        current = set(self.workers.values())
        require(current <= set(rows) <= self.all_worker_ids, 'unexpected worker registry identity')
        extra = set(rows)-current
        require(not extra or (self.target and extra == {self.target['worker_id']} and
                              all(rows[w]['LIVENESS']=='DEAD' for w in extra)), 'unexpected retained dead worker')
        for wid in current:
            row = rows[wid]
            require(row['LIVENESS'] == 'ALIVE' and int(row['HEARTBEAT_AGE_MS']) < 6000, 'worker not alive')
            require((row['JOB_ID']=='none' and row['ATTEMPT']=='0') or
                    (not idle and 1 <= int(row['ATTEMPT']) <= 2), 'unexpected worker lease')
        if idle:
            require(len(current)==self.args.workers, 'replacement pool incomplete')
        self.retained_workers = len(rows)

    def expected_stats(self):
        faults = int(self.scenario != 'no_fault')
        return {**super().expected_stats(), 'workers_retained':self.retained_workers,
                'workers_dead':self.retained_workers-self.args.workers, 'job_attempts_total':self.args.jobs+faults,
                'job_retries_total':faults, 'session_job_retries':faults}

    def drain_and_verify(self):
        while self.t_done is None:
            self.check()
            self.inspect_jobs()
            if self.t_done is None: self.pause(self.work_deadline, .01)
        # A small regression workload can finish before the fixed pause ends.
        # Keep t_done frozen, then finish resume/old-process validation outside
        # primary timing; never shorten the requested pause to obtain PASS.
        self.check()
        while self.scenario != 'no_fault' and self.fault_child and self.poll(self.fault_child) is None:
            self.pause(self.work_deadline, .01)
        require('checkpoint_ns' in self.fault, 'workload ended before scheduled intervention')
        self.verify_coverage()
        self.verify_terminal(self.last_rows)

    def verify_coverage(self):
        if self.scenario == 'no_fault': return
        require(all(k in self.fault for k in ('loss','death','closed','reassigned','restarted','completed',
                                            'replacement_registered_ns')), 'incomplete controlled recovery coverage')
        require(self.expected_runtime_exit(self.fault_child) and self.fault_child.group_retired,
                'old worker has not exited as expected')
        if self.scenario == 'heartbeat_expiry':
            require(all(k in self.fault for k in ('stopped_observed_ns','timeout','resume_return_ns')),
                    'missing heartbeat expiry evidence')
        elif self.transport_warnings:
            # Reuse the chaos verifier's strict reason/time correlation.
            # Coverage is checked both before terminal verification and after
            # history verification. Do not consume the same diagnostic twice.
            if not any(w['matched'] for w in self.transport_warnings):
                ChaosRun.account_transport_warnings(self, dict(closed=self.fault['closed'],
                    fd=self.target['fd'], worker_id=self.target['worker_id']))
            require(all(w['matched'] for w in self.transport_warnings),
                    'unattributed crash transport diagnostic')

    def verify_job_history(self):
        self.check()
        require(self.target is not None, 'missing common target observation')
        latency, history = recovery_history(self.transitions, self.require_job_ids(self.transitions, 'coordinator log'),
            self.all_worker_ids, self.target, self.scenario, self.expected_result, self.args.max_retries)
        self.save('latencies.json', latency)
        require(self.t0 <= self.t_ack <= self.t_done, 'invalid batch boundaries')
        elapsed = (self.t_done-self.t0)/1e6
        faults = int(self.scenario != 'no_fault')
        counts = self.snapshots['stats']
        self.metrics = dict(batch_elapsed_ms=elapsed, admission_ms=(self.t_ack-self.t0)/1e6,
            completed_jobs_per_second=self.args.jobs*1000/elapsed,
            mean_accepted_latency_ms=latency['mean_accepted_latency_ms'], p95_accepted_latency_ms=latency['p95_accepted_latency_ms'],
            actual_faults=self.coverage()['injected_faults'], attempts=counts['job_attempts_total'],
            retries=counts['job_retries_total'], terminal_failures=counts['jobs_failed_total'],
            intervention_lateness_ms=(self.fault['checkpoint_ns']-self.fault['scheduled_ns'])/1e6,
            observed_detection_ms=None, observed_reassignment_ms=None, observed_recovery_to_completion_ms=None,
            coordinator_loss_to_assignment_ms=None, coordinator_loss_to_completion_ms=None)
        if faults:
            self.metrics.update(recovery_delays(self.fault))
        self.verify_coverage()
        return history

    def coverage(self):
        return dict(injected_faults=int('signal_return_ns' in self.fault),
                    recovery_demonstrated='completed' in self.fault, scenario=self.scenario)

    def summary_details(self):
        self.save('fault.json', self.fault)
        return {**super().summary_details(), 'scenario':self.scenario,
                'reportable_recovery_campaign':False, 'fault':self.fault}
