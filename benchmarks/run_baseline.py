#!/usr/bin/env python3
"""Build and verify optimized baseline, scaling, or recovery measurements on macOS.

Default: one-worker development baseline. --scaling/--recovery: ordered campaigns.
Standard library only. All helpers and runtime children use BatchRun ownership.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import plistlib
import re
import shlex
import shutil
import signal
import statistics
import sys
import tempfile
import time

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/chaos'))
from run_batch import BatchRun, RunFailure, pairs, require, table, unsigned

PROFILE = ROOT / 'benchmarks/profiles/scaling-v1.json'
PROGRAMS = ('faultline', 'faultline-worker', 'faultline-coordinator')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_profile():
    profile = json.loads(PROFILE.read_text())
    expected = dict(contract='faultline-benchmarks-v1', profile='scaling-prime-count-v1',
                    task='prime_count', arguments='10000000', expected_result='664579',
                    expected_result_bytes=6, jobs=64, max_retries=0, faults_per_run=0,
                    worker_counts=[1, 2, 4, 8], warmup_order=[1, 2, 4, 8],
                    heartbeat_interval_ms=2000, heartbeat_timeout_ms=6000,
                    poll_interval_ms=200, cooldown_ms=30000, minimum_warmup_batch_ms=10000,
                    run_deadline_ms=600000, cleanup_reserve_ms=10000,
                    campaign_deadline_ms=7200000)
    require(all(profile.get(k) == v for k, v in expected.items()), 'unsupported scaling profile')
    require(profile['build'] == dict(cc='clang', cflags='-O2 -g -Werror', sanitize=0,
                                    cpu_tuning='compiler_default', lto=False), 'unsupported build settings')
    require(profile['measured_rounds'] == [[1, 2, 4, 8], [2, 4, 8, 1], [4, 8, 1, 2],
                                           [8, 1, 2, 4], [1, 4, 2, 8]], 'unsupported sample plan')
    return profile


def source_inventory():
    """Hash inputs, including the uncommitted driver, without spawning helpers."""
    files = {ROOT / 'Makefile', PROFILE, Path(__file__)}
    for directory in ('src', 'include', 'tests', 'benchmarks'):
        files.update(p for p in (ROOT / directory).rglob('*')
                     if p.is_file() and p.suffix in ('.c', '.h', '.py', '.json')
                     and '__pycache__' not in p.parts)
    return {str(p.relative_to(ROOT)): digest(p) for p in sorted(files)}


def power_state(battery, settings, thermal):
    """Unknown power/mode is not a permit to publish a timing."""
    require("Now drawing from 'AC Power'" in battery, 'AC power is required')
    ac = settings.split('AC Power:', 1)
    require(len(ac) == 2, 'AC power settings unavailable')
    modes = re.findall(r'^\s*lowpowermode\s+(\d+)\s*$', ac[1], re.M)
    require(modes == ['0'], 'low-power mode must be off')
    # pmset's absent telemetry is recorded as unknown, not as zero temperature.
    warnings = re.findall(r'(?:Thermal_Level|Performance_Warning|CPU_Speed_Limit|'
                          r'CPU_Scheduler_Limit)\s*=\s*(\d+)', thermal)
    for name, value in re.findall(r'(\w+)\s*=\s*(\d+)', thermal):
        if name in ('Thermal_Level', 'Performance_Warning'):
            require(int(value) == 0, 'active thermal/performance warning')
        if name in ('CPU_Speed_Limit', 'CPU_Scheduler_Limit'):
            require(int(value) == 100, 'CPU performance is limited')
    require(all('warning' not in line.lower() or 'no ' in line.lower() or
                re.fullmatch(r'\s*Performance_Warning\s*=\s*0\s*', line)
                for line in thermal.splitlines()), 'active thermal/performance warning')
    return dict(source='AC', low_power_mode=False, thermal_raw=thermal.strip(),
                thermal_telemetry_available=bool(warnings))


def machine_snapshot(owner, label):
    def command(name, argv):
        output, _ = owner.command(label + '-' + name, argv, owner.work_deadline)
        return output
    power = power_state(command('power', ['pmset', '-g', 'batt']),
                        command('settings', ['pmset', '-g', 'custom']),
                        command('thermal', ['pmset', '-g', 'therm']))
    hardware = command('hardware', ['sysctl', 'machdep.cpu.brand_string', 'hw.model',
                       'hw.physicalcpu', 'hw.logicalcpu', 'hw.memsize',
                       'hw.perflevel0.name', 'hw.perflevel0.physicalcpu',
                       'hw.perflevel1.name', 'hw.perflevel1.physicalcpu'])
    filesystem = command('filesystem', ['df', '-P', str(owner.directory)]).splitlines()
    require(len(filesystem) >= 2 and filesystem[-1].split()[0].startswith('/dev/'),
            'cannot identify the output filesystem')
    disk = plistlib.loads(command('storage', ['diskutil', 'info', '-plist',
                                            filesystem[-1].split()[0]]).encode())
    require(disk.get('SolidState') is True and disk.get('Internal') is True and
            disk.get('FilesystemType') == 'apfs', 'expected internal APFS SSD')
    free = shutil.disk_usage(owner.directory).free
    require(free >= 1024**3, 'less than 1 GiB available for evidence')
    system = os.uname()
    return dict(observed_monotonic_ns=time.monotonic_ns(), power=power,
                hardware=dict(line.split(': ', 1) for line in hardware.splitlines()),
                os=command('os', ['sw_vers']),
                kernel=dict(system=system.sysname, release=system.release,
                            version=system.version, machine=system.machine),
                storage={k: disk.get(k) for k in ('FilesystemType', 'TotalSize', 'SolidState', 'Internal')},
                available_bytes=free, load_average=list(os.getloadavg()), python=sys.version)


def latency_report(transitions, ids, worker_ids, task, expected, max_retries):
    require(set(transitions) == set(ids), 'log job IDs differ from acknowledged IDs')
    require(worker_ids and all(isinstance(wid, int) and wid > 0 for wid in worker_ids),
            'invalid owned worker IDs')
    latencies = []
    for job_id in sorted(ids):
        events = transitions[job_id]
        require(len(events) == 4, f'job {job_id} missing or duplicate log transitions')
        worker_id = unsigned(events[1]['worker_id'], 'assigned worker ID', (1 << 32)-1)
        require(worker_id in worker_ids, f'job {job_id} assigned outside the owned pool')
        sequence = [('job_submitted', 'QUEUED', 'ACCEPTED', '0', '0'),
                    ('job_assigned', 'ASSIGNED', 'ASSIGNED', '1', str(worker_id)),
                    ('job_started', 'RUNNING', 'RUNNING', '1', str(worker_id)),
                    ('job_completed', 'DONE', 'COMPLETED', '1', str(worker_id))]
        times, sequences = [], []
        for event, (name, state, outcome, attempt, wid) in zip(events, sequence):
            require(all(event[k] == v for k, v in dict(event=name, state=state, outcome=outcome,
                    attempt=attempt, worker_id=wid, task=task, failure='NONE', retry_count='0',
                    max_retries=str(max_retries), durable='1', previous_worker_id='0').items()),
                    f'invalid logged lifecycle for job {job_id}')
            times.append(unsigned(event['monotonic_ms'], 'log timestamp', (1 << 63) - 1))
            sequences.append(unsigned(event['wal_sequence'], 'WAL sequence'))
            require(event['result_bytes'] == str(len(expected) if name == 'job_completed' else 0),
                    f'wrong logged result size for job {job_id}')
        require(times == sorted(times) and all(a < b for a, b in zip(sequences, sequences[1:])),
                f'invalid log ordering for job {job_id}')
        require(events[-1].get('result') == expected, f'wrong logged result for job {job_id}')
        latencies.append(dict(job_id=job_id, submitted_ms=times[0], completed_ms=times[-1],
                              accepted_latency_ms=times[-1] - times[0]))
    values = sorted(row['accepted_latency_ms'] for row in latencies)
    return dict(jobs=latencies, mean_accepted_latency_ms=statistics.mean(values),
                p95_accepted_latency_ms=values[math.ceil(.95 * len(values)) - 1])


class BaselineRun(BatchRun):
    mode = 'benchmark_one_worker'

    def __init__(self, args, directory, profile, stage, series_deadline, snapshot=machine_snapshot):
        super().__init__(args, directory)
        self.profile, self.stage, self.series_deadline = profile, stage, series_deadline
        self.snapshot = snapshot
        self.inhibitor = None
        self.transitions = {}
        self.last_sequence = 0
        self.t0 = self.t_ack = self.t_done = self.verified_ns = None
        self.inspections = []
        self.metrics = None
        self.machine = {}
        self.measured_round = None
        self.campaign_index = None

    def spawn(self, role, argv):
        if self.started is None:
            self.started = time.monotonic()
            self.run_deadline = min(self.started + self.args.deadline_ms / 1000, self.series_deadline)
            self.work_deadline = self.run_deadline - 10
        return super().spawn(role, argv)

    def check(self, deadline=None):
        super().check(deadline)
        if self.inhibitor is not None:
            require(self.poll(self.inhibitor) is None, 'idle-sleep inhibitor exited early')

    def event(self, event, **fields):
        super().event(event, observed_monotonic_ns=time.monotonic_ns(), **fields)

    def manifest(self):
        result = super().manifest()
        result.update(contract=self.profile['contract'], profile=self.profile, stage=self.stage,
                      measured_round=self.measured_round, campaign_index=self.campaign_index,
                      reportable_scaling_campaign=False, timing_clock='time.monotonic_ns',
                      source_inputs=source_inventory())
        return result

    def start_pool(self):
        if self.snapshot is machine_snapshot:
            self.inhibitor = self.spawn('idle-inhibitor', ['caffeinate', '-i'])
        self.machine['before'] = self.snapshot(self, 'before')
        self.save('machine.json', self.machine)
        super().start_pool()

    def runtime_line(self, child, line, path):
        super().runtime_line(child, line, path)
        if child is not self.coordinator or not re.search(r' coordinator job_\w+ ', line):
            return
        tokens = shlex.split(line)
        name = tokens[3]
        require(name in ('job_submitted', 'job_assigned', 'job_started', 'job_completed'),
                'unexpected coordinator job transition')
        fields = [token.split('=', 1) for token in tokens[4:]]
        require(all(len(pair) == 2 for pair in fields), 'malformed log fields')
        event = dict(fields)
        require(len(event) == len(fields), 'duplicate log fields')
        require(event.get('pid') == str(child.process.pid), 'wrong coordinator log PID')
        job_id = unsigned(event['job_id'], 'logged job ID')
        seq = unsigned(event['wal_sequence'], 'logged WAL sequence')
        require(seq > self.last_sequence, 'duplicate/non-increasing logged WAL sequence')
        self.last_sequence = seq
        event['event'] = name
        self.transitions.setdefault(job_id, []).append(event)
        self.event('coordinator_transition', fields=event)

    @property
    def expected_result(self):
        return self.profile['expected_result']

    def cli(self, command, *arguments, deadline=None):
        if command == 'submit' and self.t0 is None:
            self.t0 = time.monotonic_ns()
        return super().cli(command, *arguments, deadline=deadline)

    def submit(self):
        deadline = min(self.work_deadline, time.monotonic() + 60)
        for index in range(self.args.jobs):
            entry = dict(index=index, task=self.profile['task'], arguments=self.profile['arguments'],
                         max_retries=0, job_id=None)
            self.ledger.append(entry)
            self.save('submissions.json', self.ledger)
            self.event('submission_attempt', **entry)
            try:
                output = self.cli('submit', entry['task'], '--args', entry['arguments'],
                                  '--max-retries', '0', deadline=deadline)
                job_id = unsigned(pairs(output, ['job_id'])['job_id'], 'acknowledged ID')
                require(job_id > 0 and all(e['job_id'] != job_id for e in self.ledger), 'duplicate/zero ACK')
            except (RunFailure, OSError, UnicodeError) as error:
                raise RunFailure(f'admission uncertain at input {index}; not resubmitted: {error}') from error
            entry['job_id'] = job_id
            entry['ack_observed_ns'] = time.monotonic_ns()
            if index == self.args.jobs - 1:
                self.t_ack = entry['ack_observed_ns']
            self.event('acknowledged', **entry)
            self.save('submissions.json', self.ledger)

    @property
    def task_name(self):
        return self.profile['task']

    def verify_job_history(self):
        self.check()
        ids = self.require_job_ids(self.transitions, 'coordinator log')
        latency = latency_report(self.transitions, ids, set(self.workers.values()), self.profile['task'],
                                 self.expected_result, 0)
        self.save('latencies.json', latency)
        require(self.t0 is not None and self.t0 <= self.t_ack <= self.t_done, 'invalid batch boundaries')
        elapsed = (self.t_done - self.t0) / 1e6
        require(elapsed > 0, 'nonpositive batch duration')
        self.metrics = dict(batch_elapsed_ms=elapsed, admission_ms=(self.t_ack-self.t0)/1e6,
                            completed_jobs_per_second=self.args.jobs * 1000 / elapsed,
                            mean_accepted_latency_ms=latency['mean_accepted_latency_ms'],
                            p95_accepted_latency_ms=latency['p95_accepted_latency_ms'])
        if self.stage == 'warmup':
            require(elapsed >= self.profile['minimum_warmup_batch_ms'], 'warmup workload is too short')
        return {job_id: [] for job_id in ids}

    def drain_and_verify(self):
        while True:
            query_start = time.monotonic_ns()
            rows = table(self.cli('jobs'), 'jobs')
            query_end = time.monotonic_ns()
            self.require_job_ids(rows, 'job listing')
            done = all(row['STATE'] == 'DONE' for row in rows.values())
            if done:
                self.t_done = query_end
            self.inspections.append(dict(start_ns=query_start, end_ns=query_end))
            self.event('inspection', start_ns=query_start, end_ns=query_end, all_done=done)
            self.observe_jobs(rows)
            if len(self.inspections) == 1:
                self.drain_eligible = {i for i, row in rows.items() if row['STATE'] != 'DONE'}
            self.verify_workers(table(self.cli('workers'), 'workers'), idle=done)
            if done:
                break
            next_query = max(query_start / 1e9 + .2, time.monotonic())
            while time.monotonic() < next_query:
                self.pause(self.work_deadline, max(0, min(.05, next_query-time.monotonic())))
        self.verify_terminal(rows)

    def verify_terminal(self, rows):
        """Freeze timing first; then audit stable results, histories and the pool."""
        previous_deadline = self.work_deadline
        self.work_deadline = min(previous_deadline, time.monotonic() + 60)
        try:
            statuses = {job_id: self.status(job_id, row) for job_id, row in rows.items()}
            final_rows = table(self.cli('jobs'), 'jobs')
            self.observe_jobs(final_rows)
            for job_id, row in final_rows.items():
                require(self.status(job_id, row) == statuses[job_id], 'terminal status changed')
            workers = table(self.cli('workers'), 'workers')
            self.verify_workers(workers, idle=True)
            stats = self.verify_stats(self.cli('stats'))
            self.snapshots = dict(jobs=final_rows, statuses=statuses, workers=workers, stats=stats)
            self.save('final-snapshots.json', self.snapshots)
            self.verify_accounting(final_rows, statuses)
            self.machine['after'] = self.snapshot(self, 'after')
            self.save('machine.json', self.machine)
            self.verified_ns = time.monotonic_ns()
        finally:
            self.work_deadline = previous_deadline

    def summary_details(self):
        timings = dict(t0_ns=self.t0, t_ack_ns=self.t_ack, t_done_ns=self.t_done,
                       verified_ns=self.verified_ns, inspections=self.inspections)
        self.save('timings.json', timings)
        overhead = dict(startup_ms=(self.t0 / 1e9-self.started)*1000 if self.t0 else None,
                        verification_ms=(self.verified_ns-self.t_done)/1e6 if self.verified_ns else None)
        return dict(profile=self.profile['profile'], stage=self.stage, metrics=self.metrics,
                    workers=self.args.workers, measured_round=self.measured_round,
                    campaign_index=self.campaign_index,
                    overhead=overhead, reportable_scaling_campaign=False)

    def save(self, name, value):
        if name == 'summary.json':
            value['contract'] = self.profile['contract']
            value['timing_valid'] = value['exit_code'] == 0 and self.metrics is not None
            value['overhead']['cleanup_ms'] = value['cleanup']['elapsed_ms']
        super().save(name, value)


class SeriesOwner(BatchRun):
    """Own build/test/metadata commands and signal-aware cooldowns."""

    def long_command(self, role, argv, timeout):
        child = self.spawn(role, argv)
        limit = min(self.work_deadline, time.monotonic() + timeout)
        while self.poll(child) is None:
            self.pause(limit, .05)
        self.check(limit)
        require(child.group_retired and child.process.returncode == 0,
                f'{role} failed; see {child.stdout.name} and {child.stderr.name}')
        return dict(argv=child.argv, stdout=child.stdout.name, stderr=child.stderr.name,
                    exit_code=child.process.returncode)


def aggregate(samples):
    require(len(samples) == 5 and all(s['timing_valid'] and s['exit_code'] == 0 for s in samples),
            'need five valid samples, without replacing failures')
    names = ('batch_elapsed_ms', 'completed_jobs_per_second',
             'mean_accepted_latency_ms', 'p95_accepted_latency_ms')
    return {name: dict(values=[s['metrics'][name] for s in samples],
                       median=statistics.median(s['metrics'][name] for s in samples),
                       minimum=min(s['metrics'][name] for s in samples),
                       maximum=max(s['metrics'][name] for s in samples)) for name in names}


def sample_plan(profile, scaling):
    if scaling:
        entries = [('warmup', None, workers) for workers in profile['warmup_order']]
        entries += [('measured', number, workers)
                    for number, order in enumerate(profile['measured_rounds'], 1) for workers in order]
    else:
        entries = [('warmup', None, 1)] + [('measured', None, 1)] * 5
    return [dict(campaign_index=index, stage=stage, measured_round=number, workers=workers)
            for index, (stage, number, workers) in enumerate(entries)]


def scaling_aggregate(samples, profile):
    plan = sample_plan(profile, True)
    require(len(samples) == len(plan), 'incomplete scaling campaign')
    for sample, expected in zip(samples, plan):
        require(all(sample.get(key) == value for key, value in expected.items()),
                'scaling sample order/configuration mismatch')
        require(sample['exit_code'] == 0 and sample['timing_valid'], 'invalid scaling sample')
        require(sample['accounting']['counts'] == dict(submitted=profile['jobs'],
                completed=profile['jobs'], terminally_failed=0), 'incomplete scaling accounting')
        require(all(math.isfinite(value) and value >= 0 for value in sample['metrics'].values())
                and sample['metrics']['batch_elapsed_ms'] > 0, 'invalid scaling metrics')
        if expected['stage'] == 'warmup':
            require(sample['metrics']['batch_elapsed_ms'] >= profile['minimum_warmup_batch_ms'],
                    'warmup workload is too short')
    grouped = {str(workers): aggregate([s for s in samples if s['stage'] == 'measured'
                                       and s['workers'] == workers])
               for workers in profile['worker_counts']}
    baseline = grouped['1']['batch_elapsed_ms']['median']
    for workers, metrics in grouped.items():
        metrics['speedup'] = baseline / metrics['batch_elapsed_ms']['median']
        metrics['worker_normalized_efficiency_percent'] = metrics['speedup'] * 100 / int(workers)
    return grouped


def machine_identity(snapshot):
    # Load, free space, power observations and timestamps vary; platform must not.
    return {key: snapshot[key] for key in ('hardware', 'os', 'kernel', 'storage', 'python')}


def require_clean_source(status):
    require(not status.strip(), 'campaign requires a clean committed checkout; save changes before running')


def make_command(bin_dir, compiler):
    clean_env = ['env']
    for name in ('MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS',
                 'LDLIBS', 'CC', 'SANITIZE', 'ASAN_OPTIONS', 'UBSAN_OPTIONS'):
        clean_env += ['-u', name]
    # Make's executable recipes prefix BUILD_DIR with "./". Keep it relative to
    # its explicit -C directory, even when the evidence directory is absolute.
    return clean_env + ['PYTHONDONTWRITEBYTECODE=1', 'make', '-C', str(ROOT),
                f'BUILD_DIR={os.path.relpath(bin_dir, ROOT)}', f'CC={compiler}', 'SANITIZE=0',
                'CFLAGS=-O2 -g -Werror', f'PYTHON={sys.executable}']


def run_series(directory, profile, *, scaling=False, recovery=False):
    if recovery:
        import run_recovery
    args = argparse.Namespace(bin_dir=directory/'bin', deadline_ms=profile['campaign_deadline_ms'])
    owner = SeriesOwner(args, directory)
    handlers = {sig: signal.signal(sig, owner.on_signal) for sig in (signal.SIGINT, signal.SIGTERM)}
    failure, samples, runs, provenance = None, [], [], {}
    plan = run_recovery.sample_plan(profile) if recovery else sample_plan(profile, scaling)
    scope = ('complete ordered recovery campaign' if recovery else
             'complete ordered scaling campaign' if scaling else 'one-worker development baseline')
    controlled = scaling or recovery
    reference_machine = None
    try:
        # Start its absolute budget before the first owned metadata helper.
        child = owner.spawn('git-revision', ['git', '-C', ROOT, 'rev-parse', 'HEAD'])
        while owner.poll(child) is None:
            owner.pause(min(owner.work_deadline, owner.started+8), .02)
        require(child.process.returncode == 0 and child.group_retired, 'cannot identify source revision')
        status, _ = owner.command('git-status', ['git', '-C', ROOT, 'status', '--porcelain'], owner.work_deadline)
        patch_command = owner.long_command('git-diff', ['git', '-C', ROOT, 'diff', 'HEAD'], 8)
        patch = (directory/patch_command['stdout']).read_text()
        inputs = source_inventory()
        for relative in inputs:
            copy = directory/'source-inputs'/relative
            copy.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT/relative, copy)
            require(digest(copy) == inputs[relative], 'source changed during snapshot')
        provenance = dict(revision=child.stdout.read_text().strip(), git_status=status,
                          source_inputs=inputs, source_patch=patch,
                          reportable_scaling_campaign=False,
                          scope=scope)
        owner.save('provenance.json', provenance)
        if controlled:
            require_clean_source(status)
        owner.save('profile.json', profile)
        owner.save('plan.json', dict(scope=scope, runs=plan, cooldown_ms=profile['cooldown_ms'],
                   reportable_scaling_campaign=False))
        shutil.copyfile(ROOT/'docs/benchmarks.md', directory/'contract.md')
        compiler = shutil.which('clang')
        require(compiler is not None, 'clang is required')
        version, _ = owner.command('compiler-version', [compiler, '--version'], owner.work_deadline)
        make_version, _ = owner.command('make-version', ['make', '--version'], owner.work_deadline)
        # Prevent inherited build overrides from silently changing the profile.
        command = make_command(args.bin_dir, compiler)
        build = owner.long_command('optimized-build', command+['--jobs=2', 'all'], 120)
        correctness = owner.long_command('optimized-tests', command+['test'], 600)
        if recovery:
            provenance['recovery_regressions'] = owner.long_command(
                'recovery-regressions', command+['test-recovery-benchmark-harness'], 300)
            owner.save('provenance.json', provenance)
        owner.save('build.json', dict(compiler=compiler, compiler_version=version, make_version=make_version,
                    build=build, correctness=correctness, cflags=profile['build']['cflags'], sanitize=0,
                    binaries={name:digest(args.bin_dir/name) for name in PROGRAMS}))
        require(source_inventory() == inputs, 'source changed during build/correctness checks')
        for entry in plan:
            index, stage, workers = entry['campaign_index'], entry['stage'], entry['workers']
            owner.check()
            if index:
                until = min(owner.work_deadline, time.monotonic()+profile['cooldown_ms']/1000)
                while time.monotonic() < until:
                    owner.check()
                    time.sleep(max(0, min(.2, until-time.monotonic())))
                owner.check()
            require(owner.work_deadline-time.monotonic() > 10, 'insufficient remaining series budget')
            suffix = f'-r{entry["measured_round"]}' if entry['measured_round'] is not None else ''
            name = (f'{index:02d}-{stage}{suffix}-{entry["scenario"]}' if recovery else
                    f'{index:02d}-{stage}{suffix}-w{workers}' if scaling else f'{index:02d}-{stage}')
            path = directory / name
            path.mkdir()
            run_args = argparse.Namespace(bin_dir=args.bin_dir, workers=workers, jobs=profile['jobs'],
                         max_retries=profile['max_retries'], deadline_ms=profile['run_deadline_ms'], output_dir=path)
            print(f'Starting {path.name}: {profile["jobs"]} {profile["task"]} jobs, {workers} worker(s)', flush=True)
            run = (run_recovery.RecoveryRun(run_args, path, profile, stage, owner.run_deadline,
                                          scenario=entry['scenario']) if recovery else
                   BaselineRun(run_args, path, profile, stage, owner.run_deadline))
            run.measured_round, run.campaign_index = entry['measured_round'], index
            if scaling:
                run.mode = 'benchmark_scaling_sample'
            code = run.run()
            result = json.loads((path/'summary.json').read_text())
            runs.append(dict(path=path.name, exit_code=code, metrics=result['metrics'], **entry))
            if code in (128+signal.SIGINT, 128+signal.SIGTERM):
                owner.stop_signal = code-128
            require(code == 0 and result['timing_valid'], f'{path.name} failed; no replacement sample')
            require(source_inventory() == inputs, 'source changed during baseline series')
            require({name:digest(args.bin_dir/name) for name in PROGRAMS} ==
                    json.loads((directory/'build.json').read_text())['binaries'], 'binaries changed')
            if controlled:
                for moment in ('before', 'after'):
                    identity = machine_identity(run.machine[moment])
                    if reference_machine is None:
                        reference_machine = identity
                    require(identity == reference_machine, 'machine configuration changed during campaign')
                current, _ = owner.command('source-status', ['git', '-C', ROOT, 'status', '--porcelain'], owner.work_deadline)
                require_clean_source(current)
                revision, _ = owner.command('source-revision', ['git', '-C', ROOT, 'rev-parse', 'HEAD'], owner.work_deadline)
                require(revision.strip() == provenance['revision'], 'source revision changed during campaign')
            if controlled or stage == 'measured':
                samples.append(result)
        # Validate aggregates inside the guarded body so errors retain a FAIL summary.
        metrics = (run_recovery.aggregate(samples, profile) if recovery else
                   scaling_aggregate(samples, profile) if scaling else aggregate(samples))
    except Exception as error:
        failure = f'{type(error).__name__}: {error}'
    finally:
        cleanup = owner.cleanup()
        failure = failure or ('series cleanup failed' if not cleanup['ok'] else None)
        if owner.stop_signal:
            failure = failure or f'interrupted by signal {owner.stop_signal}'
        code = 128+owner.stop_signal if owner.stop_signal else (1 if failure else 0)
        result = dict(contract=profile['contract'], profile=profile['profile'],
                      verdict=('RECOVERY_PASS' if recovery else 'SCALING_PASS' if scaling else
                               'ONE_WORKER_BASELINE_PASS') if code == 0 else 'FAIL',
                      exit_code=code, first_failure=failure, reportable_scaling_campaign=scaling and code == 0,
                      reportable_recovery_campaign=recovery and code == 0,
                      scope=scope, runs=runs, measured_samples=sum(s['stage'] == 'measured' for s in samples),
                      aggregate=metrics if code == 0 else None, cleanup=cleanup,
                      elapsed_ms=owner.elapsed(),
                      children=[dict(role=c.role, pid=c.process.pid, returncode=c.process.returncode,
                                     reaped=c.exit_ms is not None, group_retired=c.group_retired)
                                for c in owner.children])
        try:
            owner.event('verdict', verdict=result['verdict'], exit_code=code, first_failure=failure)
            owner.save('summary.json', result)
        finally:
            owner.events.close()
            for sig, handler in handlers.items():
                signal.signal(sig, handler)
    print(f'{result["verdict"]}: {directory}', flush=True)
    if failure:
        print(failure, file=sys.stderr)
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path, help='fresh directory; default under build/benchmarks')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--scaling', action='store_true', help='run the full ordered 1/2/4/8-worker campaign')
    mode.add_argument('--recovery', action='store_true', help='run matched control/crash/heartbeat recovery rounds')
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('automatic machine/power validation currently supports macOS only')
    try:
        if args.recovery:
            from run_recovery import load_profile as load_recovery_profile
            profile = load_recovery_profile()
        else:
            profile = load_profile()
        if args.output_dir:
            directory = args.output_dir.absolute()
            directory.mkdir(parents=True, exist_ok=False)
        else:
            parent = ROOT/'build/benchmarks'
            parent.mkdir(parents=True, exist_ok=True)
            directory = Path(tempfile.mkdtemp(prefix='recovery-' if args.recovery else
                                             'scaling-' if args.scaling else 'one-worker-', dir=parent))
        print(f'Benchmark artifacts: {directory}', flush=True)
        # Tee the driver's own output to retained files without launching helpers.
        class Tee:
            def __init__(self, terminal, file):
                self.terminal, self.file = terminal, file
            def write(self, text):
                self.terminal.write(text)
                return self.file.write(text)
            def flush(self):
                self.terminal.flush()
                self.file.flush()
        with (directory/'harness.stdout.log').open('x') as out, (directory/'harness.stderr.log').open('x') as err:
            stdout, stderr = sys.stdout, sys.stderr
            try:
                sys.stdout, sys.stderr = Tee(stdout, out), Tee(stderr, err)
                return run_series(directory, profile, scaling=args.scaling, recovery=args.recovery)
            finally:
                sys.stdout, sys.stderr = stdout, stderr
    except (OSError, ValueError, RunFailure) as error:
        print(f'benchmark error: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
