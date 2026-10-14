"""Power conditions and report audits. Synthetic timings are never measurements."""
import argparse
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'benchmarks'))
import run_baseline as base
import compare_power as comparison
import power
import test_baseline as harness_tests

UNKNOWN = 'Note: No thermal warning level has been recorded\n'


def battery(source='Battery', charge=95):
    return f"Now drawing from '{source} Power'\n -InternalBattery-0 (id=1)\t{charge}%; discharging; 3:00 remaining present: true\n"


def raw(name='battery', charge=95):
    mode = int(name == 'battery-low-power')
    source = 'AC' if name == 'ac' else 'Battery'
    return dict(profile=power.load_profile(name), battery_before=battery(source, charge),
                battery_after=battery(source, charge),
                settings=f'AC Power:\n lowpowermode 0\nBattery Power:\n lowpowermode {mode}\n',
                effective=f'Currently in use:\n lowpowermode {mode}\n', thermal=UNKNOWN)


class PowerTests(unittest.TestCase):
    def test_profiles_match_selected_source_and_effective_settings(self):
        for name in power.NAMES:
            observation = power.interpret_probe(raw(name), power.load_profile(name))
            self.assertEqual(observation['source'], 'AC' if name == 'ac' else 'Battery')
            self.assertEqual(observation['low_power_mode'], name == 'battery-low-power')
            self.assertEqual(observation['charge_percent'], 95)
            self.assertFalse(observation['thermal_telemetry_available'])

    def test_sections_do_not_leak_into_each_other_in_either_order(self):
        for settings in ('AC Power:\n lowpowermode 0\nBattery Power:\n lowpowermode 1\n',
                         'Battery Power:\n lowpowermode 1\nAC Power:\n lowpowermode 0\n'):
            self.assertFalse(power.low_power_mode(settings, 'AC'))
            self.assertTrue(power.low_power_mode(settings, 'Battery'))
            with self.assertRaises(base.RunFailure):
                power.state(battery(), settings, UNKNOWN, power.load_profile('battery'))

    def test_missing_ambiguous_or_changed_conditions_fail_closed(self):
        mutations = [dict(battery_before=battery('AC')), dict(battery_after=battery('UPS')),
            dict(settings='AC Power:\n lowpowermode 0\n'), dict(effective=''),
            dict(effective='Currently in use:\n lowpowermode 1\n'),
            dict(settings='Battery Power:\n lowpowermode 0\n lowpowermode 0\n'),
            dict(settings='Battery Power:\n lowpowermode 0\nBattery Power:\n lowpowermode 0\n'),
            dict(battery_after=battery()+battery()), dict(battery_after="Now drawing from 'Battery Power'\n")]
        for change in mutations:
            with self.subTest(change=change), self.assertRaises(base.RunFailure):
                power.interpret_probe({**raw(), **change}, power.load_profile('battery'))

    def test_charge_range_and_floor_are_distinct(self):
        profile = power.load_profile('battery')
        for charge in (20,89,90,100):
            observation = power.interpret_probe(raw(charge=charge), profile)
            power.validate_observation(observation, profile)
            if charge >= 90:
                power.validate_observation(observation, profile, initial=True)
            else:
                with self.assertRaises(base.RunFailure):
                    power.validate_observation(observation, profile, initial=True)
        for charge in (19,101,-95,'unknown','9.5',''):
            with self.subTest(charge=charge), self.assertRaises(base.RunFailure):
                power.interpret_probe(raw(charge=charge), profile)
        missing = raw(); missing['battery_after'] = battery().replace('present: true','present: false')
        with self.assertRaises(base.RunFailure): power.interpret_probe(missing, profile)

    def test_low_power_limit_is_recorded_but_explicit_thermal_warning_rejects(self):
        p = power.load_profile('battery-low-power')
        self.assertTrue(power.interpret_probe({**raw('battery-low-power'), 'thermal':'CPU_Speed_Limit = 60'}, p)
                        ['thermal_telemetry_available'])
        for thermal in ('Thermal_Level = 1','Performance_Warning = 1','Thermal warning active',
                        'CPU_Speed_Limit = 0','CPU_Scheduler_Limit = 101'):
            with self.subTest(thermal=thermal), self.assertRaises(base.RunFailure):
                power.interpret_probe({**raw('battery-low-power'),'thermal':thermal}, p)
        with self.assertRaises(base.RunFailure):
            power.interpret_probe({**raw(),'thermal':'CPU_Speed_Limit = 60'}, power.load_profile('battery'))

    def test_probe_retains_raw_failure_evidence_and_command_errors(self):
        class Owner:
            work_deadline = 1
            def __init__(self, values, fail=False): self.values, self.fail, self.saved = values, fail, {}
            def command(self, label, argv, deadline):
                if self.fail and argv[-1] == 'live': raise base.RunFailure('injected read failure')
                return self.values[label.split('-',1)[1]], 0
            def save(self, name, value): self.saved[name] = value
        for failed_command in (False,True):
            owner = Owner({**raw(),'battery_after':battery('AC')}, failed_command)
            with self.assertRaises(base.RunFailure): power.probe(owner,'before',power.load_profile('battery'))
            saved = owner.saved['before-power.json']
            self.assertIn('error',saved)
            self.assertIn('battery_before',saved)
            self.assertGreaterEqual(saved['finished_monotonic_ns'],saved['observed_monotonic_ns'])


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='faultline-power-fixture-')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def save(self, directory, name, value):
        (directory/name).write_text(json.dumps(value))

    def campaign(self, name, *, recovering=False):
        directory = self.directory/name
        directory.mkdir()
        profile = comparison.recovery.load_profile() if recovering else base.load_profile()
        pp = power.load_profile(name)
        plan = comparison.recovery.sample_plan(profile) if recovering else base.sample_plan(profile, True)
        cleanup = dict(ok=True,remaining=[],errors=[])
        children = [dict(reaped=True,group_retired=True)]
        machine = dict(hardware={'fixture':True},os='fixture',kernel={},storage={},python='fixture')
        source = directory/'source-inputs'; source.mkdir(); (source/'fixture.py').write_text('# test data only\n')
        provenance = dict(revision='a'*40,git_status='',source_patch='',power_profile=pp,
                          source_inputs={'fixture.py':base.digest(source/'fixture.py')})
        if recovering: provenance['recovery_regressions'] = dict(exit_code=0)
        binaries = directory/'bin'; binaries.mkdir()
        for program in base.PROGRAMS: (binaries/program).write_text('fixture, not executable')
        build = dict(compiler='fixture',compiler_version='fixture',make_version='fixture',
                     cflags=profile['build']['cflags'],sanitize=0,build={'exit_code':0},correctness={'exit_code':0},
                     binaries={n:base.digest(binaries/n) for n in base.PROGRAMS})
        samples, runs = [],[]
        for entry in plan:
            path = directory/str(entry['campaign_index']); path.mkdir()
            elapsed = 20000 if entry['stage']=='warmup' else 40000/entry['workers']
            elapsed *= dict(ac=1,battery=1.2,**{'battery-low-power':1.5})[name]
            ids=list(range(1,profile['jobs']+1))
            faults = int(recovering and entry['scenario'] != 'no_fault')
            accounting=dict(verified=True,submitted_ids=ids,completed_ids=ids,terminally_failed_ids=[],
                counts=dict(submitted=len(ids),completed=len(ids),terminally_failed=0),jobs=[dict(
                    submission=dict(index=i-1,job_id=i,**{k:profile[k] for k in ('task','arguments','max_retries')}),
                    terminal_status=dict(job_id=str(i),state='DONE',failure='NONE',result=json.dumps(profile['expected_result']),
                        result_bytes=str(profile['expected_result_bytes']),
                        retries=f'{int(i==1 and faults)}/{profile["max_retries"]}',attempt=str(1+int(i==1 and faults)))) for i in ids])
            sample=dict(**entry,exit_code=0,timing_valid=True,cleanup=cleanup,children=children,
                power_profile=pp,accounting=accounting,metrics=dict(batch_elapsed_ms=elapsed,
                completed_jobs_per_second=len(ids)*1000/elapsed,mean_accepted_latency_ms=100,p95_accepted_latency_ms=100))
            if recovering:
                sample['coverage']=dict(injected_faults=faults,recovery_demonstrated=bool(faults))
                sample['fault']=dict(signal_before_ns=100000000,signal_return_ns=101000000,
                    loss=dict(observed_ns=110000000,monotonic_ms='8000'),
                    reassigned=dict(observed_ns=120000000,monotonic_ms='8003'),
                    restarted=dict(observed_ns=125000000),completed=dict(observed_ns=150000000,monotonic_ms='8040'),
                    replacement_launch_before_ns=111000000,replacement_launch_return_ns=113000000,
                    replacement_registered_ns=119000000)
                sample['metrics'].update(actual_faults=faults,attempts=len(ids)+faults,retries=faults,terminal_failures=0)
                sample['metrics'].update({k:v if faults else None for k,v in
                                          comparison.recovery.recovery_delays(sample['fault']).items()})
            manifest=dict(profile=profile,power_profile=pp,source=dict(revision='a'*40,dirty=False))
            snapshots={}
            for moment in ('before','after'):
                observation=raw(name)
                observation['power']=power.interpret_probe(observation,pp)
                self.save(path,moment+'-power.json',observation)
                snapshots[moment]={**machine,'power':observation['power']}
            self.save(path,'machine.json',snapshots)
            self.save(path,'summary.json',sample); self.save(path,'manifest.json',manifest)
            self.save(path,'accounting.json',accounting)
            self.save(path,'timings.json',dict(t0_ns=1000000,t_ack_ns=1000001,
                t_done_ns=1000000+int(elapsed*1e6),verified_ns=1000001+int(elapsed*1e6)))
            self.save(path,'latencies.json',dict(jobs=[dict(job_id=i,submitted_ms=1,completed_ms=101,
                                                         accepted_latency_ms=100) for i in ids]))
            (path/'events.jsonl').write_text('test fixture\n'); (path/'coordinator.wal').write_bytes(b'fixture')
            samples.append(sample); runs.append(dict(path=path.name,**entry))
        summary=dict(exit_code=0,verdict='RECOVERY_PASS' if recovering else 'SCALING_PASS',
                     reportable_scaling_campaign=not recovering,reportable_recovery_campaign=recovering,power_profile=pp,
                     runs=runs,cleanup=cleanup,children=children,measured_samples=15 if recovering else 20,
                     aggregate=comparison.recovery.aggregate(samples,profile) if recovering else base.scaling_aggregate(samples,profile))
        for filename, obj in [('summary.json',summary),('profile.json',profile),('power-profile.json',pp),
                              ('provenance.json',provenance),('build.json',build),('plan.json',dict(runs=plan))]:
            self.save(directory,filename,obj)
        return directory

    def test_recovery_campaigns_keep_crash_and_expiry_metrics_separate(self):
        campaigns={name:comparison.audit_campaign(self.campaign(name,recovering=True),name) for name in power.NAMES}
        report=comparison.compare(campaigns)
        self.assertEqual({r['configuration'] for r in report['comparisons']},set(comparison.recovery.SCENARIOS))
        self.assertFalse(any(r['configuration']=='no_fault' and r['metric']=='observed_detection_ms'
                             for r in report['comparisons']))
        self.assertEqual(len([r for r in report['comparisons'] if r['metric']=='observed_detection_ms']),6)
        comparison.write_report(self.directory/'recovery-report',report)
        path=self.directory/'battery'/'1'/'summary.json'
        sample=json.loads(path.read_text());sample['coverage']['injected_faults']=0
        path.write_text(json.dumps(sample))
        with self.assertRaises(base.RunFailure): comparison.audit_campaign(self.directory/'battery','battery')

    def test_complete_separate_campaigns_generate_comparison_without_pooling(self):
        campaigns={name:comparison.audit_campaign(self.campaign(name),name) for name in power.NAMES}
        report=comparison.compare(campaigns)
        row=next(r for r in report['comparisons'] if r['configuration']=='1' and
                 r['power_profile']=='battery-low-power' and r['metric']=='batch_elapsed_ms')
        self.assertAlmostEqual(row['change_vs_ac_percent'],50)
        self.assertAlmostEqual(row['change_vs_battery_percent'],25)
        self.assertEqual(len(row['samples']),5)
        out=self.directory/'report'
        comparison.write_report(out,report)
        self.assertTrue(all((out/name).exists() for name in ('comparison.json','comparison.csv','comparison.md')))
        with self.assertRaises(FileExistsError): comparison.write_report(out,report)
        for field in ('workload','source_revision','source_inputs','build','machine','power_profile'):
            bad=copy.deepcopy(campaigns); bad['battery'][field]='changed'
            with self.subTest(field=field), self.assertRaises(base.RunFailure): comparison.compare(bad)

    def test_audit_rejects_corrupted_or_incomplete_evidence(self):
        path=self.campaign('battery')
        for marker in (path/'INVALID',path/'0'/'INVALID'):
            marker.write_text('Known interruption between observations\n')
            with self.assertRaises(base.RunFailure): comparison.audit_campaign(path,'battery')
            marker.unlink()
        mutations=[('summary.json',lambda x:x['runs'].pop()),
            ('summary.json',lambda x:x['runs'][0].update(path='../outside')),
            ('summary.json',lambda x:x['cleanup'].update(ok=False)),
            ('summary.json',lambda x:x['aggregate']['1']['batch_elapsed_ms'].update(median=1)),
            ('provenance.json',lambda x:x.update(git_status=' M changed.py')),
            ('provenance.json',lambda x:x['source_inputs'].update({'fixture.py':'0'*64})),
            ('0/summary.json',lambda x:x.update(power_profile=power.load_profile('ac'))),
            ('0/summary.json',lambda x:x['accounting']['submitted_ids'].__setitem__(0,2)),
            ('0/summary.json',lambda x:x['accounting']['jobs'][0]['terminal_status'].update(result='"bad"')),
            ('0/summary.json',lambda x:x['accounting']['jobs'][0]['terminal_status'].update(retries='1/0')),
            ('0/summary.json',lambda x:x['metrics'].update(batch_elapsed_ms=1)),
            ('0/summary.json',lambda x:x['children'][0].update(group_retired=False)),
            ('0/before-power.json',lambda x:x.update(battery_after=battery('AC'))),
            ('0/before-power.json',lambda x:x.update(battery_before=battery(charge=89),battery_after=battery(charge=89))),
            ('1/after-power.json',lambda x:x.update(battery_after=battery(charge=19))),
            ('0/latencies.json',lambda x:x['jobs'][0].update(accepted_latency_ms=1))]
        for filename, mutate in mutations:
            original=(path/filename).read_text(); obj=json.loads(original); mutate(obj)
            self.save(path,filename,obj)
            try:
                with self.subTest(filename=filename), self.assertRaises(base.RunFailure):
                    comparison.audit_campaign(path,'battery')
            finally: (path/filename).write_text(original)


class PowerProcessTests(unittest.TestCase):
    setUp = harness_tests.ProcessTests.setUp
    launch = harness_tests.ProcessTests.launch
    result = harness_tests.ProcessTests.result

    def test_separate_profile_metadata_with_real_jobs(self):
        for name in ('battery','battery-low-power'):
            with self.subTest(name=name):
                self.setUp()
                with self.launch(power_profile=name) as (process, output):
                    summary=self.result(process,output,0)
                    self.assertEqual(summary['power_profile'],power.load_profile(name))
                    self.assertEqual(json.loads((output/'manifest.json').read_text())['power_profile'],summary['power_profile'])
                    self.assertEqual(summary['accounting']['counts'],dict(submitted=3,completed=3,terminally_failed=0))

    def test_changed_source_mode_or_low_charge_invalidates_finished_sample(self):
        for change in ('source','mode','charge'):
            with self.subTest(change=change):
                self.setUp()
                with self.launch(power_profile='battery',power_change=change) as (process,output):
                    summary=self.result(process,output,1)
                    self.assertTrue(summary['accounting']['verified'])
                    self.assertIsNotNone(summary['metrics'])
                    self.assertIn({'source':'Battery power','mode':'Low Power Mode','charge':'20%'}[change],
                                  summary['first_failure'])

    def test_battery_profile_deadline_still_cleans_up(self):
        with self.launch(power_profile='battery-low-power',slow=True,deadline_ms=11000) as (process,output):
            self.assertIn('deadline',self.result(process,output,1)['first_failure'])


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--bin-dir',type=Path,default=harness_tests.BIN_DIR)
    args,remaining=parser.parse_known_args()
    harness_tests.BIN_DIR=args.bin_dir.resolve()
    unittest.main(argv=[__file__,*remaining],verbosity=2)
