"""Seed/trace invariants, live worker crashes, replacements, and negative coverage."""

import argparse
from copy import deepcopy
import errno
import json
from pathlib import Path
import random
import signal
import sys
import tempfile
import unittest
from unittest.mock import patch

import run_batch as batch
import run_chaos as chaos
import test_batch as baseline


class PlanAndEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        args = argparse.Namespace(bin_dir=baseline.BIN_DIR, seed=42, workers=2, jobs=2,
                                  sleep_ms=2000, max_retries=1, fault_duration_ms=4000)
        self.run = chaos.ChaosRun(args, Path(self.temporary.name))
        self.addCleanup(self.run.events.close)
        self.run.coordinator = argparse.Namespace(process=argparse.Namespace(pid=123))

    def record(self, event, fields, level='INFO'):
        self.run.runtime_line(self.run.coordinator,
            f'time=unavailable [{level}] coordinator {event} {fields} pid=123 monotonic_ms=100', Path('fixture.log'))

    def action(self):
        action = dict(worker_id=7, fd='6', observed=dict(job_id=1, attempt=1), loss=None, death=None, closed=None)
        self.run.actions.append(action)
        self.run.crashes[7] = action
        return action

    def test_literal_seed_plan_and_private_generator(self):
        state = random.getstate()
        expected = [dict(index=0, gap_ms=7238, slots=[3, 1, 2, 4, 0]),
                    dict(index=1, gap_ms=4006, slots=[3, 0, 2, 4, 1]),
                    dict(index=2, gap_ms=7543, slots=[3, 1, 2, 0, 4])]
        self.assertEqual(chaos.candidate_plan(42, 5, 6000), expected)
        self.assertEqual(chaos.candidate_plan(42, 5, 8000)[:3], expected)
        self.assertEqual(random.getstate(), state)
        self.assertNotEqual(chaos.candidate_plan(7, 5, 6000), expected)
        self.assertEqual(chaos.candidate_plan(42, 5, 1999), [])

    def test_manifest_saves_complete_plan_before_children(self):
        manifest = self.run.manifest()
        self.assertEqual(manifest['candidate_plan'], chaos.candidate_plan(42, 2, 4000))
        self.assertEqual((manifest['mode'], manifest['seed_used']), ('chaos', True))
        self.assertEqual(self.run.children, [])

    def test_actual_lost_lease_can_differ_from_target_snapshot(self):
        action = self.action()
        self.record('job_assigned', 'worker_id=7 job_id=2 attempt=1 state=ASSIGNED')
        self.record('job_started', 'worker_id=7 job_id=2 attempt=1 state=RUNNING')
        self.record('job_worker_lost', 'previous_worker_id=7 worker_id=0 job_id=2 attempt=1 state=QUEUED '
                    'retry_count=1 max_retries=1 outcome=REQUEUED failure=WORKER_LOST result_bytes=0 durable=1 wal_sequence=9', 'WARN')
        self.record('worker_dead', 'worker_id=7 fd=6 state=DEAD reason=eof', 'WARN')
        self.record('client_closed', 'worker_id=7 fd=6 reason=eof')
        self.assertEqual(action['observed']['job_id'], 1)
        self.assertEqual(action['loss']['job_id'], 2)
        self.run.account_transport_warnings(action)

    def test_busy_snapshot_without_interruption_cannot_prove_recovery(self):
        self.action()
        self.record('job_assigned', 'worker_id=7 job_id=1 attempt=1 state=ASSIGNED')
        self.record('job_completed', 'worker_id=7 job_id=1 attempt=1 state=DONE')
        self.record('worker_dead', 'worker_id=7 fd=6 state=DEAD reason=eof', 'WARN')
        self.assertEqual(self.run.coverage()['interrupted_attempts'], 0)
        with self.assertRaisesRegex(batch.RunFailure, 'INSUFFICIENT_COVERAGE'):
            self.run.verify_coverage()

    def test_unknown_worker_loss_and_missing_loss_transition_are_rejected(self):
        with self.assertRaisesRegex(batch.RunFailure, 'unplanned'):
            self.record('worker_dead', 'worker_id=7 fd=6 reason=eof', 'WARN')
        self.action()
        self.record('job_assigned', 'worker_id=7 job_id=1 attempt=1 state=ASSIGNED')
        with self.assertRaisesRegex(batch.RunFailure, 'omitted an active job-loss'):
            self.record('worker_dead', 'worker_id=7 fd=6 reason=eof', 'WARN')

    def test_transport_warning_requires_matching_crash_close(self):
        action = self.action()
        self.record('system_error', f'operation=recv errno={errno.ECONNRESET} message="connection reset"', 'WARN')
        action['closed'] = dict(reason='recv_error', monotonic_ms='101')
        self.run.account_transport_warnings(action)
        self.assertTrue(self.run.transport_warnings[0]['matched'])
        self.record('system_error', f'operation=recv errno={errno.ECONNRESET}', 'WARN')
        action['closed']['reason'] = 'eof'
        with self.assertRaisesRegex(batch.RunFailure, 'unattributed'):
            self.run.account_transport_warnings(action)

    def test_heartbeat_expiry_and_protocol_warnings_are_never_expected_crashes(self):
        self.action()
        for event, fields in [('heartbeat_timeout', 'worker_id=7'), ('invalid_message', 'fd=6 code=1'),
                              ('system_error', f'operation=accept errno={errno.ECONNRESET}')]:
            with self.subTest(event=event), self.assertRaises(batch.RunFailure):
                self.record(event, fields, 'WARN')

    def test_terminal_failures_and_retries_have_independent_accounting(self):
        run = self.run
        run.all_worker_ids.update((7, 8, 9, 10))
        run.ledger = [dict(job_id=1), dict(job_id=2)]
        rows = batch.table('jobs=2\n' + ' '.join(batch.JOB_COLUMNS) + '\n'
                           '1 sleep DONE 9 2 1/1 NONE 13\n'
                           '2 sleep FAILED 10 2 1/1 WORKER_LOST 0\n', 'jobs')
        run.observe_jobs(rows)
        run.actions = [dict(worker_id=7, loss=dict(job_id=1, attempt=1, outcome='REQUEUED')),
                       dict(worker_id=8, loss=dict(job_id=2, attempt=1, outcome='REQUEUED')),
                       dict(worker_id=10, loss=dict(job_id=2, attempt=2, outcome='FAILED'))]
        run.verify_job_history()
        run.verify_coverage()
        stats = run.expected_stats()
        self.assertEqual([stats[k] for k in ('jobs_completed_total', 'jobs_failed_total',
                                           'job_attempts_total', 'job_retries_total')], [1, 1, 4, 2])
        self.assertEqual(run.coverage()['recovered_job_ids'], [1])
        rows[1] = {**rows[1], 'WORKER_ID': '8'}
        with self.assertRaisesRegex(batch.RunFailure, 'terminal job 1 changed'):
            run.observe_jobs(rows)

    def test_unattributed_retry_and_drain_failure_fail(self):
        run = self.run
        run.all_worker_ids.add(7)
        run.ledger = [dict(job_id=1), dict(job_id=2)]
        rows = batch.table('jobs=2\n' + ' '.join(batch.JOB_COLUMNS) + '\n'
                          '1 sleep DONE 7 2 1/1 NONE 13\n'
                          '2 sleep FAILED 7 2 1/1 WORKER_LOST 0\n', 'jobs')
        run.drain_eligible = {2}
        with self.assertRaisesRegex(batch.RunFailure, 'unexpected terminal job failure'):
            run.observe_jobs(rows)
        run.drain_eligible.clear()
        run.observe_jobs(rows)
        with self.assertRaisesRegex(batch.RunFailure, 'retry without attributed'):
            run.verify_job_history()


class AccountingTests(unittest.TestCase):
    """Challenge the oracle with internally consistent but incorrect replies."""

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        args = argparse.Namespace(bin_dir=baseline.BIN_DIR, seed=42, workers=2, jobs=3,
                                  sleep_ms=2000, max_retries=2, fault_duration_ms=4000)
        self.run = chaos.ChaosRun(args, self.directory)
        self.addCleanup(self.run.events.close)
        self.run.all_worker_ids.update(range(7, 13))
        # Deliberately non-contiguous IDs; numeric order is not submission order.
        self.run.ledger = [dict(index=i, task='sleep', arguments='2000', max_retries=2, job_id=job_id)
                           for i, job_id in enumerate((205, 101, 309))]
        self.rows = batch.table('jobs=3\n' + ' '.join(batch.JOB_COLUMNS) + '\n'
                                '101 sleep DONE 8 1 0/2 NONE 13\n'
                                '205 sleep DONE 11 2 1/2 NONE 13\n'
                                '309 sleep FAILED 12 3 2/2 WORKER_LOST 0\n', 'jobs')
        self.run.actions = [dict(worker_id=wid, loss=dict(job_id=job, attempt=attempt,
                            outcome=outcome, wal_sequence=sequence))
                            for sequence, (wid, job, attempt, outcome) in enumerate([
                                (7, 205, 1, 'REQUEUED'), (9, 309, 1, 'REQUEUED'),
                                (10, 309, 2, 'REQUEUED'), (12, 309, 3, 'FAILED')], 10)]
        self.run.drain_eligible = {205}

    def statuses(self):
        statuses = {}
        for job_id, row in self.rows.items():
            status = {key: row[key.upper()] for key in
                      ('job_id', 'state', 'worker_id', 'attempt', 'retries', 'failure', 'result_bytes')}
            if row['STATE'] == 'DONE':
                status['result'] = '"slept_ms=2000"'
            statuses[job_id] = status
        return statuses

    def verify(self, statuses=None):
        self.run.observe_jobs(self.rows)
        statuses = self.statuses() if statuses is None else statuses
        def reply(_command, identity):
            return ''.join(f'{key}={value}\n' for key, value in statuses[int(identity)].items())
        with patch.object(self.run, 'cli', side_effect=reply):
            checked = {job_id: self.run.status(job_id, row) for job_id, row in self.rows.items()}
        self.run.verify_accounting(self.rows, checked)
        return self.run.accounting

    def test_partition_report_joins_inputs_exact_results_and_every_lost_attempt(self):
        report = self.verify()
        self.assertEqual(report['counts'], dict(submitted=3, completed=2, terminally_failed=1))
        self.assertEqual(report['submitted_ids'], [101, 205, 309])
        self.assertEqual(report['completed_ids'], [101, 205])
        self.assertEqual(report['terminally_failed_ids'], [309])
        self.assertEqual(report['drain_eligible_ids'], [205])
        self.assertEqual([j['submission']['job_id'] for j in report['jobs']], [205, 101, 309])
        exhausted = report['jobs'][2]
        self.assertEqual([loss['attempt'] for loss in exhausted['lost_attempts']], [1, 2, 3])
        self.assertEqual(exhausted['terminal_status']['failure'], 'WORKER_LOST')
        self.assertEqual(json.loads((self.directory / 'accounting.json').read_text()), report)

    def test_equal_totals_cannot_hide_one_missing_and_one_unexpected_id(self):
        self.rows[999] = {**self.rows.pop(101), 'JOB_ID': '999'}
        self.assertEqual(len(self.rows), len(self.run.ledger))
        with self.assertRaisesRegex(batch.RunFailure, r'missing=\[101\], unexpected=\[999\]'):
            self.verify()
        self.assertFalse((self.directory / 'accounting.json').exists())

    def test_duplicate_submission_ledger_is_not_collapsed_to_a_set(self):
        self.run.ledger[1]['job_id'] = 205
        with self.assertRaisesRegex(batch.RunFailure, 'duplicate IDs'):
            self.verify()

    def test_missing_or_swapped_status_id_cannot_pass(self):
        self.run.observe_jobs(self.rows)
        statuses = self.statuses()
        statuses[999] = statuses.pop(101)
        with self.assertRaisesRegex(batch.RunFailure, 'final statuses differs'):
            self.run.verify_accounting(self.rows, statuses)
        statuses = self.statuses()
        statuses[101]['job_id'] = '205'
        with self.assertRaisesRegex(batch.RunFailure, 'status/listing disagreement'):
            self.verify(statuses)

    def test_correct_length_and_done_state_cannot_hide_wrong_result(self):
        statuses = self.statuses()
        statuses[101]['result'] = '"slept_ms=2001"'
        with self.assertRaisesRegex(batch.RunFailure, 'wrong exact result for job 101'):
            self.verify(statuses)

    def test_nonterminal_job_is_not_counted_as_a_failure(self):
        self.rows[309] = {**self.rows[309], 'STATE': 'RUNNING', 'FAILURE': 'NONE'}
        self.run.observe_jobs(self.rows)
        with self.assertRaisesRegex(batch.RunFailure, 'terminal observations differs'):
            self.run.verify_accounting(self.rows, self.statuses())

    def test_retry_limits_attempt_relation_and_failure_conditions(self):
        original = deepcopy(self.rows)
        mutations = [dict(ATTEMPT='4', RETRIES='3/2'), dict(ATTEMPT='4'),
                     dict(ATTEMPT='2'), dict(RETRIES='2/3'),
                     dict(ATTEMPT='2', RETRIES='1/2'), dict(FAILURE='TASK'), dict(RESULT_BYTES='13')]
        for mutation in mutations:
            with self.subTest(mutation=mutation), self.assertRaises(batch.RunFailure):
                self.rows = deepcopy(original)
                self.rows[309].update(mutation)
                self.run.terminal.clear()
                self.verify()

    def test_exhausted_job_that_was_eligible_at_drain_is_still_a_failure(self):
        self.run.drain_eligible.add(309)
        with self.assertRaisesRegex(batch.RunFailure, 'unexpected terminal job failure'):
            self.verify()

    def test_loss_for_unsubmitted_id_cannot_be_ignored(self):
        self.run.actions.append(dict(worker_id=13, loss=dict(job_id=999, attempt=1, outcome='REQUEUED')))
        with self.assertRaisesRegex(batch.RunFailure, 'unacknowledged job'):
            self.verify()

    def test_gaps_duplicates_and_wrong_exhausted_attempt_are_rejected(self):
        original = deepcopy(self.run.actions)
        for index, attempt in ((1, 2), (1, 4), (3, 2)):
            with self.subTest(index=index, attempt=attempt), self.assertRaises(batch.RunFailure):
                self.run.actions = deepcopy(original)
                self.run.actions[index]['loss']['attempt'] = attempt
                self.verify()

    def test_missing_and_extra_loss_transitions_cannot_match_valid_counters(self):
        original = deepcopy(self.run.actions)
        for actions in (original[:1] + original[2:], original + [original[0]], original[:-1]):
            with self.subTest(actions=actions), self.assertRaises(batch.RunFailure):
                self.run.actions = actions
                self.verify()


class CrashProcessTests(baseline.HarnessCase):
    RUNNER = Path(chaos.__file__)

    def launch(self, *extra, binary_dir=None):
        return super().launch('--workers', '2', '--jobs', '6', '--sleep-ms', '2000', '--seed', '1',
                              '--fault-duration-ms', '4000', *extra, binary_dir=binary_dir)

    def test_kill_replacement_and_recovered_result_have_correlated_evidence(self):
        with self.launch() as (process, output):
            summary = self.summary(process, output)
        self.assertEqual(summary['verdict'], 'CHAOS_PASS')
        self.assertEqual((summary['acknowledged'], summary['verified_completed'], summary['verified_failed']), (6, 6, 0))
        self.assertTrue(summary['cleanup']['ok'])
        report = self.assert_accounted(output, summary)
        self.assertTrue(report['drain_eligible_ids'])
        self.assertTrue(summary['coverage']['recovery_demonstrated'])
        self.assertEqual(summary['coverage']['injected_faults'], 1)
        action = summary['fault_actions'][0]
        replacement = action['replacement']
        self.assertNotEqual(action['worker_id'], replacement['worker_id'])
        self.assertEqual(replacement['generation'], action['generation'] + 1)
        window = summary['fault_window']
        self.assertTrue(window['start_elapsed_ms'] <= action['signal_elapsed_ms'] < window['start_elapsed_ms'] + 4000)
        snapshots = json.loads((output / 'final-snapshots.json').read_text())
        lost = snapshots['statuses'][str(action['loss']['job_id'])]
        self.assertGreater(int(lost['attempt']), action['loss']['attempt'])
        crashed = [child for child in summary['children'] if child['intentional_crash']]
        self.assertEqual(len(crashed), 1)
        self.assertEqual((crashed[0]['pid'], crashed[0]['returncode']), (action['pid'], -signal.SIGKILL))
        self.assertEqual(json.loads((output / 'fault-actions.json').read_text()), summary['fault_actions'])
        manifest = json.loads((output / 'manifest.json').read_text())
        self.assertEqual(manifest['candidate_plan'], chaos.candidate_plan(1, 2, 4000))
        events = self.events(output)
        self.assertEqual(sum(e['event'] == 'fault_signal' for e in events), 1)
        self.assertEqual(sum(e['event'] == 'replacement_ready' for e in events), 1)

    def test_zero_retry_allowance_requires_attributed_exhaustion(self):
        with self.launch('--max-retries', '0') as (process, output):
            summary = self.summary(process, output)
        self.assertEqual((summary['verified_completed'], summary['verified_failed']), (5, 1))
        self.assertEqual(summary['totals']['job_retries_total'], 0)
        self.assert_accounted(output, summary)
        self.assertEqual(len(summary['coverage']['exhausted_job_ids']), 1)
        self.assertFalse(summary['coverage']['recovery_demonstrated'])

    def test_no_busy_worker_waits_full_window_then_fails_coverage(self):
        with self.launch('--jobs', '2', '--sleep-ms', '0') as (process, output):
            summary = self.summary(process, output, 1)
        self.assertIn('INSUFFICIENT_COVERAGE', summary['first_failure'])
        self.assertEqual(summary['coverage_status'], 'INSUFFICIENT_COVERAGE')
        self.assertEqual(summary['coverage']['injected_faults'], 0)
        self.assertEqual(summary['verified_completed'], 2)
        self.assert_accounted(output, summary)
        window = summary['fault_window']
        self.assertGreaterEqual(window['settled_elapsed_ms'] - window['start_elapsed_ms'], 4000)
        self.assertTrue(any(e['event'] == 'fault_skipped' and e['reason'] == 'no_busy_worker' for e in self.events(output)))

    def test_tiny_window_records_unused_plan_and_cannot_pass(self):
        with self.launch('--jobs', '1', '--sleep-ms', '0', '--fault-duration-ms', '2000') as (process, output):
            summary = self.summary(process, output, 1)
        self.assertEqual(summary['unused_plan_from'], 0)
        self.assertEqual(summary['fault_actions'], [])
        self.assertEqual(summary['coverage_status'], 'INSUFFICIENT_COVERAGE')

    def test_zero_window_explicitly_selects_baseline(self):
        with self.launch('--sleep-ms', '0', '--fault-duration-ms', '0') as (process, output):
            summary = self.summary(process, output)
        self.assertEqual((summary['mode'], summary['verdict']), ('baseline', 'BASELINE_PASS'))

    def test_matching_listing_total_with_substituted_id_fails_after_faults(self):
        binaries = self.cli_wrapper(
            'from pathlib import Path\n'
            f"if sys.argv[1] == 'jobs' and list(Path({str(self.directory)!r}).glob('run-*/drain-start.json')):\n"
            "    lines = output.splitlines()\n"
            "    fields = lines[2].split()\n"
            "    fields[0] = '999'\n"
            "    lines[2] = ' '.join(fields)\n"
            "    output = '\\n'.join(lines) + '\\n'")
        with self.launch(binary_dir=binaries) as (process, output):
            summary = self.summary(process, output, 1)
        self.assertIn('job listing differs from acknowledged ID set', summary['first_failure'])
        self.assertIn('unexpected=[999]', summary['first_failure'])
        self.assertEqual(summary['acknowledged'], 6)
        self.assertIsNone(summary['accounting'])
        self.assertFalse((output / 'accounting.json').exists())
        self.assertTrue(any(e['event'] == 'drain_started' for e in self.events(output)))
        self.assertTrue(summary['cleanup']['ok'])

    def test_unknown_acknowledged_status_is_not_a_terminal_failure(self):
        binaries = self.cli_wrapper("if sys.argv[1] == 'status':\n"
                                    "    output = 'job_id=' + sys.argv[2] + '\\nstate=UNKNOWN\\n'")
        with self.launch(binary_dir=binaries) as (process, output):
            summary = self.summary(process, output, 1)
        self.assertIn('output fields', summary['first_failure'])
        self.assertIsNone(summary['accounting'])
        self.assertTrue(summary['cleanup']['ok'])

    def test_drain_deadline_cannot_count_unfinished_work_as_failed(self):
        with self.launch('--sleep-ms', '6000', '--deadline-ms', '19000') as (process, output):
            summary = self.summary(process, output, 1)
        self.assertIn('deadline', summary['first_failure'])
        self.assertEqual(summary['acknowledged'], 6)
        self.assertEqual(summary['verified_failed'], 0)
        self.assertIsNone(summary['accounting'])
        self.assertTrue(summary['drain_eligible_job_ids'])
        events = self.events(output)
        start = next(i for i, e in enumerate(events) if e['event'] == 'drain_started')
        self.assertFalse(any(e['event'] == 'fault_signal' for e in events[start:]))
        self.assertTrue(summary['cleanup']['ok'])

    def replacement_wrapper(self, wait=False):
        marker = self.directory / 'first-worker'
        real = str((baseline.BIN_DIR / 'faultline-worker').resolve())
        waiting = ('import signal,time\nsignal.signal(signal.SIGTERM, lambda *_: sys.exit(0))\n'
                   'print("replacement_waiting",flush=True)\ntime.sleep(60)\n') if wait else 'sys.exit(7)\n'
        return self.wrapper('faultline-worker', 'import os,sys\n'
            f'try: fd = os.open({str(marker)!r}, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)\n'
            'except FileExistsError: pass\n'
            'else:\n    os.close(fd)\n'
            f'    os.execv({real!r}, [{real!r}, *sys.argv[1:]])\n' + waiting)

    def test_failed_replacement_fails_and_reaps_all_generations(self):
        binaries = self.replacement_wrapper()
        with self.launch('--workers', '1', '--jobs', '3', binary_dir=binaries) as (process, output):
            summary = self.summary(process, output, 1)
        self.assertIn('worker-0-g1 exit: 7', summary['first_failure'])
        self.assertEqual(len(summary['fault_actions']), 1)
        self.assertIsNone(summary['fault_actions'][0]['replacement'])

    def test_sigterm_while_replacement_waits_still_cleans_every_generation(self):
        binaries = self.replacement_wrapper(wait=True)
        with self.launch('--workers', '1', '--jobs', '3', binary_dir=binaries) as (process, output):
            self.wait_event(process, output, lambda e: e['event'] == 'spawn' and e['role'] == 'worker-0-g1')
            process.send_signal(signal.SIGTERM)
            summary = self.summary(process, output, 143)
        self.assertEqual(len(summary['fault_actions']), 1)
        self.assertTrue(all(child['reaped'] for child in summary['children']))

    def test_full_window_must_fit_after_admission(self):
        binaries = self.cli_wrapper("if sys.argv[1] == 'submit':\n    import time\n    time.sleep(1)")
        with self.launch('--jobs', '2', '--sleep-ms', '0', '--deadline-ms', '15000',
                         binary_dir=binaries) as (process, output):
            summary = self.summary(process, output, 1)
        self.assertIn('full fault window does not fit', summary['first_failure'])
        self.assertEqual(summary['fault_actions'], [])

    def test_invalid_window_configuration_starts_no_children(self):
        for options in (['--fault-duration-ms', '-1'], ['--fault-duration-ms', '20000001'],
                        ['--deadline-ms', '14000']):
            with self.subTest(options=options), self.launch(*options) as (process, output):
                self.assertEqual(process.wait(timeout=5), 2)
                self.assertFalse(output.exists())


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=baseline.BIN_DIR)
    args, remaining = parser.parse_known_args()
    baseline.BIN_DIR = args.bin_dir.resolve()
    unittest.main(argv=[__file__, *remaining], verbosity=2)
