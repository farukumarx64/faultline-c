"""Regression checks for the baseline harness, including its failure/cleanup paths.

These fixtures deliberately break replies or stop a test-owned process to check
the harness. The run_batch.py experiment itself never injects workload faults.
"""

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

import run_batch as batch


BIN_DIR = batch.ROOT / 'build/debug'


class OutputTests(unittest.TestCase):
    def test_manifest_collection_starts_no_hidden_helpers(self):
        with tempfile.TemporaryDirectory() as directory:
            run = batch.BatchRun(argparse.Namespace(bin_dir=BIN_DIR), Path(directory))
            try:
                with patch.object(batch.subprocess, 'Popen', side_effect=AssertionError('untracked helper')):
                    manifest = run.manifest()
                self.assertEqual(manifest['platform']['machine'], os.uname().machine)
                self.assertEqual(len(manifest['binaries']), 3)
                self.assertEqual(run.children, [])
            finally:
                run.events.close()

    def test_rejects_missing_duplicate_unknown_fields_and_invalid_ids(self):
        for output in ('', 'job_id=1\njob_id=1\n', 'other=1\n', 'job_id\n'):
            with self.subTest(output=output), self.assertRaises(batch.RunFailure):
                batch.pairs(output, ['job_id'])
        for value in ('-1', '+1', '01', 'nan', '18446744073709551616'):
            with self.subTest(value=value), self.assertRaises(batch.RunFailure):
                batch.unsigned(value, 'ID')

    def test_rejects_truncated_extra_and_duplicate_listing_rows(self):
        header = 'jobs=2\n' + ' '.join(batch.JOB_COLUMNS) + '\n'
        row = '1 sleep DONE 1 1 0/3 NONE 11\n'
        for output in (header + row, header + row * 2, header + row * 3,
                       'jobs=0\n', 'jobs=1\nWRONG HEADER\n' + row):
            with self.subTest(output=output), self.assertRaises(batch.RunFailure):
                batch.table(output, 'jobs')

    def test_group_permission_probe_does_not_retire_ownership(self):
        with tempfile.TemporaryDirectory() as directory:
            run = batch.BatchRun(argparse.Namespace(bin_dir=BIN_DIR), Path(directory))
            try:
                child = batch.Child(argparse.Namespace(pid=123), 'fixture', Path('out'), Path('err'), [], 0)
                with patch.object(batch.os, 'killpg', side_effect=PermissionError):
                    self.assertTrue(run.group_exists(child))
                    self.assertFalse(child.group_retired)
                with patch.object(batch.os, 'killpg', side_effect=ProcessLookupError):
                    self.assertFalse(run.group_exists(child))
                    self.assertTrue(child.group_retired)
                with patch.object(batch.os, 'killpg') as probe:
                    self.assertFalse(run.group_exists(child))
                    probe.assert_not_called()
            finally:
                run.events.close()


class HarnessCase(unittest.TestCase):
    RUNNER = Path(batch.__file__)

    def setUp(self):
        # The test runner owns this parent; individual runs still create fresh dirs.
        parent = batch.ROOT / 'build/chaos'
        parent.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix='regression-', dir=parent))
        self.run_index = 0

    def wrapper(self, program, code):
        directory = self.directory / ('binaries-' + program)
        directory.mkdir()
        for name in ('faultline', 'faultline-worker', 'faultline-coordinator'):
            target = directory / name
            if name == program:
                target.write_text('#!' + sys.executable + '\n' + code, encoding='utf-8')
                target.chmod(0o755)
            else:
                target.symlink_to((BIN_DIR / name).resolve())
        return directory

    def cli_wrapper(self, mutation):
        return self.wrapper('faultline',
            'import subprocess, sys\n'
            f'result = subprocess.run([{str((BIN_DIR / "faultline").resolve())!r}, *sys.argv[1:]], capture_output=True, text=True)\n'
            'output = result.stdout\n' + mutation + '\n'
            'sys.stdout.write(output)\nsys.stderr.write(result.stderr)\nsys.exit(result.returncode)\n')

    @contextmanager
    def launch(self, *extra, binary_dir=None):
        self.run_index += 1
        output = self.directory / f'run-{self.run_index}'
        argv = [sys.executable, str(self.RUNNER), '--bin-dir', str(binary_dir or BIN_DIR),
                '--workers', '3', '--jobs', '9', '--sleep-ms', '25', '--deadline-ms', '30000',
                '--output-dir', str(output), *extra]
        with (self.directory / f'harness-{self.run_index}.stdout').open('w+') as out, \
             (self.directory / f'harness-{self.run_index}.stderr').open('w+') as err:
            process = subprocess.Popen(argv, stdout=out, stderr=err, start_new_session=True)
            try:
                yield process, output
            finally:
                if process.poll() is None:
                    process.terminate()
                # Harness deadlines plus cleanup remain below this outer watchdog.
                process.wait(timeout=40)

    def summary(self, process, output, expected=0):
        self.assertEqual(process.wait(timeout=40), expected, f'artifacts: {output}')
        summary = json.loads((output / 'summary.json').read_text())
        self.assertEqual(summary['exit_code'], expected)
        self.assertEqual(summary['cleanup']['remaining'], [])
        self.assertTrue(all(child['reaped'] and child['group_retired'] for child in summary['children']), summary)
        # Empty process groups are part of the public cleanup claim. Probe only
        # these test-owned IDs; never send signals based on names or global scans.
        for child in summary['children']:
            with self.assertRaises(ProcessLookupError, msg=f'group still exists: {child}'):
                os.killpg(child['pid'], 0)
        return summary

    def events(self, output):
        path = output / 'events.jsonl'
        if not path.exists():
            return []
        # The final line can be in flight while the harness is running.
        return [json.loads(line) for line in path.read_text().splitlines(keepends=True) if line.endswith('\n')]

    def wait_event(self, process, output, predicate):
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            for event in self.events(output):
                if predicate(event):
                    return event
            self.assertIsNone(process.poll(), f'harness exited before expected event: {output}')
            time.sleep(.02)
        self.fail(f'event deadline: {output}')


class HarnessTests(HarnessCase):
    def test_real_pool_results_identity_accounting_and_artifacts(self):
        with self.launch() as (process, output):
            summary = self.summary(process, output)
        self.assertEqual(summary['verdict'], 'BASELINE_PASS')
        self.assertEqual((summary['acknowledged'], summary['verified_completed']), (9, 9))
        self.assertEqual(summary['coverage'], dict(injected_faults=0, recovery_demonstrated=False))
        self.assertTrue(summary['cleanup']['ok'])
        manifest = json.loads((output / 'manifest.json').read_text())
        self.assertEqual((manifest['candidate_plan'], manifest['seed_used']), ([], False))
        self.assertEqual(len(manifest['binaries']['faultline']['sha256']), 64)
        self.assertTrue((output / 'coordinator.wal').stat().st_size > 0)
        ledger = json.loads((output / 'submissions.json').read_text())
        final = json.loads((output / 'final-snapshots.json').read_text())
        self.assertEqual({str(entry['job_id']) for entry in ledger}, set(final['statuses']))
        self.assertEqual(len(final['workers']), 3)
        for status in final['statuses'].values():
            self.assertEqual((status['state'], status['attempt'], status['retries'], status['result']),
                             ('DONE', '1', '0/3', '"slept_ms=25"'))
        events = self.events(output)
        self.assertEqual(sum(event['event'] == 'registered' for event in events), 3)
        self.assertEqual(events[-1]['event'], 'verdict')
        cleanup_start = next(i for i, event in enumerate(events) if event['event'] == 'cleanup_started')
        self.assertTrue(all(event['event'] != 'spawn' for event in events[cleanup_start:]))
        self.assertEqual(sum(event['event'] == 'exit_reaped' for event in events), len(summary['children']))

    def test_zero_duration_and_retry_boundaries(self):
        for retries in ('0', '4294967295'):
            with self.subTest(retries=retries), self.launch('--workers', '1', '--jobs', '2',
                                                          '--sleep-ms', '0', '--max-retries', retries) as (p, out):
                summary = self.summary(p, out)
                self.assertEqual(summary['totals']['job_attempts_total'], 2)
                self.assertEqual(summary['totals']['job_retries_total'], 0)

    def test_invalid_configuration_and_existing_output_start_no_children(self):
        for options in (['--workers', '0'], ['--workers', '17'], ['--jobs', '257'], ['--jobs', '0'],
                        ['--max-retries', '-1'], ['--seed', '4294967296'], ['--sleep-ms', 'nan'],
                        ['--deadline-ms', '10000'], ['--fault-duration-ms', '1'],
                        ['--bin-dir', str(self.directory / 'missing')]):
            with self.subTest(options=options), self.launch(*options) as (process, output):
                self.assertEqual(process.wait(timeout=5), 2)
                self.assertFalse(output.exists())
        with self.launch('--output-dir', str(self.directory)) as (process, _):
            self.assertEqual(process.wait(timeout=5), 2)
            self.assertFalse((self.directory / 'manifest.json').exists())

    def test_missing_ack_is_uncertain_and_never_resubmitted(self):
        binaries = self.cli_wrapper("if sys.argv[1] == 'submit': output = ''")
        with self.launch(binary_dir=binaries) as (process, output):
            summary = self.summary(process, output, 1)
        self.assertIn('admission uncertain', summary['first_failure'])
        self.assertEqual(summary['acknowledged'], 0)
        self.assertEqual(sum(e['event'] == 'submission_attempt' for e in self.events(output)), 1)
        coordinator = next(output.glob('*-coordinator.stdout.log')).read_text()
        self.assertEqual(coordinator.count('coordinator job_submitted '), 1)

    def test_duplicate_ack_aborts_before_third_submission(self):
        binaries = self.cli_wrapper("if sys.argv[1] == 'submit': output = 'job_id=1\\n'")
        with self.launch(binary_dir=binaries) as (process, output):
            summary = self.summary(process, output, 1)
        self.assertIn('duplicate acknowledgment ID', summary['first_failure'])
        self.assertEqual(summary['acknowledged'], 1)
        self.assertEqual(sum(e['event'] == 'submission_attempt' for e in self.events(output)), 2)

    def test_hung_submission_hits_cli_deadline_and_is_not_retried(self):
        binaries = self.cli_wrapper("if sys.argv[1] == 'submit':\n    import time\n    time.sleep(60)")
        with self.launch('--workers', '1', '--jobs', '1', binary_dir=binaries) as (process, output):
            summary = self.summary(process, output, 1)
        self.assertIn('admission uncertain', summary['first_failure'])
        self.assertIn('deadline', summary['first_failure'])
        self.assertEqual(sum(e['event'] == 'submission_attempt' for e in self.events(output)), 1)
        self.assertLess(summary['elapsed_ms'], 15000)

    def test_wrong_result_is_not_hidden_by_done_or_matching_totals(self):
        binaries = self.cli_wrapper("if sys.argv[1] == 'status': output = output.replace('slept_ms=25', 'slept_ms=26')")
        with self.launch(binary_dir=binaries) as (process, output):
            summary = self.summary(process, output, 1)
        self.assertIn('wrong exact result', summary['first_failure'])
        self.assertEqual(summary['acknowledged'], 9)

    def test_changed_terminal_result_is_rejected_on_second_read(self):
        marker = self.directory / 'seen-status'
        binaries = self.cli_wrapper(
            "if sys.argv[1] == 'status':\n"
            '    from pathlib import Path\n'
            f'    marker = Path({str(marker)!r})\n'
            "    if marker.exists() and marker.read_text() == sys.argv[2]: output = output.replace('slept_ms=25', 'slept_ms=26')\n"
            '    if not marker.exists(): marker.write_text(sys.argv[2])')
        with self.launch('--workers', '1', '--jobs', '1', binary_dir=binaries) as (process, output):
            summary = self.summary(process, output, 1)
        self.assertIn('wrong exact result', summary['first_failure'])

    def test_stats_disagreement_is_rejected(self):
        binaries = self.cli_wrapper("if sys.argv[1] == 'stats': output = output.replace('job_attempts_total=9', 'job_attempts_total=10')")
        with self.launch(binary_dir=binaries) as (process, output):
            summary = self.summary(process, output, 1)
        self.assertIn('stats disagreement', summary['first_failure'])

    def test_partial_startup_failure_cleans_other_children(self):
        binaries = self.wrapper('faultline-worker', 'import sys\nsys.exit(7)\n')
        with self.launch(binary_dir=binaries) as (process, output):
            summary = self.summary(process, output, 1)
        self.assertIn('unexpected worker-', summary['first_failure'])
        self.assertEqual(summary['acknowledged'], 0)

    def test_work_deadline_stops_active_jobs_and_reaps_pool(self):
        with self.launch('--workers', '1', '--jobs', '1', '--sleep-ms', '60000',
                         '--deadline-ms', '13000') as (process, output):
            summary = self.summary(process, output, 1)
        self.assertIn('deadline', summary['first_failure'])
        self.assertEqual(summary['acknowledged'], 1)
        self.assertLess(summary['elapsed_ms'], 13000)
        self.assertTrue(summary['cleanup']['ok'])

    def test_unplanned_coordinator_exit_cannot_pass(self):
        with self.launch('--workers', '1', '--jobs', '1', '--sleep-ms', '60000') as (process, output):
            coordinator = self.wait_event(process, output,
                lambda e: e['event'] == 'spawn' and e['role'] == 'coordinator')
            self.wait_event(process, output, lambda e: e['event'] == 'acknowledged')
            os.kill(coordinator['pid'], signal.SIGTERM)
            summary = self.summary(process, output, 1)
            self.assertEqual(summary['verdict'], 'FAIL')
            self.assertIsNotNone(summary['first_failure'])

    def test_sigint_and_sigterm_clean_up_including_a_stopped_worker(self):
        for signum in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signum=signum), self.launch('--workers', '1', '--jobs', '1',
                                                         '--sleep-ms', '60000') as (process, output):
                registration = self.wait_event(process, output, lambda e: e['event'] == 'registered')
                self.wait_event(process, output, lambda e: e['event'] == 'acknowledged')
                if signum == signal.SIGTERM:
                    os.kill(registration['pid'], signal.SIGSTOP)
                process.send_signal(signum)
                summary = self.summary(process, output, 128 + signum)
                self.assertTrue(summary['cleanup']['ok'], summary['cleanup'])

    def test_sanitizer_diagnostic_cannot_pass_even_with_exit_zero(self):
        binaries = self.cli_wrapper("if sys.argv[1] == 'submit': output += 'AddressSanitizer: fixture\\n'")
        with self.launch(binary_dir=binaries) as (process, output):
            summary = self.summary(process, output, 1)
        self.assertIn('sanitizer diagnostic', summary['first_failure'])

    def test_forced_cleanup_kill_is_failure_and_idempotent(self):
        directory = self.directory / 'cleanup'
        directory.mkdir()
        run = batch.BatchRun(argparse.Namespace(bin_dir=BIN_DIR, deadline_ms=30000), directory)
        try:
            child = run.spawn('helper-fixture', [sys.executable, '-c',
                'import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); print("ready",flush=True); time.sleep(60)'])
            deadline = time.monotonic() + 5
            while 'ready' not in child.stdout.read_text():
                run.pause(deadline, .02)
            result = run.cleanup()
            self.assertFalse(result['ok'])
            self.assertEqual(result['remaining'], [])
            self.assertEqual(child.process.returncode, -signal.SIGKILL)
            self.assertTrue(child.group_retired)
            signals = list(child.signals)
            self.assertIs(run.cleanup(), result)
            self.assertEqual(child.signals, signals)
            with self.assertRaises(batch.RunFailure):
                run.spawn('forbidden', [sys.executable, '-c', 'pass'])
        finally:
            run.cleanup()
            run.events.close()

    def test_cleanup_reaches_descendants_in_the_owned_group(self):
        directory = self.directory / 'descendants'
        directory.mkdir()
        run = batch.BatchRun(argparse.Namespace(bin_dir=BIN_DIR, deadline_ms=30000), directory)
        try:
            child = run.spawn('helper-family', [sys.executable, '-c',
                'import signal,subprocess,sys,time\n'
                'stopping = False\n'
                'def stop(*_):\n    global stopping\n    stopping = True\n'
                'signal.signal(signal.SIGTERM, stop)\n'
                'child = subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"])\n'
                'print(child.pid,flush=True)\n'
                'while not stopping: time.sleep(.01)\n'
                'child.wait(timeout=2)\n'])
            deadline = time.monotonic() + 5
            while not child.stdout.read_text().strip():
                run.pause(deadline, .02)
            result = run.cleanup()
            self.assertTrue(result['ok'], result)
            self.assertEqual(child.process.returncode, 0)
            self.assertTrue(child.group_retired)
            with self.assertRaises(ProcessLookupError):
                os.killpg(child.process.pid, 0)
        finally:
            run.cleanup()
            run.events.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=BIN_DIR)
    args, remaining = parser.parse_known_args()
    BIN_DIR = args.bin_dir.resolve()
    unittest.main(argv=[__file__, *remaining], verbosity=2)
