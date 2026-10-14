"""Durable keyed admission, lost ACKs, conflicts, crash boundaries, and legacy use."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import threading
import unittest
import zlib

import test_persistence as persistence
import test_ping
from test_scheduling import frame, identity, receive_frame


KEY = bytes(range(16))
POINTS = ('before_write', 'partial_header', 'partial_payload', 'before_sync', 'after_sync')


def keyed(key=KEY, arguments=b'a\0\xff', retries=1, task=4):
    return frame(21, key + struct.pack('!HII', task, retries, len(arguments)) + arguments)


class DeduplicationTests(persistence.PersistenceTestCase):
    def cli(self, server, *arguments, expected=0):
        result = subprocess.run([str((persistence.BIN_DIR / 'faultline').resolve()), *arguments,
                                 '--coordinator', f'127.0.0.1:{server.port}'],
                                cwd=self.directory, capture_output=True, text=True, timeout=8)
        self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
        if expected == 0:
            self.assertEqual(result.stderr, '')
        return result

    def admission(self, server, message=None):
        with server.connect() as connection:
            connection.sendall(keyed() if message is None else message)
            kind, payload = receive_frame(connection)
            self.assertEqual(kind, 7)
            return struct.unpack('!Q', payload)[0]

    def unchanged_retry(self, server, job=1, message=None):
        before = self.path.read_bytes()
        status = self.cli(server, 'status', str(job)).stdout
        stats = self.cli(server, 'stats').stdout
        self.assertEqual(self.admission(server, message), job)
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(self.cli(server, 'status', str(job)).stdout, status)
        # Session uptime and worker heartbeat ages can advance independently.
        before_fields = dict(line.split('=', 1) for line in stats.splitlines())
        after_fields = dict(line.split('=', 1) for line in self.cli(server, 'stats').stdout.splitlines())
        for name in ('jobs_submitted_total', 'session_jobs_submitted', 'job_attempts_total', 'job_retries_total'):
            self.assertEqual(before_fields[name], after_fields[name])

    def test_fragmented_coalesced_and_concurrent_requests_share_one_id(self):
        server = self.server().ready()
        # Race first admission too, before any mapping has been created.
        with ThreadPoolExecutor(max_workers=8) as pool:
            self.assertEqual(list(pool.map(lambda _: self.admission(server), range(24))), [1] * 24)
        with server.connect() as connection:
            message = keyed()
            for cut in (1, 11, 12, 13, 27, 37, len(message) - 1):
                connection.sendall(message[:cut])
                self.assertEqual(select.select([connection], [], [], .02)[0], [])
                connection.sendall(message[cut:])
                self.assertEqual(receive_frame(connection), (7, struct.pack('!Q', 1)))
            before = self.path.read_bytes()
            connection.sendall(message + message + test_ping.PING)
            self.assertEqual(receive_frame(connection), (7, struct.pack('!Q', 1)))
            self.assertEqual(receive_frame(connection), (7, struct.pack('!Q', 1)))
            self.assertEqual(receive_frame(connection), (2, b''))
            self.assertEqual(self.path.read_bytes(), before)
        with ThreadPoolExecutor(max_workers=8) as pool:
            self.assertEqual(list(pool.map(lambda _: self.admission(server), range(24))), [1] * 24)
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(len(persistence.read_wal(self.path)[0]), 1)
        server.stop()

    def test_keyed_empty_and_maximum_binary_arguments_survive_restart(self):
        server = self.server().ready()
        requests = (keyed(arguments=b''), keyed(key=b'\xff' * 16, arguments=bytes(range(256)) * 4))
        for job, request in enumerate(requests, 1):
            self.assertEqual(self.admission(server, request), job)
        server.stop(crash=True)
        server = self.server(initialize=False).ready()
        for job, request in enumerate(requests, 1):
            self.unchanged_retry(server, job, request)
        server.stop()

    def test_conflicts_compare_every_definition_field_and_preserve_connection(self):
        server = self.server().ready()
        self.assertEqual(self.admission(server), 1)
        before = self.path.read_bytes()
        with server.connect() as connection:
            for message in (keyed(task=1), keyed(arguments=b'a\0\xfe'),
                            keyed(arguments=b'a\0'), keyed(retries=2)):
                connection.sendall(message + keyed())
                self.assertEqual(receive_frame(connection), (22, KEY))
                self.assertEqual(receive_frame(connection), (7, struct.pack('!Q', 1)))
        self.assertEqual(self.path.read_bytes(), before)
        rejected = self.cli(server, 'submit', 'hash', '--args', 'different',
                            '--request-key', KEY.hex(), expected=3)
        self.assertEqual(rejected.stdout, '')
        self.assertIn('request key conflict', rejected.stderr)
        self.assertNotIn('unconfirmed', rejected.stderr)
        self.assertEqual(self.path.read_bytes(), before)
        server.stop()

    def test_cli_key_validation_case_normalization_and_legacy_admission(self):
        server = self.server().ready()
        for key in ('', '0' * 32, '1' * 31, '1' * 33, 'g' * 32, ' ' * 32):
            result = self.cli(server, 'submit', 'hash', '--request-key', key, expected=1)
            self.assertIn('Usage:', result.stderr)
        self.cli(server, 'submit', 'hash', '--request-key', KEY.hex(),
                 '--request-key', KEY.hex(), expected=1)
        self.assertEqual(len(persistence.read_wal(self.path)[0]), 0)
        for key in (KEY.hex(), KEY.hex().upper()):
            result = self.cli(server, 'submit', 'hash', '--args-hex', '6100ff',
                              '--max-retries', '1', '--request-key', key)
            self.assertEqual(result.stdout, 'job_id=1\n')
        self.assertEqual(self.admission(server, keyed(key=b'\xff' * 16)), 2)
        self.assertEqual(self.submit(server, arguments=b'a\0\xff'), 3)
        self.assertEqual(self.submit(server, arguments=b'a\0\xff'), 4)
        server.stop()
        server = self.server(initialize=False).ready()
        self.unchanged_retry(server)
        self.assertEqual(self.submit(server), 5)
        server.stop()

    def test_duplicates_at_capacity_and_after_restart(self):
        server = self.server().ready()
        self.assertEqual(self.admission(server), 1)
        for expected in range(2, 257):
            self.assertEqual(self.submit(server), expected)
        for restart in (False, True):
            if restart:
                server.stop()
                server = self.server(initialize=False).ready()
            self.unchanged_retry(server)
            before = self.path.read_bytes()
            with server.connect() as connection:
                connection.sendall(keyed(arguments=b'conflict'))
                self.assertEqual(receive_frame(connection), (22, KEY))
                connection.sendall(keyed(key=b'\xff' * 16))
                self.closed(connection)
            self.assertEqual(self.path.read_bytes(), before)
        server.stop()

    def test_retries_preserve_queued_assigned_running_done_and_failed_jobs(self):
        server = self.server().ready()
        self.assertEqual(self.admission(server), 1)
        self.unchanged_retry(server)
        worker = self.connect(server)
        self.assertEqual(self.register(worker), 1)
        self.assigned(worker, 1, 1, arguments=b'a\0\xff')
        self.unchanged_retry(server)
        worker.sendall(frame(9, identity(1, 1)))
        server.event('job_started job_id=1 ')
        self.unchanged_retry(server)
        worker.sendall(frame(10, identity(1, 1) + struct.pack('!I', 3) + b'\xff\0x'))
        server.event('job_completed job_id=1 ')
        self.unchanged_retry(server)
        second = keyed(key=b'\xfe' * 16, retries=0)
        self.assertEqual(self.admission(server, second), 2)
        self.assigned(worker, 2, 1, arguments=b'a\0\xff')
        worker.sendall(frame(11, identity(2, 1) + struct.pack('!H', 1)))
        server.event('job_failed job_id=2 ')
        self.unchanged_retry(server, 2, second)
        server.stop(crash=True)
        reused = [line for line in server.output.splitlines() if ' job_submission_reused ' in line]
        self.assertEqual(len(reused), 5)
        for line, state in zip(reused, ('QUEUED', 'ASSIGNED', 'RUNNING', 'DONE', 'FAILED')):
            self.assertIn(f'state={state} ', line)
            self.assertIn('outcome=EXISTING ', line)
        self.assertEqual(server.output.count(' job_submitted '), 2)
        self.assertEqual(server.output.count(' job_completed '), 1)
        self.assertEqual(server.output.count(' job_failed '), 1)
        server = self.server(initialize=False).ready()
        self.unchanged_retry(server)
        self.unchanged_retry(server, 2, second)
        server.stop()

    def test_interrupted_attempt_keeps_key_retry_budget_and_fifo_order(self):
        server = self.server().ready()
        first = self.connect(server)
        self.assertEqual(self.register(first), 1)
        self.assertEqual(self.admission(server), 1)
        self.assigned(first, 1, 1, arguments=b'a\0\xff')
        first.sendall(frame(9, identity(1, 1)))
        server.event('job_started job_id=1 ')
        self.assertEqual(self.submit(server), 2)
        server.stop(crash=True)
        server = self.server(initialize=False).ready()
        self.unchanged_retry(server)
        self.assertIn('retries=1/1\n', self.cli(server, 'status', '1').stdout)
        replacement = self.connect(server)
        self.assertEqual(self.register(replacement), 2)
        self.assigned(replacement, 2, 2)  # The duplicate did not move job 1 ahead.
        self.complete(replacement, 2, 2)
        self.assigned(replacement, 1, 2, attempt=2, arguments=b'a\0\xff')
        replacement.sendall(frame(11, identity(1, 2, 2) + struct.pack('!H', 1)))
        server.event('job_failed job_id=1 ')
        self.unchanged_retry(server)
        self.assertIn('state=FAILED\n', self.cli(server, 'status', '1').stdout)
        server.stop()

    def test_crashes_at_each_keyed_creation_boundary_never_separate_job_and_key(self):
        for point in POINTS:
            with self.subTest(point=point):
                self.path = self.directory / f'{point}.wal'
                server = self.server().ready()
                self.assertEqual(self.submit(server), 1)  # Legacy prefix remains usable.
                server.stop()
                environment = dict(os.environ, FAULTLINE_TEST_WAL_KIND='8',
                                   FAULTLINE_TEST_WAL_POINT=point, FAULTLINE_TEST_WAL_OCCURRENCE='1')
                server = persistence.Server((persistence.BIN_DIR / 'tests/crash-coordinator').resolve(),
                                            self.directory, self.path, initialize=False, env=environment)
                self.addCleanup(server.close)
                server.ready()
                connection = self.connect(server)
                connection.sendall(keyed())
                server.event(f'[TEST] crash_boundary kind=8 point={point} occurrence=1\n')
                self.assertEqual(select.select([connection], [], [], 0)[0], [])
                self.assertNotIn('job_submitted job_id=2 ', server.output)
                records, tail = persistence.read_wal(self.path, allow_tail=True)
                complete = point in ('before_sync', 'after_sync')
                self.assertEqual(len(records), 2 if complete else 1)
                server.stop(crash=True)
                server = self.server(initialize=False).ready()
                self.assertIn(f'repaired_bytes={tail}', server.output)
                before = self.path.read_bytes()
                self.assertEqual(self.admission(server), 2)
                if complete:
                    self.assertEqual(self.path.read_bytes(), before)
                self.unchanged_retry(server, 2)
                records, _ = persistence.read_wal(self.path)
                self.assertEqual([(r['job'], r['request_key']) for r in records if r['kind'] == 8], [(2, KEY)])
                self.assertEqual(self.admission(server, keyed(key=b'\xff' * 16)), 3)
                server.stop()
                server = self.server(initialize=False).ready()
                self.unchanged_retry(server, 2)
                server.stop()

    def test_invalid_or_duplicate_persisted_keys_fail_without_repair(self):
        server = self.server().ready()
        self.admission(server)
        self.admission(server, keyed(key=b'\xff' * 16))
        server.stop()
        original = self.path.read_bytes()
        first_size = struct.unpack_from('!I', original, 24 + 8)[0]
        offset = 24 + 32 + first_size
        for key in (KEY, bytes(16)):
            with self.subTest(key=key.hex()):
                data = bytearray(original)
                data[offset + 32:offset + 48] = key
                struct.pack_into('!I', data, offset + 24, zlib.crc32(data[offset + 32:]))
                struct.pack_into('!I', data, offset + 28, zlib.crc32(data[offset:offset + 28]))
                self.path.write_bytes(data)
                rejected = self.server(initialize=False)
                rejected.wait(1)
                self.assertNotIn(' listening ', rejected.output)
                self.assertEqual(self.path.read_bytes(), data)

    def test_invalid_wire_keys_lengths_and_worker_role_are_rejected(self):
        server = self.server().ready()
        for message in (keyed(key=bytes(16)), frame(21, b''), frame(21, KEY + b'\0' * 9),
                        frame(21, KEY + struct.pack('!HII', 4, 1, 1025)),
                        frame(21, KEY + struct.pack('!HII', 4, 1, 0) + b'x')):
            with server.connect() as connection:
                connection.sendall(message)
                self.closed(connection)
        worker = self.connect(server)
        self.register(worker)
        worker.sendall(keyed()[:12])
        self.closed(worker)
        self.assertEqual(self.admission(server), 1)
        with server.connect() as connection:
            connection.sendall(keyed())
            self.assertEqual(receive_frame(connection), (7, struct.pack('!Q', 1)))
            connection.sendall(test_ping.REGISTER)
            self.closed(connection)
        server.stop()

    def test_cli_lost_ack_retains_key_and_retry_recovers_original_id(self):
        server = self.server().ready()
        errors = []
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(5)
            def proxy():
                try:
                    with listener.accept()[0] as client, server.connect() as upstream:
                        client.settimeout(5)
                        kind, payload = receive_frame(client)
                        self.assertEqual(kind, 21)
                        upstream.sendall(frame(kind, payload))
                        self.assertEqual(receive_frame(upstream), (7, struct.pack('!Q', 1)))
                        # The job is durable; deliberately lose the ACK to the CLI.
                except BaseException as error:
                    errors.append(error)
            thread = threading.Thread(target=proxy, daemon=True)
            thread.start()
            proxy_endpoint = type('Endpoint', (), {'port': listener.getsockname()[1]})()
            try:
                result = self.cli(proxy_endpoint, 'submit', 'hash', '--args-hex', '6100ff',
                                  '--max-retries', '1', '--request-key', KEY.hex(), expected=1)
            finally:
                thread.join(timeout=6)
            self.assertFalse(thread.is_alive())
            if errors:
                raise errors[0]
        self.assertEqual(result.stdout, '')
        self.assertIn('submission unconfirmed', result.stderr)
        self.assertIn('--request-key ' + KEY.hex(), result.stderr)
        server.stop(crash=True)
        server = self.server(initialize=False).ready()
        before = self.path.read_bytes()
        result = self.cli(server, 'submit', 'hash', '--args-hex', '6100ff',
                          '--max-retries', '1', '--request-key', KEY.hex())
        self.assertEqual(result.stdout, 'job_id=1\n')
        self.assertEqual(self.path.read_bytes(), before)
        server.stop()

    def test_cli_rejects_wrong_conflict_key_and_never_downgrades(self):
        for response in (frame(22, b'\xff' * 16), b''):
            with self.subTest(response=response), socket.socket() as listener:
                listener.bind(('127.0.0.1', 0))
                listener.listen(1)
                listener.settimeout(5)
                errors = []
                def peer():
                    try:
                        with listener.accept()[0] as client:
                            client.settimeout(5)
                            kind, payload = receive_frame(client)
                            self.assertEqual((kind, payload[:16]), (21, KEY))
                            if response:
                                client.sendall(response)
                            client.shutdown(socket.SHUT_WR)
                            # No unkeyed fallback on this connection.
                            self.assertEqual(client.recv(1), b'')
                    except BaseException as error:
                        errors.append(error)
                thread = threading.Thread(target=peer, daemon=True)
                thread.start()
                endpoint = type('Endpoint', (), {'port': listener.getsockname()[1]})()
                try:
                    result = self.cli(endpoint, 'submit', 'hash', '--request-key', KEY.hex(), expected=1)
                finally:
                    thread.join(timeout=6)
                self.assertFalse(thread.is_alive())
                if errors:
                    raise errors[0]
                # No fallback by opening another connection either.
                self.assertEqual(select.select([listener], [], [], 0)[0], [])
                self.assertEqual(result.stdout, '')
                self.assertIn('submission unconfirmed', result.stderr)
                self.assertIn('--request-key ' + KEY.hex(), result.stderr)
                if response:
                    self.assertIn('wrong request key', result.stderr)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=persistence.BIN_DIR)
    args, remaining = parser.parse_known_args()
    persistence.BIN_DIR = args.bin_dir.resolve()
    unittest.main(argv=[__file__, *remaining], verbosity=2)
