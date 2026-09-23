"""Durability boundaries and restart behavior of the real coordinator (stdlib only)."""
import argparse
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zlib

import test_ping
from test_scheduling import frame, identity, receive_frame, submit

BIN_DIR = Path('build/debug')


def read_wal(path, allow_tail=False):
    """Independent Python decoder: validate CRCs, lengths, sequences, and payload bytes."""
    data = Path(path).read_bytes()
    assert data[:8] == b'FLINWAL\0' and len(data) >= 24
    assert struct.unpack_from('!HHII', data, 8) == (1, 24, 0, 0)
    assert zlib.crc32(data[:20]) == struct.unpack_from('!I', data, 20)[0]
    records, offset = [], 24
    while offset < len(data):
        if len(data) - offset < 32:
            break
        header = data[offset:offset + 32]
        magic, version, kind, size, flags, sequence, payload_crc, header_crc = struct.unpack('!IHHIIQII', header)
        assert (magic, version, flags, sequence) == (0x464c5752, 1, 0, len(records) + 1)
        assert zlib.crc32(header[:28]) == header_crc
        assert 1 <= kind <= 7 and 4 <= size <= 2132
        if len(data) - offset < 32 + size:
            break
        payload = data[offset + 32:offset + 32 + size]
        assert zlib.crc32(payload) == payload_crc
        record = dict(kind=kind, sequence=sequence)
        if kind == 1:
            record['worker'], = struct.unpack('!I', payload)
        else:
            values = struct.unpack_from('!QHHIQIIQQQQQHHII', payload)
            keys = ('job', 'task', 'state', 'worker', 'attempt', 'retry', 'max_retries',
                    'created', 'updated', 'assigned', 'started', 'finished', 'failure',
                    'reserved', 'argument_size', 'result_size')
            record.update(zip(keys, values))
            args_end = 84 + record['argument_size']
            assert len(payload) == args_end + record['result_size']
            record['arguments'], record['result'] = payload[84:args_end], payload[args_end:]
        records.append(record)
        offset += 32 + size
    if not allow_tail:
        assert offset == len(data), f'Incomplete tail of {len(data) - offset} bytes'
    return records, len(data) - offset


class Server:
    def __init__(self, executable, directory, path, initialize=True, limit=None, default_path=False, extra=(), port=None):
        if port is None:
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                port = reservation.getsockname()[1]
        self.port = port
        command = [str(executable), '--port', str(self.port)]
        if not default_path:
            command += ['--wal', str(path)]
        if initialize:
            command += ['--init-wal']
        command += list(extra)
        if limit is not None:
            # Set the limit in a single-threaded child, then exec the real binary.
            script = ('import os,resource,signal,sys; '
                      'signal.signal(signal.SIGXFSZ, signal.SIG_IGN); '
                      'n=int(sys.argv[1]); resource.setrlimit(resource.RLIMIT_FSIZE,(n,n)); '
                      'os.execv(sys.argv[2],sys.argv[2:])')
            command = [sys.executable, '-c', script, str(limit), *command]
        # Pipes keep the process's file-size limit from interfering with diagnostics.
        self.process = subprocess.Popen(command, cwd=directory, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True)
        self.lines = []
        self.reader = threading.Thread(target=self.drain, daemon=True)
        self.reader.start()

    def drain(self):
        self.lines.extend(self.process.stdout)

    @property
    def output(self):
        return ''.join(self.lines)

    def ready(self):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.reader.join(1)
                raise AssertionError(f'Coordinator exited: {self.output}')
            if 'coordinator listening ' in self.output:
                return self
            time.sleep(.01)
        raise AssertionError(f'Coordinator not ready: {self.output}')

    def wait(self, code):
        actual = self.process.wait(timeout=8)
        self.reader.join(2)
        assert actual == code, (actual, code, self.output)
        assert 'AddressSanitizer' not in self.output and 'runtime error:' not in self.output, self.output

    def stop(self, crash=False):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
            self.wait(-signal.SIGKILL if crash else 0)

    def close(self):
        if self.process.poll() is None:
            self.process.kill()
        self.process.wait(timeout=8)
        self.reader.join(2)
        self.process.stdout.close()

    def connect(self):
        return socket.create_connection(('127.0.0.1', self.port), timeout=3)

    def event(self, text):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if text in self.output:
                return
            if self.process.poll() is not None:
                break
            time.sleep(.01)
        raise AssertionError(f'Missing {text}: {self.output}')


class PersistenceTestCase(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='faultline-persistence-')
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.path = self.directory / 'state.wal'
        self.executable = (BIN_DIR / 'faultline-coordinator').resolve()

    def server(self, **options):
        server = Server(self.executable, self.directory, options.pop('path', self.path), **options)
        self.addCleanup(server.close)
        return server

    def connect(self, server):
        connection = server.connect()
        self.addCleanup(connection.close)
        return connection

    def register(self, connection):
        connection.sendall(test_ping.REGISTER)
        kind, payload = receive_frame(connection)
        self.assertEqual(kind, 4)
        return struct.unpack('!I', payload)[0]

    def submit(self, server, arguments=b'abc', retries=1):
        with server.connect() as connection:
            connection.sendall(submit(arguments, retries))
            kind, payload = receive_frame(connection)
            self.assertEqual(kind, 7)
            return struct.unpack('!Q', payload)[0]

    def assigned(self, connection, job, worker, attempt=1, arguments=b'abc'):
        kind, payload = receive_frame(connection)
        self.assertEqual((kind, payload), (8, identity(job, worker, attempt) + struct.pack('!HI', 4, len(arguments)) + arguments))

    def complete(self, connection, job, worker, result=b'OK', attempt=1):
        ref = identity(job, worker, attempt)
        connection.sendall(frame(9, ref) + frame(10, ref + struct.pack('!I', len(result)) + result))

    def closed(self, connection):
        try:
            self.assertEqual(connection.recv(1), b'')
        except ConnectionResetError:
            pass

    def latest(self):
        return {record['job']: record for record in read_wal(self.path)[0] if record['kind'] != 1}


class PersistenceTests(PersistenceTestCase):
    def test_ack_assignment_and_binary_result_survive_sigkill(self):
        server = self.server().ready()
        arguments = b'a\0b'
        self.assertEqual(self.submit(server, arguments), 1)
        records, _ = read_wal(self.path)
        self.assertEqual((records[-1]['kind'], records[-1]['arguments']), (2, arguments))
        server.stop(crash=True)
        server = self.server(initialize=False).ready()
        worker = self.connect(server)
        worker_id = self.register(worker)
        self.assigned(worker, 1, worker_id, arguments=arguments)
        self.assertEqual(self.latest()[1]['state'], 2)  # Assignment exists before its bytes arrive.
        self.assertEqual(self.submit(server, b'next', retries=0), 2)
        result = bytes(range(256)) * 4
        self.complete(worker, 1, worker_id, result)
        self.assigned(worker, 2, worker_id, arguments=b'next')
        records, _ = read_wal(self.path)
        completed = next(r for r in records if r['kind'] == 5)
        assigned_next = records[-1]
        self.assertEqual(completed['result'], result)
        self.assertLess(completed['sequence'], assigned_next['sequence'])
        self.assertEqual((assigned_next['job'], assigned_next['kind']), (2, 3))
        server.stop(crash=True)
        server = self.server(initialize=False).ready()
        self.assertEqual(self.latest()[1]['result'], result)
        self.assertEqual(self.latest()[1]['state'], 4)
        self.assertEqual(self.latest()[2]['state'], 5)
        replacement = self.connect(server)
        self.assertEqual(self.register(replacement), worker_id + 1)
        self.assertEqual(self.submit(server, b'third'), 3)
        self.assigned(replacement, 3, worker_id + 1, arguments=b'third')
        server.stop()

    def test_startup_retries_keep_pending_fifo_and_do_not_charge_twice(self):
        server = self.server().ready()
        first, second = self.connect(server), self.connect(server)
        a, b = self.register(first), self.register(second)
        self.assertEqual(self.submit(server), 1)
        self.assigned(first, 1, a)
        first.sendall(frame(9, identity(1, a)))
        server.event('job_started job_id=1')
        self.assertEqual(self.submit(server, retries=0), 2)
        self.assigned(second, 2, b)
        self.assertEqual(self.submit(server), 3)
        server.stop(crash=True)
        server = self.server(initialize=False).ready()
        latest = self.latest()
        self.assertEqual((latest[1]['state'], latest[1]['retry'], latest[1]['attempt']), (1, 1, 1))
        self.assertEqual((latest[2]['state'], latest[2]['retry'], latest[2]['failure']), (5, 0, 2))
        self.assertEqual([r['job'] for r in read_wal(self.path)[0][-2:]], [1, 2])
        before = self.path.read_bytes()
        server.stop(crash=True)
        server = self.server(initialize=False).ready()
        self.assertEqual(self.path.read_bytes(), before)
        worker = self.connect(server)
        c = self.register(worker)
        self.assertEqual(c, 3)
        self.assigned(worker, 3, c)
        self.complete(worker, 3, c)
        self.assigned(worker, 1, c, attempt=2)
        self.complete(worker, 1, c, attempt=2)
        server.event('job_completed job_id=1')
        self.assertEqual(self.latest()[1]['retry'], 1)
        server.stop()

    def test_write_failure_never_exposes_success_or_appends_cleanup(self):
        # Header 24; worker allocation 36; each job snapshot with abc is 119.
        cases = [('register', 24), ('submit', 60), ('assign', 179), ('start', 298),
                 ('complete', 417), ('task_retry', 417), ('task_failed', 417),
                 ('disconnect', 417), ('heartbeat', 417)]
        for action, prefix in cases:
            with self.subTest(action=action):
                path = self.directory / (action + '.wal')
                server = self.server(path=path, limit=prefix + 7,
                                     extra=('--heartbeat-timeout-ms', '700') if action == 'heartbeat' else ()).ready()
                worker = self.connect(server)
                worker.sendall(test_ping.REGISTER)
                if action != 'register':
                    self.assertEqual(receive_frame(worker), (4, struct.pack('!I', 1)))
                    client = self.connect(server)
                    client.sendall(submit(retries=0 if action == 'task_failed' else 1))
                    # Assignment failure can stop the loop before an already
                    # durable submission's queued ACK reaches the client.
                    if action not in ('submit', 'assign'):
                        self.assertEqual(receive_frame(client), (7, struct.pack('!Q', 1)))
                        self.assigned(worker, 1, 1)
                        worker.sendall(frame(9, identity(1, 1)))
                        if action != 'start':
                            server.event('job_started job_id=1')
                            if action == 'complete':
                                worker.sendall(frame(10, identity(1, 1) + struct.pack('!I', 2) + b'OK'))
                            elif action.startswith('task_'):
                                worker.sendall(frame(11, identity(1, 1) + struct.pack('!H', 1)))
                            elif action == 'disconnect':
                                worker.close()
                server.wait(1)
                self.assertIn('persistence_failed', server.output)
                self.assertNotIn('job_worker_lost', server.output)
                forbidden = {'register': 'worker_registered', 'submit': 'job_submitted',
                             'assign': 'job_assigned', 'start': 'job_started', 'complete': 'job_completed',
                             'task_retry': 'job_failed', 'task_failed': 'job_failed'}
                if action in forbidden:
                    self.assertNotIn(forbidden[action], server.output)
                if action != 'disconnect':
                    self.closed(worker)
                if action == 'submit':
                    self.closed(client)
                self.assertEqual(path.stat().st_size, prefix + 7)
                records, tail = read_wal(path, allow_tail=True)
                self.assertEqual(tail, 7)
                recovered = self.server(path=path, initialize=False).ready()
                self.assertIn('repaired_bytes=7', recovered.output)
                read_wal(path)
                recovered.stop()

    def test_reconciliation_write_failure_refuses_readiness(self):
        server = self.server().ready()
        worker = self.connect(server)
        a = self.register(worker)
        self.submit(server)
        self.assigned(worker, 1, a)
        server.stop(crash=True)
        size = self.path.stat().st_size
        failed = self.server(initialize=False, limit=size + 7)
        failed.wait(1)
        self.assertNotIn('coordinator listening', failed.output)
        self.assertEqual(self.path.stat().st_size, size + 7)
        server = self.server(initialize=False).ready()
        self.assertEqual((self.latest()[1]['state'], self.latest()[1]['retry']), (1, 1))
        server.stop()

    def test_missing_existing_locked_and_corrupt_files_refuse_startup(self):
        missing = self.server(initialize=False)
        missing.wait(1)
        self.assertFalse(self.path.exists())
        server = self.server().ready()
        for initialize in (True, False):
            other = self.server(initialize=initialize)
            other.wait(1)
            self.assertNotIn('coordinator listening', other.output)
        self.submit(server)
        server.stop(crash=True)
        with self.path.open('ab') as stream:
            stream.write(b'FLW')
        server = self.server(initialize=False).ready()
        self.assertIn('repaired_bytes=3', server.output)
        server.stop()
        damaged = bytearray(self.path.read_bytes())
        damaged[-1] ^= 1
        self.path.write_bytes(damaged)
        failed = self.server(initialize=False)
        failed.wait(1)
        self.assertNotIn('coordinator listening', failed.output)
        self.assertEqual(self.path.read_bytes(), damaged)

    def test_idle_worker_ids_and_graceful_interruption_are_durable(self):
        server = self.server().ready()
        idle = self.connect(server)
        self.assertEqual(self.register(idle), 1)
        server.stop()
        server = self.server(initialize=False).ready()
        worker = self.connect(server)
        a = self.register(worker)
        self.assertEqual(a, 2)
        self.submit(server)
        self.assigned(worker, 1, a)
        worker.sendall(frame(9, identity(1, a)))
        server.event('job_started job_id=1')
        before = self.path.read_bytes()
        server.stop()
        self.assertEqual(self.path.read_bytes(), before)  # No shutdown loss records.
        server = self.server(initialize=False).ready()
        self.assertEqual((self.latest()[1]['state'], self.latest()[1]['retry']), (1, 1))
        server.stop()

    def test_default_path_and_invalid_options(self):
        default = self.directory / 'faultline.wal'
        server = self.server(path=default, default_path=True).ready()
        server.stop()
        self.assertTrue(default.exists())
        server = self.server(path=default, initialize=False, default_path=True).ready()
        server.stop()
        for options in (['--wal'], ['--wal', ''], ['--wal', 'a', '--wal', 'b'],
                        ['--init-wal', '--init-wal'], ['--init-wal', 'extra']):
            result = subprocess.run([str(self.executable), *options], cwd=self.directory,
                                    capture_output=True, text=True, timeout=3)
            self.assertEqual(result.returncode, 1)
            self.assertIn('Usage:', result.stderr)
        result = subprocess.run([str(self.executable), '--help'], cwd=self.directory,
                                capture_output=True, text=True, timeout=3)
        self.assertEqual(result.returncode, 0)
        self.assertIn('--init-wal', result.stdout)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=BIN_DIR)
    args, remaining = parser.parse_known_args()
    BIN_DIR = args.bin_dir
    unittest.main(argv=[__file__, *remaining], verbosity=2)
