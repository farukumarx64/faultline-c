"""Real built-in execution, terminal reports, cancellation, and worker reuse."""

import argparse
from pathlib import Path
import re
import signal
import struct
import time
import unittest

import test_ping
from test_worker import read_output
from test_scheduling import JobProcessTestCase, assignment, identity, receive_frame


class ExecutionTests(JobProcessTestCase):
    COORDINATOR_ARGUMENTS = ('--heartbeat-timeout-ms', '500')

    def wait_completed(self, job_id, expected, timeout=5):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            match = re.search(rf'job_completed job_id={job_id}\b([^\n]*)', read_output(self.log))
            if match:
                fields = dict(re.findall(r'(\w+)=("[^"]*"|\S+)', match[1]))
                self.assertEqual(fields['state'], 'DONE')
                self.assertEqual(fields['result_bytes'], str(len(expected)))
                self.assertEqual(fields['result'], f'"{expected}"')
                return match[1]
            time.sleep(0.01)
        self.fail(f'Job {job_id} did not complete: {read_output(self.log)}')

    def reports_until_terminal(self, connection, worker_id):
        reports = []
        while True:
            kind, payload = receive_frame(connection)
            if kind == 5:
                self.assertEqual(payload, struct.pack('!I', worker_id))
                continue
            reports.append((kind, payload))
            if kind in (10, 11):
                return reports

    def test_sleep_completes_and_worker_takes_next_job(self):
        first = self.accepted_id(self.submit_cli('sleep', '--args', '650'))
        second = self.accepted_id(self.submit_cli('sleep', '--args', '0'))
        with self.worker_process(interval_ms=80) as (worker, output, errors):
            worker_id = self.wait_for_registration(worker, output, errors)
            self.wait_job_event('job_started', first)
            start = time.monotonic()
            self.assertNotIn(f'job_assigned job_id={second}', read_output(self.log))
            self.wait_completed(first, 'slept_ms=650')
            self.assertGreater(time.monotonic() - start, 0.5)
            self.assertGreaterEqual(len(self.worker_events('heartbeat_received', worker_id)), 4)
            event = self.wait_completed(second, 'slept_ms=0')
            self.assertIn(f'worker_id={worker_id}', event)
            log = read_output(self.log)
            self.assertLess(log.index(f'job_completed job_id={first}'), log.index(f'job_assigned job_id={second}'))
            self.stop_worker(worker, errors)

    def test_all_task_results_through_cli_and_real_worker(self):
        cases = [('prime_count', ['--args', '100'], '25'),
                 ('fibonacci', ['--args', '93'], '12200160415121876738'),
                 ('hash', ['--args', 'hello'], 'a430d84680aabd0b'),
                 ('hash', [], 'cbf29ce484222325'),
                 ('hash', ['--args-hex', '00'], 'af63bd4c8601b7df')]
        with self.worker_process(interval_ms=80) as (worker, output, errors):
            worker_id = self.wait_for_registration(worker, output, errors)
            for task, arguments, expected in cases:
                with self.subTest(task=task, arguments=arguments):
                    job_id = self.accepted_id(self.submit_cli(task, *arguments))
                    event = self.wait_completed(job_id, expected)
                    self.assertIn(f'worker_id={worker_id}', event)
                    self.assertEqual(self.wait_job_event('job_started', job_id)['state'], 'RUNNING')
            self.stop_worker(worker, errors)

    def test_literal_reports_preserve_identity_and_maximum_binary_input(self):
        # Independent peer inspects actual worker frames, including a 64-bit attempt.
        worker_id, job_id, attempt = 0xffffffff, 0xffffffffffffffff, 0x100000001
        arguments = bytes(range(256)) * 4
        reference = identity(job_id, worker_id, attempt)
        with self.fake_coordinator(interval_ms=30) as (connection, worker, output, errors):
            connection.sendall(test_ping.REGISTER_ACK_HEADER + struct.pack('!I', worker_id))
            request = assignment(job_id, worker_id, arguments, attempt=attempt)
            offset = 0
            for end in (1, 11, 38, len(request) - 1):
                connection.sendall(request[offset:end])
                offset = end
                self.assertEqual(receive_frame(connection), (5, struct.pack('!I', worker_id)))
                self.assertNotIn('job_assigned', read_output(output))
            connection.sendall(request[offset:])
            reports = self.reports_until_terminal(connection, worker_id)
            # Known checksum of bytes 0..255 repeated four times.
            expected = b'1e5698f9d66e6f25'
            self.assertEqual(reports, [(9, reference),
                (10, reference + struct.pack('!I', len(expected)) + expected)])
            self.stop_worker(worker, errors)

    def test_invalid_task_inputs_fail_without_losing_worker(self):
        with self.worker_process(interval_ms=80) as (worker, output, errors):
            worker_id = self.wait_for_registration(worker, output, errors)
            cases = [('sleep', []), ('sleep', ['--args', '-1']),
                     ('sleep', ['--args', '86400001']), ('prime_count', ['--args', '100000001']),
                     ('fibonacci', ['--args', '94']), ('fibonacci', ['--args-hex', '310032'])]
            for task, arguments in cases:
                job_id = self.accepted_id(self.submit_cli(task, *arguments))
                self.assertEqual(self.wait_job_event('job_failed', job_id)['state'], 'FAILED')
                self.assertNotIn(f'job_completed job_id={job_id}', read_output(self.log))
            valid = self.accepted_id(self.submit_cli('fibonacci', '--args', '10'))
            self.assertIn(f'worker_id={worker_id}', self.wait_completed(valid, '55'))
            self.assertEqual(self.worker_events('worker_dead', worker_id), [])
            self.stop_worker(worker, errors)

    def test_task_failure_retries_at_queue_tail_then_exhausts(self):
        invalid = self.accepted_id(self.submit_cli('fibonacci', '--args', '94', '--max-retries', '1'))
        valid = self.accepted_id(self.submit_cli('fibonacci', '--args', '10'))
        with self.worker_process(interval_ms=80) as (worker, output, errors):
            self.wait_for_registration(worker, output, errors)
            self.wait_completed(valid, '55')
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                failures = re.findall(rf'job_failed job_id={invalid}\b([^\n]*)', read_output(self.log))
                if len(failures) == 2:
                    break
                time.sleep(0.01)
            self.assertEqual(len(failures), 2)
            self.assertIn('state=QUEUED', failures[0])
            self.assertIn('state=FAILED', failures[1])
            self.assertIn('attempt=2 retry_count=1', failures[1])
            assigned = re.findall(r'coordinator job_assigned job_id=(\d+)', read_output(self.log))
            self.assertEqual(assigned, [str(invalid), str(valid), str(invalid)])
            self.stop_worker(worker, errors)

    def test_cpu_task_keeps_heartbeats_and_can_be_cancelled(self):
        job_id = self.accepted_id(self.submit_cli('prime_count', '--args', '100000000'))
        with self.worker_process(interval_ms=60) as (worker, output, errors):
            worker_id = self.wait_for_registration(worker, output, errors)
            self.wait_job_event('job_started', job_id)
            deadline = time.monotonic() + 2
            while len(self.worker_events('heartbeat_received', worker_id)) < 12 and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertGreaterEqual(len(self.worker_events('heartbeat_received', worker_id)), 12)
            self.assertNotIn(f'job_completed job_id={job_id}', read_output(self.log))
            self.assert_pong(self.run_cli())
            self.stop_worker(worker, errors)
            self.assertEqual(self.wait_job_event('job_worker_lost', job_id)['state'], 'FAILED')

    def test_two_workers_execute_concurrently_and_drain_queue(self):
        first = self.accepted_id(self.submit_cli('sleep', '--args', '1000'))
        second = self.accepted_id(self.submit_cli('sleep', '--args', '1000'))
        third = self.accepted_id(self.submit_cli('fibonacci', '--args', '10'))
        with self.worker_process(interval_ms=80) as (a, out_a, err_a), \
                self.worker_process(interval_ms=80) as (b, out_b, err_b):
            a_id = self.wait_for_registration(a, out_a, err_a)
            b_id = self.wait_for_registration(b, out_b, err_b)
            owners = {int(self.wait_job_event('job_started', job_id)['worker_id'])
                      for job_id in (first, second)}
            self.assertEqual(owners, {a_id, b_id})
            self.assertEqual(len(owners), 2)
            self.assertNotIn('job_completed', read_output(self.log))
            self.assertNotIn(f'job_assigned job_id={third}', read_output(self.log))
            self.wait_completed(first, 'slept_ms=1000')
            self.wait_completed(second, 'slept_ms=1000')
            self.wait_completed(third, '55')
            self.stop_worker(a, err_a)
            self.stop_worker(b, err_b)

    def test_terminated_running_sleep_retries_and_really_completes(self):
        job_id = self.accepted_id(self.submit_cli('sleep', '--args', '650', '--max-retries', '1'))
        with self.worker_process(interval_ms=80) as (first, first_output, first_errors):
            first_id = self.wait_for_registration(first, first_output, first_errors)
            self.wait_job_event('job_started', job_id)
            self.stop_worker(first, first_errors)
            self.assertEqual(self.wait_job_event('job_worker_lost', job_id)['state'], 'QUEUED')
            with self.worker_process(interval_ms=80) as (second, second_output, second_errors):
                second_id = self.wait_for_registration(second, second_output, second_errors)
                self.assertNotEqual(first_id, second_id)
                event = self.wait_completed(job_id, 'slept_ms=650')
                self.assertIn(f'worker_id={second_id} attempt=2 retry_count=1', event)
                self.stop_worker(second, second_errors)

    def test_disconnect_and_signals_cancel_long_running_tasks(self):
        for task, arguments in ((1, b'86400000'), (2, b'100000000')):
            for action in ('disconnect', signal.SIGINT, signal.SIGTERM):
                with self.subTest(task=task, action=action), self.fake_coordinator(interval_ms=50) as (connection, worker, output, errors):
                    connection.sendall(test_ping.REGISTER_ACK_HEADER + struct.pack('!I', 7) +
                                       assignment(42, 7, arguments, task=task))
                    while True:
                        kind, payload = receive_frame(connection)
                        if kind != 5:
                            self.assertEqual((kind, payload), (9, identity(42, 7)))
                            break
                    time.sleep(0.05)
                    if action == 'disconnect':
                        connection.close()
                        self.assertEqual(worker.wait(timeout=2), 1)
                    else:
                        self.stop_worker(worker, errors, action)
                    self.assertNotIn('job_completed_sent', read_output(output))
                    self.assertNotIn('job_failed_sent', read_output(output))

    def test_failed_report_has_task_reason_and_worker_can_continue(self):
        with self.fake_coordinator(interval_ms=50) as (connection, worker, output, errors):
            connection.sendall(test_ping.REGISTER_ACK_HEADER + struct.pack('!I', 7) +
                               assignment(42, 7, b'94', task=3))
            reference = identity(42, 7)
            self.assertEqual(self.reports_until_terminal(connection, 7),
                             [(9, reference), (11, reference + struct.pack('!H', 1))])
            connection.sendall(assignment(43, 7, b'10', task=3))
            reference = identity(43, 7)
            self.assertEqual(self.reports_until_terminal(connection, 7),
                             [(9, reference), (10, reference + struct.pack('!I', 2) + b'55')])
            self.stop_worker(worker, errors)

    def test_result_logging_escapes_control_bytes(self):
        job_id = self.accepted_id(self.submit_cli('hash'))
        with self.connect() as worker:
            worker_id = self.register_worker(worker)
            self.receive_assignment(worker, worker_id, job_id, b'')
            result = b'line\n"\\\x00\xff'
            self.complete(worker, job_id, worker_id, result=result)
            self.wait_job_event('job_completed', job_id)
            self.assertIn('result="line\\x0a\\x22\\x5c\\x00\\xff"', read_output(self.log))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=Path('build/debug'))
    parser.add_argument('--port', type=int, default=None)
    arguments = parser.parse_args()
    test_ping.BIN_DIR = arguments.bin_dir
    test_ping.TEST_PORT = arguments.port
    unittest.main(argv=[__file__], verbosity=2)
