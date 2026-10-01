"""Linux-only checks for the deadline/logging runner used by the CI workflow."""

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest


RUNNER = Path(__file__).with_name('run_with_deadline.sh')


class DeadlineTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.log = self.directory / 'command.log'
        self.identity = self.directory / 'child.json'
        self.group = None
        self.addCleanup(self.cleanup_fixture)

    def cleanup_fixture(self):
        # Also runs when an assertion fails after a wrapper exits prematurely.
        if self.group is not None:
            try:
                os.killpg(self.group, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self.assert_group_gone()

    def run_command(self, code, *, seconds='5', log=None):
        # Record only this fixture's process group, for emergency test cleanup.
        prelude = ('import json,os,signal,time,sys\n'
                   f'with open({str(self.identity)!r}, "w") as f:\n'
                   '    json.dump({"pid":os.getpid(), "group":os.getpgrp()}, f)\n')
        command = ['bash', str(RUNNER), seconds, str(self.log if log is None else log),
                   sys.executable, '-c', prelude + code]
        with tempfile.TemporaryFile() as output:
            process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            try:
                status = process.wait(timeout=25)
            finally:
                if self.identity.exists():
                    self.group = json.loads(self.identity.read_text())['group']
                if process.poll() is None:
                    # A broken wrapper must not leave the fixture running.
                    if self.group is not None:
                        try:
                            os.killpg(self.group, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait(timeout=5)
            output.seek(0)
            text = output.read().decode()
        return status, text

    def assert_group_gone(self):
        self.assertIsNotNone(self.group, 'fixture did not record its process group')
        deadline = time.monotonic() + 2
        while True:
            try:
                os.killpg(self.group, 0)
            except ProcessLookupError:
                self.group = None
                return
            self.assertLess(time.monotonic(), deadline, 'fixture process group remains')
            time.sleep(.02)

    def test_success_saves_stdout_and_stderr(self):
        status, output = self.run_command('print("out"); print("err",file=sys.stderr)')
        self.assertEqual(status, 0)
        self.assertEqual(output, self.log.read_text())
        self.assertEqual(set(output.splitlines()), {'out', 'err'})
        self.assert_group_gone()

    def test_nonzero_command_cannot_be_hidden_by_successful_tee(self):
        status, output = self.run_command('print("rejected experiment"); sys.exit(23)')
        self.assertEqual(status, 23)
        self.assertEqual(output, self.log.read_text())
        self.assertIn('rejected experiment', output)
        self.assert_group_gone()

    def test_timeout_fails_even_if_sigterm_handler_exits_zero(self):
        status, output = self.run_command('signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))\n'
                                          'print("ready",flush=True)\ntime.sleep(60)', seconds='1')
        self.assertEqual(status, 124)
        self.assertIn('ready', output)
        self.assertEqual(output, self.log.read_text())
        self.assert_group_gone()

    def test_ignored_sigterm_is_killed_after_grace_and_fails(self):
        started = time.monotonic()
        status, output = self.run_command('signal.signal(signal.SIGTERM, signal.SIG_IGN)\n'
                                          'print("ready",flush=True)\ntime.sleep(60)', seconds='1')
        self.assertEqual(status, 137)
        self.assertIn('ready', output)
        self.assertIn('ready', self.log.read_text())
        self.assertLess(time.monotonic() - started, 23)
        self.assert_group_gone()

    def test_log_write_failure_cannot_pass(self):
        status, _ = self.run_command('print("completed")', log=self.directory)
        self.assertNotEqual(status, 0)
        self.assert_group_gone()

    def test_invalid_or_disabled_deadline_starts_no_command(self):
        for seconds in ('0', '-1', '1s', 'bad', ''):
            with self.subTest(seconds=seconds):
                status, output = self.run_command('sys.exit(0)', seconds=seconds)
                self.assertEqual(status, 2)
                self.assertIn('usage:', output)
                self.assertFalse(self.identity.exists())

    def test_missing_command_fails(self):
        result = subprocess.run(['bash', str(RUNNER), '1', str(self.log),
                                 str(self.directory / 'missing-command')],
                                capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 127)
        self.assertTrue(self.log.read_text())


if __name__ == '__main__':
    # Do not silently skip the very behavior CI relies on.
    version = subprocess.run(['timeout', '--version'], capture_output=True, text=True, check=True)
    if sys.platform != 'linux' or 'GNU coreutils' not in version.stdout:
        sys.exit('These checks require Linux and GNU coreutils timeout.')
    unittest.main(verbosity=2)
