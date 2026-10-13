"""Build and install in temporary directories; exercise commands through PATH."""

import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[2]
PROGRAMS = ('faultline', 'faultline-coordinator', 'faultline-worker')
OPTIONS = None


def run(command, *, cwd, env=None, timeout=90):
    # A timed-out make may still own a compiler/linker. Retire its whole group.
    process = subprocess.Popen(command, cwd=cwd, env=env, text=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               start_new_session=True)
    try:
        out, err = process.communicate(timeout=timeout)
    except BaseException:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.communicate(timeout=5)
        raise
    return subprocess.CompletedProcess(command, process.returncode, out, err)


class InstallTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='faultline-install-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        cls.source = cls.root / 'source'
        cls.source.mkdir()
        shutil.copy2(ROOT / 'Makefile', cls.source / 'Makefile')
        for directory in ('src', 'include'):
            shutil.copytree(ROOT / directory, cls.source / directory)
        cls.env = os.environ.copy()
        # The fixture owns its build configuration and never inherits a caller's
        # PREFIX/DESTDIR, jobserver descriptors, or repository BUILD_DIR override.
        for key in ('MAKEFLAGS', 'MFLAGS', 'MAKELEVEL', 'MAKEOVERRIDES',
                    'PREFIX', 'BINDIR', 'DESTDIR', 'INSTALL', 'BUILD_DIR'):
            cls.env.pop(key, None)
        cls.make_command = [OPTIONS.make, f'CC={OPTIONS.cc}',
                            f'CFLAGS={OPTIONS.cflags}', f'SANITIZE={OPTIONS.sanitize}']
        cls.build = cls.source / 'build' / ('sanitize' if OPTIONS.sanitize == '1' else 'debug')
        cls.first_prefix = cls.root / 'first install with spaces'
        # Intentionally no preceding `make all`: install must build its inputs.
        result = run([*cls.make_command, 'install', f'PREFIX={cls.first_prefix}'],
                     cwd=cls.source, env=cls.env)
        if result.returncode != 0:
            raise AssertionError(f'Fresh make install failed:\n{result.stdout}\n{result.stderr}')

    def setUp(self):
        self.directory = self.root / self._testMethodName
        self.directory.mkdir()

    def make(self, *arguments, expected=0):
        result = run([*self.make_command, *arguments], cwd=self.source, env=self.env)
        if expected == 0:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def check_binaries(self, bindir):
        self.assertEqual(sorted(p.name for p in bindir.iterdir()), sorted(PROGRAMS))
        for name in PROGRAMS:
            installed = bindir / name
            self.assertFalse(installed.is_symlink())
            self.assertEqual(stat.S_IMODE(installed.stat().st_mode), 0o755)
            self.assertEqual(installed.read_bytes(), (self.build / name).read_bytes())
            result = run([str(installed), '--help'], cwd=self.directory, env=self.env, timeout=8)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f'Usage: {name} ', result.stdout)
            self.assertEqual(result.stderr, '')

    def test_install_builds_all_three_programs_and_supports_prefix_spaces(self):
        self.check_binaries(self.first_prefix / 'bin')
        self.assertEqual(sorted(p.name for p in self.first_prefix.iterdir()), ['bin'])

    def test_destdir_stages_default_usr_local_layout(self):
        stage = self.directory / 'package root'
        self.make('install', f'DESTDIR={stage}')
        self.check_binaries(stage / 'usr/local/bin')

    def test_bindir_override_and_reinstall(self):
        stage = self.directory / 'package root'
        arguments = ('install', f'DESTDIR={stage}', 'PREFIX=/unused',
                     'BINDIR=/opt/faultline commands')
        self.make(*arguments)
        bindir = stage / 'opt/faultline commands'
        # Reinstall must replace a previous executable, including its mode.
        (bindir / 'faultline').write_text('old installation\n')
        (bindir / 'faultline').chmod(0o600)
        self.make(*arguments)
        self.check_binaries(bindir)
        self.assertFalse((stage / 'unused').exists())

    def test_uninstall_preserves_other_files_and_needs_no_compiler(self):
        stage = self.directory / 'package root'
        layout = (f'DESTDIR={stage}', 'PREFIX=/opt/faultline')
        self.make('install', *layout)
        bindir = stage / 'opt/faultline/bin'
        for name in ('unrelated-command', 'state.wal'):
            (bindir / name).write_text(f'keep {name}\n')
        self.make('uninstall', *layout, 'CC=false', 'BUILD_DIR=does-not-exist')
        self.make('uninstall', *layout, 'CC=false', 'BUILD_DIR=does-not-exist')
        self.assertEqual(sorted(p.name for p in bindir.iterdir()), ['state.wal', 'unrelated-command'])
        for name in ('unrelated-command', 'state.wal'):
            self.assertEqual((bindir / name).read_text(), f'keep {name}\n')

    def test_install_errors_fail_make(self):
        prefix = self.directory / 'failed install'
        self.make('install', f'PREFIX={prefix}', 'INSTALL=false', expected=1)
        self.assertFalse(prefix.exists())
        blocker = self.directory / 'not-a-directory'
        blocker.write_text('keep this file\n')
        self.make('install', f'BINDIR={blocker}', expected=1)
        self.assertEqual(blocker.read_text(), 'keep this file\n')

    @contextmanager
    def server_process(self, command, cwd, env):
        with tempfile.TemporaryFile() as output:
            process = subprocess.Popen(command, cwd=cwd, env=env,
                                       stdout=output, stderr=output)
            try:
                yield process, output
            finally:
                if process.poll() is None:
                    process.terminate()
                try:
                    code = process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
                    self.fail(f'Process did not stop: {command}')
                output.seek(0)
                logs = output.read().decode(errors='replace')
                self.assertEqual(code, 0, logs)
                for diagnostic in ('AddressSanitizer', 'LeakSanitizer', 'runtime error:', '[ERROR]'):
                    self.assertNotIn(diagnostic, logs)

    def wait_for_log(self, process, output, event):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            logs = os.pread(output.fileno(), os.fstat(output.fileno()).st_size, 0).decode()
            self.assertIsNone(process.poll(), logs)
            if event in logs:
                return
            time.sleep(0.02)
        self.fail(f'Missing {event}:\n{logs}')

    def test_path_commands_execute_and_replay_from_unrelated_directories(self):
        prefix = self.directory / 'personal install'
        self.make('install', f'PREFIX={prefix}')
        env = dict(self.env, PATH=str(prefix / 'bin') + os.pathsep + os.environ.get('PATH', os.defpath))
        for name in PROGRAMS:
            self.assertEqual(shutil.which(name, path=env['PATH']), str(prefix / 'bin' / name))
        locations = {role: self.directory / role for role in ('coordinator', 'worker', 'cli', 'restart')}
        for location in locations.values():
            location.mkdir()
        wal = self.directory / 'persistent state.wal'
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        endpoint = f'127.0.0.1:{port}'

        def cli(*arguments, expected=0, expected_stderr=''):
            result = run(['faultline', *arguments, '--coordinator', endpoint],
                         cwd=locations['cli'], env=env, timeout=8)
            self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
            self.assertEqual(result.stderr, expected_stderr)
            return result.stdout

        coordinator = ['faultline-coordinator', '--port', str(port), '--wal', str(wal)]
        hidden = self.root / 'source-hidden-during-runtime'
        self.source.rename(hidden)
        try:
            # No executable can be found at its original checkout/build path.
            with self.server_process([*coordinator, '--init-wal'], locations['coordinator'], env) as (server, log):
                self.wait_for_log(server, log, ' listening ')
                self.assertEqual(cli('ping'), 'PONG\n')
                with self.server_process(['faultline-worker', '--coordinator', endpoint],
                                         locations['worker'], env) as (worker, worker_log):
                    self.wait_for_log(worker, worker_log, ' registered ')
                    self.assertEqual(cli('submit', 'sleep', '--args', '0', '--max-retries', '1'), 'job_id=1\n')
                    self.wait_for_log(server, log, ' job_completed ')
                    expected = ('job_id=1\nstate=DONE\nworker_id=1\nattempt=1\n'
                                'retries=0/1\nfailure=NONE\nresult_bytes=10\nresult="slept_ms=0"\n')
                    self.assertEqual(cli('status', '1'), expected)
                    self.assertEqual(cli('status', '999', expected=2,
                                         expected_stderr='faultline: job 999 not found\n'), '')
            with self.server_process(coordinator, locations['restart'], env) as (server, log):
                self.wait_for_log(server, log, ' listening ')
                self.assertEqual(cli('status', '1'), expected)
            self.assertGreater(wal.stat().st_size, 0)
            for location in locations.values():
                self.assertFalse((location / 'faultline.wal').exists())
        finally:
            hidden.rename(self.source)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--make', default='make')
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--cflags', default='-O0 -g')
    parser.add_argument('--sanitize', choices=('0', '1'), default='0')
    OPTIONS, remaining = parser.parse_known_args()
    # Let unittest unwind process contexts if CI cancels the suite with SIGTERM.
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
