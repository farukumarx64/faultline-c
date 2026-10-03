"""Exercise the demo's real recovery, rejected results, deadline, and cancellation."""

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

ROOT = Path(__file__).resolve().parents[2]
BIN_DIR = ROOT / 'build/debug'


class DemoTests(unittest.TestCase):
    def setUp(self):
        parent = ROOT / 'build/demos'
        parent.mkdir(parents=True, exist_ok=True)
        self.directory = Path(tempfile.mkdtemp(prefix='test-', dir=parent))

    @contextmanager
    def launch(self, *extra, binaries=None, output=None):
        output = output or self.directory / 'run'
        with (self.directory / 'harness.stdout.log').open('w') as out, \
             (self.directory / 'harness.stderr.log').open('w') as err:
            process = subprocess.Popen([sys.executable, str(ROOT / 'demos/recovery.py'),
                '--bin-dir', str(binaries or BIN_DIR), '--output-dir', str(output),
                '--sleep-ms', '1200', '--hold-ms', '0', '--deadline-ms', '25000', *extra],
                stdout=out, stderr=err, start_new_session=True)
            try:
                yield process, output
            finally:
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=15)

    def finished(self, process, output, expected):
        self.assertEqual(process.wait(timeout=30), expected, str(self.directory))
        summary = json.loads((output / 'summary.json').read_text())
        self.assertEqual(summary['exit_code'], expected, summary)
        self.assertTrue(summary['cleanup']['ok'], summary)
        self.assertEqual(summary['cleanup']['remaining'], [])
        self.assertTrue(all(c['reaped'] and c['group_retired'] for c in summary['children']))
        for child in summary['children']:
            with self.assertRaises(ProcessLookupError):
                os.killpg(child['pid'], 0)
        self.assertTrue((output / 'coordinator.wal').is_file())
        self.assertTrue((output / 'recovery.cast').is_file())
        self.assertTrue((output / 'events.jsonl').is_file())
        return summary

    def wrapper(self, name, source):
        directory = self.directory / 'bin'
        directory.mkdir()
        for executable in ('faultline', 'faultline-worker', 'faultline-coordinator'):
            path = directory / executable
            if executable == name:
                path.write_text('#!' + sys.executable + '\n' + source)
                path.chmod(0o755)
            else:
                path.symlink_to((BIN_DIR / executable).resolve())
        return directory

    def test_same_id_completes_on_existing_survivor_and_recording_matches(self):
        with self.launch() as (process, output):
            summary = self.finished(process, output, 0)
        self.assertEqual(summary['verdict'], 'RECOVERY_DEMO_PASS')
        target = summary['target']
        self.assertNotEqual(target['worker_id'], target['survivor_id'])
        self.assertEqual(len([c for c in summary['children'] if c['role'].startswith('worker-')]), 2)
        final = summary['accounting']['jobs'][0]['terminal_status']
        self.assertEqual(final, dict(job_id=str(target['job_id']), state='DONE',
            worker_id=str(target['survivor_id']), attempt='2', retries='1/1', failure='NONE',
            result_bytes='13', result='"slept_ms=1200"'))
        self.assertEqual(len(summary['transitions']), 7)
        frames = [json.loads(line) for line in (output / 'recovery.cast').read_text().splitlines()]
        self.assertEqual(frames[0]['version'], 2)
        self.assertEqual(frames[-1][2], '')
        timestamps = [event[0] for event in frames[1:]]
        self.assertEqual(timestamps, sorted(timestamps))
        final_frame = frames[-2][2]
        for key, value in final.items():
            self.assertIn(f'{key}={value}\r\n', final_frame)
        self.assertIn('Cleanup passed', final_frame)

    def test_wrong_same_length_result_fails_and_keeps_evidence(self):
        binaries = self.wrapper('faultline',
            'import subprocess,sys\n'
            f'p=subprocess.run([{str(BIN_DIR / "faultline")!r},*sys.argv[1:]],capture_output=True,text=True)\n'
            'sys.stdout.write(p.stdout.replace("slept_ms=1200","slept_ms=9999"))\n'
            'sys.stderr.write(p.stderr)\nsys.exit(p.returncode)\n')
        with self.launch(binaries=binaries) as (process, output):
            summary = self.finished(process, output, 1)
        self.assertIn('wrong exact result', summary['first_failure'])
        recording = (output / 'transcript.log').read_text()
        self.assertIn('DEMO FAILED', recording)
        self.assertNotIn('5 / VERIFIED', recording)

    def test_work_deadline_cleans_up_registered_workers(self):
        start = time.monotonic()
        with self.launch('--sleep-ms', '6000', '--hold-ms', '2500',
                         '--deadline-ms', '12000') as (process, output):
            summary = self.finished(process, output, 1)
        self.assertIn('deadline', summary['first_failure'])
        self.assertLess(time.monotonic() - start, 12)
        self.assertNotIn('5 / VERIFIED', (output / 'transcript.log').read_text())

    def test_sigterm_during_running_attempt_cleans_up(self):
        with self.launch('--sleep-ms', '6000', '--hold-ms', '1500') as (process, output):
            deadline = time.monotonic() + 10
            transcript = output / 'transcript.log'
            while not transcript.exists() or '2 / ONE ACKNOWLEDGED' not in transcript.read_text():
                self.assertIsNone(process.poll(), str(self.directory))
                self.assertLess(time.monotonic(), deadline)
                time.sleep(.02)
            process.send_signal(signal.SIGTERM)
            summary = self.finished(process, output, 128 + signal.SIGTERM)
        self.assertIn('signal 15', summary['first_failure'])
        self.assertEqual(summary['fault_actions'], [])

    def test_existing_evidence_directory_is_not_overwritten(self):
        output = self.directory / 'keep'
        output.mkdir()
        sentinel = output / 'summary.json'
        sentinel.write_text('existing evidence\n')
        with self.launch(output=output) as (process, _):
            self.assertEqual(process.wait(timeout=5), 2)
        self.assertEqual(sentinel.read_text(), 'existing evidence\n')
        self.assertEqual(sorted(p.name for p in output.iterdir()), ['summary.json'])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=BIN_DIR)
    args, remaining = parser.parse_known_args()
    BIN_DIR = args.bin_dir.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
