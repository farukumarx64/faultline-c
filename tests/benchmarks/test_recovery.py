"""Recovery benchmark verification; fixture timings are never benchmark samples."""

import argparse
import copy
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'benchmarks'))
import run_recovery as recovery

BIN_DIR = ROOT/'build/debug'


class MetricTests(unittest.TestCase):
    def test_only_revoked_workers_expected_connection_errors_are_allowed(self):
        with tempfile.TemporaryDirectory() as directory:
            args=argparse.Namespace(bin_dir=BIN_DIR)
            run=recovery.RecoveryRun(args,Path(directory),recovery.load_profile(),'measured',
                                    time.monotonic()+60,scenario='heartbeat_expiry')
            try:
                child=argparse.Namespace(process=argparse.Namespace(pid=123))
                run.fault_child=child
                run.target=dict(worker_id=7)
                run.fault.update(resume_return_ns=1,closed={})
                line='2026-10-02T00:00:00.000Z [ERROR] worker system_error operation="send job report" errno=32 pid=123 monotonic_ms=10'
                run.verify_old_worker_error(child,line)
                for bad in (line.replace('32','5'),line.replace('send job report','clock'),line.replace('pid=123','pid=456')):
                    with self.subTest(line=bad),self.assertRaises(recovery.RunFailure): run.verify_old_worker_error(child,bad)
                with self.assertRaises(recovery.RunFailure): run.verify_old_worker_error(object(),line)
                del run.fault['resume_return_ns']
                with self.assertRaises(recovery.RunFailure): run.verify_old_worker_error(child,line)
            finally:
                run.events.close()

    def test_recovery_intervals_do_not_mix_process_clocks(self):
        f=dict(signal_before_ns=100000000, signal_return_ns=101000000,
            loss=dict(observed_ns=110000000,monotonic_ms='8000'),
            reassigned=dict(observed_ns=120000000,monotonic_ms='8003'),
            restarted=dict(observed_ns=125000000), completed=dict(observed_ns=150000000,monotonic_ms='8040'),
            replacement_launch_before_ns=111000000,replacement_launch_return_ns=113000000,
            replacement_registered_ns=119000000)
        result=recovery.recovery_delays(f)
        self.assertEqual(result['observed_detection_ms'],10)
        self.assertEqual(result['observed_reassignment_ms'],10)
        self.assertEqual(result['observed_recovery_to_completion_ms'],50)
        self.assertEqual(result['coordinator_loss_to_assignment_ms'],3)
        self.assertEqual(result['coordinator_loss_to_completion_ms'],40)
        self.assertEqual(result['replacement_registration_ms'],8)
        for field in ('signal_return_ns','replacement_launch_before_ns'):
            bad=copy.deepcopy(f)
            bad[field]=1
            with self.subTest(field=field),self.assertRaises(recovery.RunFailure): recovery.recovery_delays(bad)

    def samples(self):
        profile = recovery.load_profile()
        samples = []
        for e in recovery.sample_plan(profile):
            faults = int(e['scenario'] != 'no_fault')
            elapsed = (1000 if not faults else 900 if e['scenario']=='sigkill' else 1500) + e['campaign_index']
            samples.append(dict(**e, exit_code=0, timing_valid=True,
                accounting=dict(counts=dict(submitted=32, completed=32, terminally_failed=0)),
                metrics=dict(batch_elapsed_ms=elapsed, actual_faults=faults, attempts=32+faults,
                    retries=faults, terminal_failures=0, observed_detection_ms=5 if faults else None,
                    observed_reassignment_ms=2 if faults else None, observed_recovery_to_completion_ms=100 if faults else None,
                    coordinator_loss_to_assignment_ms=1 if faults else None, coordinator_loss_to_completion_ms=90 if faults else None)))
        return profile, samples

    def test_exact_campaign_and_round_matched_differences_preserve_negatives(self):
        profile, samples = self.samples()
        self.assertEqual(len(samples), 18)
        self.assertEqual([s['scenario'] for s in samples[:3]], list(recovery.SCENARIOS))
        result = recovery.aggregate(samples, profile)
        for scenario in ('sigkill','heartbeat_expiry'):
            for pair in result[scenario]['pairs']:
                fault, control = samples[pair['fault_index']], samples[pair['control_index']]
                self.assertEqual(fault['measured_round'], control['measured_round'])
                extra = fault['metrics']['batch_elapsed_ms']-control['metrics']['batch_elapsed_ms']
                self.assertEqual(pair['extra_completion_ms'], extra)
                self.assertEqual(pair['overhead_percent'], 100*extra/control['metrics']['batch_elapsed_ms'])
        self.assertLess(result['sigkill']['metrics']['extra_completion_ms']['maximum'], 0)
        self.assertNotIn('observed_detection_ms', result['no_fault']['metrics'])

    def test_missing_reordered_failed_fabricated_and_nonfinite_samples_fail(self):
        for mutation in ('missing','duplicate','order','failure','retry','terminal','nan','absent_time','fake_control'):
            p,s = self.samples()
            if mutation=='missing': s.pop()
            elif mutation=='duplicate': s[-1]=copy.deepcopy(s[-2])
            elif mutation=='order': s[4],s[5]=s[5],s[4]
            elif mutation=='failure': s[4]['timing_valid']=False
            elif mutation=='retry': s[4]['metrics']['retries']=2
            elif mutation=='terminal': s[4]['metrics']['terminal_failures']=1
            elif mutation=='nan': s[4]['metrics']['observed_detection_ms']=float('nan')
            elif mutation=='absent_time': s[4]['metrics']['observed_detection_ms']=None
            else: s[3]['metrics']['observed_detection_ms']=0
            with self.subTest(mutation=mutation), self.assertRaises(recovery.RunFailure):
                recovery.aggregate(s,p)

    def history(self):
        sequence = [('job_submitted','QUEUED','ACCEPTED','0','0','0','NONE','0'),
            ('job_assigned','ASSIGNED','ASSIGNED','1','1','0','NONE','0'),
            ('job_started','RUNNING','RUNNING','1','1','0','NONE','0'),
            ('job_worker_lost','QUEUED','REQUEUED','0','1','1','WORKER_LOST','1'),
            ('job_assigned','ASSIGNED','ASSIGNED','2','2','1','NONE','0'),
            ('job_started','RUNNING','RUNNING','2','2','1','NONE','0'),
            ('job_completed','DONE','COMPLETED','2','2','1','NONE','0')]
        events = []
        for i, values in enumerate(sequence):
            e = dict(zip(('event','state','outcome','worker_id','attempt','retry_count','failure','previous_worker_id'),values))
            e.update(task='sleep',max_retries='1',durable='1',result_bytes='14' if i==6 else '0',
                     wal_sequence=str(i+1),monotonic_ms=str(100+i*10),result='slept_ms=10000' if i==6 else '')
            events.append(e)
        return {1:events}

    def test_recovered_latency_uses_one_coordinator_clock_and_exact_loss_history(self):
        latency, history = recovery.recovery_history(self.history(), {1}, {1,2}, dict(job_id=1,worker_id=1),
                                                      'sigkill','slept_ms=10000',1)
        self.assertEqual(latency['mean_accepted_latency_ms'],60)
        self.assertEqual(history[1], [dict(worker_id=1,job_id=1,attempt=1,outcome='REQUEUED',wal_sequence=4)])

    def test_old_attempt_wrong_result_invalid_retry_and_duplicate_transitions_fail(self):
        for mutation in ('old','result','clock','retry','duplicate','lost_owner','missing_id'):
            h=self.history()
            if mutation=='old': h[1][4]['worker_id']='1'
            elif mutation=='result': h[1][-1]['result']='slept_ms=10001'
            elif mutation=='clock': h[1][4]['monotonic_ms']='99'
            elif mutation=='retry': h[1][3]['retry_count']='0'
            elif mutation=='duplicate': h[1].append(h[1][-1])
            elif mutation=='lost_owner': h[1][3]['previous_worker_id']='2'
            else: h[2]=h.pop(1)
            with self.subTest(mutation=mutation), self.assertRaises(recovery.RunFailure):
                recovery.recovery_history(h,{1},{1,2},dict(job_id=1,worker_id=1),'sigkill','slept_ms=10000',1)


class ProcessTests(unittest.TestCase):
    def setUp(self):
        parent=ROOT/'build/benchmarks'
        parent.mkdir(parents=True,exist_ok=True)
        self.directory=Path(tempfile.mkdtemp(prefix='recovery-regression-',dir=parent))

    def run_fixture(self, scenario, *, arguments='1000', jobs=4, pause_ms=10000,
                    deadline_ms=35000, bad_result=False, slow_admission=False, interrupt=False):
        output=self.directory/'run'
        output.mkdir()
        binaries=BIN_DIR
        if bad_result or slow_admission:
            binaries=self.directory/'bin'
            binaries.mkdir()
            for name in recovery.base.PROGRAMS:
                target=binaries/name
                if name!='faultline': target.symlink_to((BIN_DIR/name).resolve())
                else:
                    target.write_text('#!'+sys.executable+'\nimport subprocess,sys,time\n'
                        f'if {slow_admission!r} and sys.argv[1]=="submit": time.sleep(.3)\n'
                        f'r=subprocess.run([{str((BIN_DIR/name).resolve())!r},*sys.argv[1:]],capture_output=True,text=True)\n'
                        'output=r.stdout\n'
                        f'if {bad_result!r} and sys.argv[1]=="status": output=output.replace("slept_ms=1000","slept_ms=1001")\n'
                        'sys.stdout.write(output)\nsys.stderr.write(r.stderr)\nsys.exit(r.returncode)\n')
                    target.chmod(0o755)
        profile=recovery.load_profile()
        profile.update(contract='recovery-regression-fixture',jobs=jobs,workers=2,arguments=arguments,
                       expected_result='slept_ms='+arguments,expected_result_bytes=len('slept_ms='+arguments))
        profile['scenarios']['heartbeat_expiry']['pause_duration_ms']=pause_ms
        script=self.directory/'fixture.py'
        script.write_text('import sys,time,argparse\nsys.dont_write_bytecode=True\n'
            f'sys.path.insert(0,{str(ROOT/"benchmarks")!r})\nimport run_recovery as r\n'
            f'p={profile!r}\na=argparse.Namespace(bin_dir=r.base.Path({str(binaries)!r}),workers=2,jobs={jobs},'
            f'max_retries=1,deadline_ms={deadline_ms},output_dir=r.base.Path({str(output)!r}))\n'
            f'run=r.RecoveryRun(a,a.output_dir,p,"measured",time.monotonic()+60,scenario={scenario!r},'
            'snapshot=lambda *args: {"fixture":True})\nsys.exit(run.run())\n')
        with (self.directory/'stdout.log').open('w') as out,(self.directory/'stderr.log').open('w') as err:
            process=subprocess.Popen([sys.executable,str(script)],stdout=out,stderr=err,start_new_session=True)
            try:
                if interrupt:
                    limit=time.monotonic()+10
                    while True:
                        trace=output/'events.jsonl'
                        text=trace.read_text() if trace.exists() else ''
                        if '"event": "intervention"' in text: break
                        self.assertIsNone(process.poll(),str(self.directory))
                        self.assertLess(time.monotonic(),limit)
                        time.sleep(.01)
                    process.send_signal(signal.SIGTERM)
                process.wait(timeout=45)
            finally:
                if process.poll() is None: process.terminate()
                process.wait(timeout=15)
        summary=json.loads((output/'summary.json').read_text())
        self.assertEqual(summary['exit_code'],process.returncode,str(self.directory))
        self.assertEqual(summary['cleanup']['remaining'],[],str(self.directory))
        self.assertTrue(all(c['reaped'] and c['group_retired'] for c in summary['children']),str(self.directory))
        for child in summary['children']:
            with self.assertRaises(ProcessLookupError): os.killpg(child['pid'],0)
        return summary

    def assert_pass(self, result, retries):
        self.assertEqual(result['exit_code'],0,(result['first_failure'],str(self.directory)))
        self.assertTrue(result['timing_valid'])
        self.assertTrue(result['cleanup']['ok'])
        self.assertEqual(result['verified_completed'],4)
        self.assertEqual(result['metrics']['retries'],retries)
        self.assertEqual(result['metrics']['terminal_failures'],0)

    def test_no_fault_control_observes_same_target_and_checkpoint(self):
        result=self.run_fixture('no_fault')
        self.assert_pass(result,0)
        self.assertIn('checkpoint_ns',result['fault'])
        self.assertNotIn('signal_before_ns',result['fault'])
        self.assertIsNone(result['metrics']['observed_detection_ms'])

    def test_sigkill_recovers_exact_first_job_during_slow_admission(self):
        result=self.run_fixture('sigkill',slow_admission=True)
        self.assert_pass(result,1)
        fault=result['fault']
        ledger=json.loads((self.directory/'run/submissions.json').read_text())
        self.assertEqual(fault['target']['job_id'],ledger[0]['job_id'])
        self.assertLess(fault['signal_return_ns'],ledger[-1]['ack_observed_ns'])
        self.assertNotIn('timeout',fault)
        self.assertNotEqual(fault['completed']['worker_id'],str(fault['target']['worker_id']))

    def test_heartbeat_expiry_keeps_worker_stopped_until_fixed_resume(self):
        result=self.run_fixture('heartbeat_expiry')
        self.assert_pass(result,1)
        f=result['fault']
        self.assertEqual(f['death']['reason'],'heartbeat_timeout')
        self.assertGreaterEqual(int(f['timeout']['silence_ms']),6000)
        self.assertLess(f['timeout']['observed_ns'],f['resume_before_ns'])
        self.assertGreaterEqual(f['resume_before_ns']-f['signal_return_ns'],10000000000)
        child=next(c for c in result['children'] if c['pid']==f['target']['pid'])
        self.assertEqual(child['returncode'],1)

    def test_short_pause_rejects_insufficient_coverage(self):
        r=self.run_fixture('heartbeat_expiry',pause_ms=100)
        self.assertEqual(r['exit_code'],1)
        self.assertIn('INSUFFICIENT_COVERAGE',r['first_failure'])
        self.assertFalse(r['timing_valid'])

    def test_missed_target_is_not_replaced_with_another_job(self):
        r=self.run_fixture('sigkill',arguments='10')
        self.assertEqual(r['exit_code'],1)
        self.assertFalse(r['timing_valid'])
        self.assertNotIn('signal_return_ns',r['fault'])

    def test_wrong_exact_result_invalidates_timing(self):
        r=self.run_fixture('no_fault',bad_result=True)
        self.assertEqual(r['exit_code'],1)
        self.assertIn('wrong exact result',r['first_failure'])
        self.assertFalse(r['timing_valid'])

    def test_deadline_during_paused_worker_still_cleans_every_child(self):
        r=self.run_fixture('heartbeat_expiry',arguments='10000',deadline_ms=13000)
        self.assertEqual(r['exit_code'],1)
        self.assertIn('deadline',r['first_failure'])
        self.assertTrue(r['cleanup']['ok'],r['cleanup'])

    def test_sigterm_during_pause_resumes_and_reaps_owned_worker(self):
        r=self.run_fixture('heartbeat_expiry',arguments='10000',interrupt=True)
        self.assertEqual(r['exit_code'],128+signal.SIGTERM)
        self.assertTrue(r['cleanup']['ok'],r['cleanup'])


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--bin-dir',type=Path,default=BIN_DIR)
    args,remaining=parser.parse_known_args()
    BIN_DIR=args.bin_dir.resolve()
    unittest.main(argv=[__file__,*remaining],verbosity=2)
