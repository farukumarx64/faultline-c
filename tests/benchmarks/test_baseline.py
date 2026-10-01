"""Benchmark timing/accounting regression checks; timings here are not samples."""

import argparse
from contextlib import contextmanager
import copy
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'benchmarks'))
import run_baseline as benchmark

BIN_DIR = ROOT/'build/debug'


class MetricTests(unittest.TestCase):
    def history(self):
        events = []
        for i, (event, state, outcome) in enumerate([
                ('job_submitted', 'QUEUED', 'ACCEPTED'), ('job_assigned', 'ASSIGNED', 'ASSIGNED'),
                ('job_started', 'RUNNING', 'RUNNING'), ('job_completed', 'DONE', 'COMPLETED')]):
            events.append(dict(event=event, state=state, outcome=outcome, attempt='0' if i == 0 else '1',
                        worker_id='0' if i == 0 else '1', task='prime_count', failure='NONE',
                        retry_count='0', max_retries='0', durable='1', previous_worker_id='0',
                        monotonic_ms=str([100, 105, 107, 150][i]), wal_sequence=str(i+1),
                        result_bytes='2' if i == 3 else '0', result='25' if i == 3 else ''))
        return {1:events}

    def test_latency_is_from_one_coordinator_clock(self):
        result = benchmark.latency_report(self.history(), {1}, {1}, 'prime_count', '25', 0)
        self.assertEqual(result['mean_accepted_latency_ms'], 50)
        self.assertEqual(result['p95_accepted_latency_ms'], 50)

    def test_rejects_missing_duplicate_ids_and_transitions(self):
        for mutation in ('missing_id', 'extra_id', 'missing_event', 'duplicate_event'):
            events = self.history()
            if mutation == 'missing_id':
                events = {}
            elif mutation == 'extra_id':
                events[2] = copy.deepcopy(events[1])
            elif mutation == 'missing_event':
                events[1].pop()
            else:
                events[1].append(events[1][-1])
            with self.subTest(mutation=mutation), self.assertRaises(benchmark.RunFailure):
                benchmark.latency_report(events, {1}, {1}, 'prime_count', '25', 0)

    def test_rejects_bad_clock_order_owner_result_and_retry(self):
        for key, value in [('monotonic_ms','-1'), ('monotonic_ms','99'), ('wal_sequence','2'),
                           ('worker_id','2'), ('result','29'), ('result_bytes','1'),
                           ('retry_count','1'), ('durable','0')]:
            events = self.history()
            events[1][-1][key] = value
            with self.subTest(key=key, value=value), self.assertRaises(benchmark.RunFailure):
                benchmark.latency_report(events, {1}, {1}, 'prime_count', '25', 0)

    def test_nearest_rank_p95_and_mean(self):
        history = {}
        for i in range(1, 21):
            events = self.history()[1]
            events[-1]['monotonic_ms'] = str(150+i)
            history[i] = events
        result = benchmark.latency_report(history, set(history), {1}, 'prime_count', '25', 0)
        self.assertEqual(result['mean_accepted_latency_ms'], 60.5)
        self.assertEqual(result['p95_accepted_latency_ms'], 69)

    def test_cannot_aggregate_missing_or_failed_samples(self):
        for samples in ([], [dict(timing_valid=False, exit_code=1)]*5):
            with self.assertRaises(benchmark.RunFailure):
                benchmark.aggregate(samples)
        good = [dict(timing_valid=True, exit_code=0, metrics={
                    name:value for name in ('batch_elapsed_ms', 'completed_jobs_per_second',
                                            'mean_accepted_latency_ms', 'p95_accepted_latency_ms')})
                for value in (8, 2, 5, 9, 1)]
        result = benchmark.aggregate(good)['batch_elapsed_ms']
        self.assertEqual(result, dict(values=[8,2,5,9,1], median=5, minimum=1, maximum=9))

    def test_power_checks_fail_closed_but_unknown_thermal_is_explicit(self):
        ac = "Now drawing from 'AC Power'\n"
        settings = 'Battery Power:\n lowpowermode 0\nAC Power:\n lowpowermode 0\n'
        unknown = 'Note: No thermal warning level has been recorded\n'
        self.assertFalse(benchmark.power_state(ac, settings, unknown)['thermal_telemetry_available'])
        self.assertTrue(benchmark.power_state(ac, settings,
                        'Performance_Warning = 0\nCPU_Speed_Limit = 100')['thermal_telemetry_available'])
        for power, mode, thermal in [
                ("Now drawing from 'Battery Power'\n", settings, unknown),
                (ac, '', unknown), (ac, settings.replace('lowpowermode 0', 'lowpowermode 1'), unknown),
                (ac, settings, 'Thermal_Level = 1'), (ac, settings, 'CPU_Speed_Limit = 80')]:
            with self.subTest(power=power, mode=mode, thermal=thermal), self.assertRaises(benchmark.RunFailure):
                benchmark.power_state(power, mode, thermal)

    def test_committed_profile_is_supported(self):
        self.assertEqual(benchmark.load_profile()['jobs'], 64)


class ScalingMetricTests(unittest.TestCase):
    def samples(self):
        profile = benchmark.load_profile()
        samples = []
        for entry in benchmark.sample_plan(profile, True):
            if entry['stage'] == 'warmup':
                elapsed = 20000
            elif entry['workers'] == 1:
                elapsed = [100000, 10000, 20000, 30000, 40000][entry['measured_round']-1]
            else:
                elapsed = [25000, 5000, 5000, 5000, 5000][entry['measured_round']-1]
            samples.append(dict(**entry, exit_code=0, timing_valid=True,
                accounting=dict(counts=dict(submitted=64, completed=64, terminally_failed=0)),
                metrics=dict(batch_elapsed_ms=elapsed, completed_jobs_per_second=64000/elapsed,
                             mean_accepted_latency_ms=elapsed/2, p95_accepted_latency_ms=elapsed*.95)))
        return profile, samples

    def test_exact_warmup_and_rotated_round_plan(self):
        profile = benchmark.load_profile()
        plan = benchmark.sample_plan(profile, True)
        self.assertEqual(len(plan), 24)
        self.assertEqual([e['workers'] for e in plan[:4]], [1,2,4,8])
        self.assertTrue(all(e['stage']=='warmup' and e['measured_round'] is None for e in plan[:4]))
        for number, expected in enumerate([[1,2,4,8],[2,4,8,1],[4,8,1,2],[8,1,2,4],[1,4,2,8]],1):
            self.assertEqual([e['workers'] for e in plan if e['measured_round']==number], expected)
        self.assertEqual([e['campaign_index'] for e in plan], list(range(24)))

    def test_speedup_is_ratio_of_medians_not_median_of_ratios(self):
        profile, samples = self.samples()
        result = benchmark.scaling_aggregate(samples, profile)
        self.assertEqual(result['1']['batch_elapsed_ms']['median'], 30000)
        self.assertEqual(result['2']['speedup'], 6)
        self.assertEqual(result['2']['worker_normalized_efficiency_percent'], 300)
        self.assertEqual(result['1']['speedup'], 1)
        self.assertEqual(result['2']['batch_elapsed_ms']['values'], [25000,5000,5000,5000,5000])
        self.assertEqual(result['2']['batch_elapsed_ms']['minimum'], 5000)
        self.assertEqual(result['2']['batch_elapsed_ms']['maximum'], 25000)

    def test_rejects_missing_duplicate_reordered_and_invalid_samples(self):
        for mutation in ('missing', 'duplicate', 'reordered', 'failed', 'invalid', 'accounting',
                         'warmup', 'nan', 'zero', 'negative', 'wrong_round', 'wrong_workers'):
            profile, samples = self.samples()
            if mutation == 'missing': samples.pop()
            elif mutation == 'duplicate': samples[-1] = copy.deepcopy(samples[-2])
            elif mutation == 'reordered': samples[5], samples[6] = samples[6], samples[5]
            elif mutation == 'failed': samples[5]['exit_code'] = 1
            elif mutation == 'invalid': samples[5]['timing_valid'] = False
            elif mutation == 'accounting': samples[5]['accounting']['counts']['completed'] = 63
            elif mutation == 'warmup': samples[0]['metrics']['batch_elapsed_ms'] = 9999
            elif mutation == 'nan': samples[5]['metrics']['mean_accepted_latency_ms'] = float('nan')
            elif mutation == 'zero': samples[5]['metrics']['batch_elapsed_ms'] = 0
            elif mutation == 'negative': samples[5]['metrics']['p95_accepted_latency_ms'] = -1
            elif mutation == 'wrong_round': samples[5]['measured_round'] = 2
            else: samples[5]['workers'] = 8
            with self.subTest(mutation=mutation), self.assertRaises(benchmark.RunFailure):
                benchmark.scaling_aggregate(samples, profile)

    def test_multiple_workers_each_keep_their_own_lease(self):
        histories = MetricTests().history()
        histories[2] = copy.deepcopy(histories[1])
        for event in histories[2][1:]: event['worker_id'] = '2'
        self.assertEqual(len(benchmark.latency_report(histories, {1,2}, {1,2}, 'prime_count','25',0)['jobs']), 2)
        for mutation in ('unknown_owner', 'changed_owner', 'unowned_completion'):
            altered = copy.deepcopy(histories)
            if mutation == 'unknown_owner':
                for event in altered[2][1:]: event['worker_id'] = '3'
            elif mutation == 'changed_owner': altered[2][-1]['worker_id'] = '1'
            else: altered[2][-1]['worker_id'] = '3'
            with self.subTest(mutation=mutation), self.assertRaises(benchmark.RunFailure):
                benchmark.latency_report(altered, {1,2}, {1,2}, 'prime_count', '25', 0)

    def test_dirty_source_is_rejected(self):
        benchmark.require_clean_source('')
        for status in (' M benchmarks/run_baseline.py\n', '?? new.py\n', 'A  tracked.py\n'):
            with self.subTest(status=status), self.assertRaises(benchmark.RunFailure):
                benchmark.require_clean_source(status)

    def test_machine_comparison_includes_platform_but_not_load(self):
        before=dict(hardware={'model':'M4'},os='macOS',kernel={'release':'27'},storage={'internal':True},
                    python='3.9',load_average=[1,2,3],available_bytes=1000)
        after=copy.deepcopy(before);after['load_average']=[3,2,1];after['available_bytes']=999
        self.assertEqual(benchmark.machine_identity(before), benchmark.machine_identity(after))
        for key in ('hardware','os','kernel','storage','python'):
            changed=copy.deepcopy(before);changed[key]='changed'
            self.assertNotEqual(benchmark.machine_identity(before), benchmark.machine_identity(changed))


class ProcessTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT/'build/benchmarks'
        parent.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix='regression-', dir=parent))

    def test_absolute_output_build_and_execution(self):
        compiler = shutil.which('clang')
        if not compiler:
            self.skipTest('Clang needed for optimized build regression')
        directory = self.directory.resolve()
        args = argparse.Namespace(bin_dir=directory/'absolute-bin', deadline_ms=60000)
        owner = benchmark.SeriesOwner(args, directory)
        try:
            # Exercise Make's "./BUILD_DIR" recipes as well as its compile rules.
            command = benchmark.make_command(args.bin_dir, compiler)
            owner.long_command('build', command+['test-logs'], 30)
            self.assertTrue((args.bin_dir/'tests/test_log').is_file())
        finally:
            cleanup = owner.cleanup()
            owner.events.close()
            self.assertTrue(cleanup['ok'], cleanup)

    def cli_wrapper(self, mutation):
        directory = self.directory/'bin'
        directory.mkdir()
        for name in benchmark.PROGRAMS:
            path = directory/name
            if name != 'faultline':
                path.symlink_to((BIN_DIR/name).resolve())
            else:
                path.write_text('#!'+sys.executable+'\nimport subprocess,sys\n'
                    f'r=subprocess.run([{str((BIN_DIR/name).resolve())!r}, *sys.argv[1:]], capture_output=True, text=True)\n'
                    'output=r.stdout\n'+mutation+'\n'
                    'sys.stdout.write(output)\nsys.stderr.write(r.stderr)\nsys.exit(r.returncode)\n')
                path.chmod(0o755)
        return directory

    @contextmanager
    def launch(self, *, binary_dir=None, slow=False, short_warmup=False, deadline_ms=30000,
               bad_timing=False, bad_cleanup=False, workers=1, jobs=3, full_prime=False):
        profile = benchmark.load_profile()
        profile.update(contract='benchmark-regression-fixture', jobs=jobs, arguments='100',
                       expected_result='25', expected_result_bytes=2, minimum_warmup_batch_ms=0)
        if full_prime:
            profile.update(arguments='10000000', expected_result='664579', expected_result_bytes=6)
        if slow:
            profile.update(task='sleep', arguments='10000', expected_result='slept_ms=10000',
                           expected_result_bytes=14)
        if short_warmup:
            profile['minimum_warmup_batch_ms'] = 600000
        output = self.directory/'run'
        output.mkdir()
        code = (
            f'import sys,time,argparse\nsys.dont_write_bytecode=True\nsys.path.insert(0,{str(ROOT/"benchmarks")!r})\n'
            'import run_baseline as b\n'
            f'p={profile!r}\na=argparse.Namespace(bin_dir=b.Path({str(binary_dir or BIN_DIR)!r}), '
            f'workers={workers},jobs={jobs},max_retries=0,deadline_ms={deadline_ms},output_dir=b.Path({str(output)!r}))\n'
            'class Fixture(b.BaselineRun):\n'
            ' def verify_job_history(self):\n'
            f'  if {bad_timing!r}: self.t_done=self.t0-1\n'
            '  return super().verify_job_history()\n'
            ' def cleanup(self):\n'
            '  result=super().cleanup()\n'
            f'  if {bad_cleanup!r}: result["ok"]=False; result["errors"]=["injected cleanup failure"]\n'
            '  return result\n'
            f'r=Fixture(a,a.output_dir,p,{"warmup" if short_warmup else "measured"!r},time.monotonic()+60,'
            'snapshot=lambda *args: {"fixture":True})\nsys.exit(r.run())\n')
        script = self.directory/'fixture.py'
        script.write_text(code)
        with (self.directory/'harness.stdout.log').open('w') as out, (self.directory/'harness.stderr.log').open('w') as err:
            process = subprocess.Popen([sys.executable, str(script)], stdout=out, stderr=err, start_new_session=True)
            try:
                yield process, output
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=20)

    def result(self, process, output, code):
        self.assertEqual(process.wait(timeout=25), code, str(self.directory))
        summary = json.loads((output/'summary.json').read_text())
        self.assertEqual(summary['exit_code'], code)
        self.assertEqual(summary['cleanup']['remaining'], [])
        self.assertTrue(all(c['reaped'] and c['group_retired'] for c in summary['children']))
        self.assertEqual(summary['timing_valid'], code == 0)
        self.assertFalse(summary['reportable_scaling_campaign'])
        events = [json.loads(line) for line in (output/'events.jsonl').read_text().splitlines()]
        self.assertEqual(events[-1]['event'], 'verdict')
        self.assertEqual(events[-1]['exit_code'], code)
        for child in summary['children']:
            with self.assertRaises(ProcessLookupError):
                os.killpg(child['pid'], 0)
        return summary

    def test_real_prime_jobs_exact_results_and_timing_boundaries(self):
        with self.launch() as (process, output):
            summary = self.result(process, output, 0)
            self.assertEqual(summary['accounting']['counts'], dict(submitted=3, completed=3, terminally_failed=0))
            timings = json.loads((output/'timings.json').read_text())
            self.assertLessEqual(timings['t0_ns'], timings['t_ack_ns'])
            self.assertLessEqual(timings['t_ack_ns'], timings['t_done_ns'])
            self.assertLess(timings['t_done_ns'], timings['verified_ns'])
            self.assertEqual(summary['metrics']['batch_elapsed_ms'], (timings['t_done_ns']-timings['t0_ns'])/1e6)
            self.assertEqual(summary['metrics']['completed_jobs_per_second'], 3000/summary['metrics']['batch_elapsed_ms'])
            snapshots = json.loads((output/'final-snapshots.json').read_text())
            self.assertTrue(all(row['TASK'] == 'prime_count' for row in snapshots['jobs'].values()))
            self.assertTrue(all(s['result'] == '"25"' for s in snapshots['statuses'].values()))
            self.assertEqual(len(json.loads((output/'latencies.json').read_text())['jobs']), 3)

    def test_wrong_same_length_result_is_rejected(self):
        binaries = self.cli_wrapper('if sys.argv[1]=="status": output=output.replace(\'result="25"\',\'result="29"\')')
        with self.launch(binary_dir=binaries) as (process, output):
            self.assertIn('wrong exact result', self.result(process, output, 1)['first_failure'])

    def test_real_two_four_eight_worker_pools(self):
        for workers in (2,4,8):
            with self.subTest(workers=workers):
                self.directory = Path(tempfile.mkdtemp(prefix='scaling-regression-', dir=ROOT/'build/benchmarks'))
                with self.launch(workers=workers, jobs=8, full_prime=True) as (process, output):
                    summary = self.result(process, output, 0)
                    self.assertEqual(summary['accounting']['counts'], dict(submitted=8,completed=8,terminally_failed=0))
                    snapshots = json.loads((output/'final-snapshots.json').read_text())
                    self.assertEqual(len(snapshots['workers']), workers)
                    owners = {row['WORKER_ID'] for row in snapshots['jobs'].values()}
                    self.assertGreater(len(owners), 1)
                    self.assertTrue(owners <= set(snapshots['workers']))
                    self.assertTrue(all(s['result']=='"664579"' for s in snapshots['statuses'].values()))

    def test_equal_total_with_missing_id_is_rejected(self):
        binaries = self.cli_wrapper('import re\nif sys.argv[1]=="jobs": output=re.sub(r"(?m)^1(\\s+prime_count\\s)", lambda m: "77"+m[1], output)')
        with self.launch(binary_dir=binaries) as (process, output):
            self.assertIn('acknowledged ID set', self.result(process, output, 1)['first_failure'])

    def test_duplicate_ack_stops_admission_without_resubmitting(self):
        binaries = self.cli_wrapper('if sys.argv[1]=="submit": output="job_id=1\\n"')
        with self.launch(binary_dir=binaries) as (process, output):
            summary = self.result(process, output, 1)
            self.assertIn('admission uncertain', summary['first_failure'])
            self.assertEqual(sum(c['role'] == 'cli-submit' for c in summary['children']), 2)

    def test_retry_is_rejected_in_no_fault_baseline(self):
        binaries = self.cli_wrapper('if sys.argv[1]=="jobs": output=output.replace(" 0/0 ", " 1/0 ")')
        with self.launch(binary_dir=binaries) as (process, output):
            self.assertIn('consumed a retry', self.result(process, output, 1)['first_failure'])

    def test_timing_corruption_fails_after_execution(self):
        with self.launch(bad_timing=True) as (process, output):
            self.assertIn('batch boundaries', self.result(process, output, 1)['first_failure'])

    def test_short_warmup_cannot_be_counted(self):
        with self.launch(short_warmup=True) as (process, output):
            self.assertIn('too short', self.result(process, output, 1)['first_failure'])

    def test_cleanup_failure_invalidates_completed_work(self):
        with self.launch(bad_cleanup=True) as (process, output):
            summary = self.result(process, output, 1)
            self.assertTrue(summary['accounting']['verified'])
            self.assertIn('cleanup', summary['first_failure'])

    def test_work_deadline_still_cleans_up(self):
        with self.launch(slow=True, deadline_ms=11000) as (process, output):
            self.assertIn('deadline', self.result(process, output, 1)['first_failure'])

    def test_sigterm_reaps_a_busy_worker(self):
        with self.launch(slow=True) as (process, output):
            until = time.monotonic()+8
            while time.monotonic() < until:
                ledger = output/'submissions.json'
                if ledger.exists() and any(e['job_id'] for e in json.loads(ledger.read_text())):
                    break
                self.assertIsNone(process.poll(), str(self.directory))
                time.sleep(.02)
            else:
                self.fail('no acknowledgment before signal')
            process.terminate()
            self.assertIn('signal', self.result(process, output, 128+signal.SIGTERM)['first_failure'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=BIN_DIR)
    args, remaining = parser.parse_known_args()
    BIN_DIR = args.bin_dir.resolve()
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
