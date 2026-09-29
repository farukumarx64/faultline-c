"""Recover real running jobs after worker failure, using the real CLI and workers."""

import argparse
from contextlib import ExitStack
import os
from pathlib import Path
import re
import signal
import time
import unittest

import test_ping
from test_scheduling import JobProcessTestCase
from test_worker import read_output


class RecoveryTestCase(JobProcessTestCase):
    def job_events(self, job_id):
        events = []
        for line in read_output(self.log).splitlines(keepends=True):
            # The coordinator may still be writing a line when we read its log.
            if not line.endswith('\n'):
                continue
            match = re.fullmatch(
                rf'\S+ \[(?:INFO|WARN)\] coordinator (job_(?:submitted|assigned|started|completed|failed|worker_lost)) job_id={job_id} ([^\n]*)\n', line)
            if match:
                fields = dict(re.findall(r'(\w+)=("[^"]*"|\S+)', match[2]))
                events.append({'event': match[1], **fields})
        return events

    def wait_transition(self, job_id, event, timeout=2, **fields):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for entry in self.job_events(job_id):
                if entry['event'] == event and all(entry.get(key) == str(value)
                                                 for key, value in fields.items()):
                    return entry
            self.assertIsNone(self.server.poll(), read_output(self.log))
            time.sleep(0.01)
        self.fail(f'Missing {event} for job {job_id} with {fields}:\n{read_output(self.log)}')

    def wait_next_heartbeat(self, worker_id, process):
        previous = len(self.worker_events('heartbeat_received', worker_id))
        deadline = time.monotonic() + 2
        while len(self.worker_events('heartbeat_received', worker_id)) <= previous:
            self.assertIsNone(process.poll(), 'Worker exited while awaiting its next heartbeat')
            self.assertLess(time.monotonic(), deadline, read_output(self.log))
            time.sleep(0.01)

    def assert_recovery_transitions(self, job_id, owner_id, survivor_id):
        transitions = [(entry['event'], entry['state'], entry['worker_id'],
                        entry['attempt'], entry['retry_count'])
                       for entry in self.job_events(job_id)]
        self.assertEqual(transitions, [
            ('job_submitted', 'QUEUED', '0', '0', '0'),
            ('job_assigned', 'ASSIGNED', str(owner_id), '1', '0'),
            ('job_started', 'RUNNING', str(owner_id), '1', '0'),
            ('job_worker_lost', 'QUEUED', '0', '1', '1'),
            ('job_assigned', 'ASSIGNED', str(survivor_id), '2', '1'),
            ('job_started', 'RUNNING', str(survivor_id), '2', '1'),
            ('job_completed', 'DONE', str(survivor_id), '2', '1'),
        ])

    def pause_worker(self, process):
        process.send_signal(signal.SIGSTOP)
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            child, status = os.waitpid(process.pid, os.WUNTRACED | os.WNOHANG)
            if child:
                if os.WIFEXITED(status) or os.WIFSIGNALED(status):
                    # Preserve an unexpected terminal status already consumed by waitpid.
                    process.returncode = os.waitstatus_to_exitcode(status)
                self.assertTrue(os.WIFSTOPPED(status), 'Worker exited instead of stopping')
                self.assertEqual(os.WSTOPSIG(status), signal.SIGSTOP)
                return
            time.sleep(0.01)
        self.fail('Worker did not enter the stopped state after SIGSTOP')


class HardCrashRecoveryTests(RecoveryTestCase):
    # Keep the six-second default so prompt disconnect detection cannot be
    # mistaken for an accelerated heartbeat timeout.
    def test_sigkill_busy_worker_recovers_on_connected_idle_worker(self):
        with self.worker_process(interval_ms=100) as first, \
                self.worker_process(interval_ms=100) as second:
            workers = {}
            for process, output, errors in (first, second):
                worker_id = self.wait_for_registration(process, output, errors)
                self.wait_for_worker_event('heartbeat_received', worker_id)
                workers[worker_id] = (process, output, errors)
            self.assertEqual(len(workers), 2)

            # Both workers are registered BEFORE submission. Discover the owner
            # from STARTED instead of assuming process launch/registration order.
            job_id = self.accepted_id(self.submit_cli('sleep', '--args', '3000',
                                                      '--max-retries', '1'))
            started = self.wait_transition(job_id, 'job_started', state='RUNNING',
                                           attempt=1, retry_count=0)
            owner_id = int(started['worker_id'])
            owner, owner_output, owner_errors = workers[owner_id]
            survivor_id, = workers.keys() - {owner_id}
            survivor, survivor_output, survivor_errors = workers[survivor_id]
            survivor_pid = survivor.pid
            self.assertNotIn('worker job_assigned', read_output(survivor_output))
            # Require activity after STARTED, rather than killing at registration.
            self.wait_next_heartbeat(owner_id, owner)
            self.assertFalse(any(event['event'] == 'job_completed'
                                 for event in self.job_events(job_id)))

            crashed_at = time.monotonic()
            owner.send_signal(signal.SIGKILL)
            self.assertEqual(owner.wait(timeout=2), -signal.SIGKILL, read_output(owner_errors))
            self.assertEqual(read_output(owner_errors), '')
            self.assertNotIn('job_completed_sent', read_output(owner_output))
            self.assertNotIn('job_failed_sent', read_output(owner_output))

            self.wait_transition(job_id, 'job_worker_lost', state='QUEUED', worker_id=0,
                                 attempt=1, retry_count=1, pending=1)
            dead = self.wait_for_worker_event('worker_dead', owner_id)
            self.assertEqual(dead['state'], 'DEAD')
            self.assertIn(dead['reason'], ('eof', 'recv_error', 'truncated_message'))
            self.assertEqual(self.worker_events('heartbeat_timeout', owner_id), [])
            self.wait_transition(job_id, 'job_started', state='RUNNING', worker_id=survivor_id,
                                 attempt=2, retry_count=1)
            recovery_seconds = time.monotonic() - crashed_at
            self.assertLess(recovery_seconds, 2.5)  # Comfortably below the 6 s heartbeat limit.
            self.assertIsNone(survivor.poll())
            self.assertEqual(survivor.pid, survivor_pid)
            self.assertEqual(len(self.worker_events('worker_registered', survivor_id)), 1)
            self.assert_pong(self.run_cli())

            self.wait_transition(job_id, 'job_completed', timeout=6, state='DONE',
                                 worker_id=survivor_id, attempt=2, retry_count=1,
                                 pending=0, result_bytes=13, result='"slept_ms=3000"')

            # The surviving process must also be free for a new, unrelated job.
            followup = self.accepted_id(self.submit_cli('fibonacci', '--args', '10'))
            self.assertNotEqual(followup, job_id)
            self.wait_transition(followup, 'job_completed', state='DONE', worker_id=survivor_id,
                                 attempt=1, retry_count=0, pending=0, result_bytes=2, result='"55"')
            self.wait_next_heartbeat(survivor_id, survivor)

            # One job ID, exactly one requeue, two assignments/starts, and one
            # accepted completion: no duplicate or missing transition can pass.
            self.assert_recovery_transitions(job_id, owner_id, survivor_id)
            self.assertEqual(len(self.worker_events('worker_dead', owner_id)), 1)
            self.assertEqual(self.worker_events('worker_dead', survivor_id), [])
            self.assert_pong(self.run_cli())
            print(f'job {job_id}: SIGKILL worker {owner_id}; connected worker {survivor_id} '
                  f'started attempt 2 within {recovery_seconds:.3f}s; '
                  f'result=slept_ms=3000; follow-up job {followup}=55', flush=True)
            self.stop_worker(survivor, survivor_errors)


class HeartbeatRecoveryTests(RecoveryTestCase):
    def test_paused_busy_worker_expires_and_connected_worker_completes(self):
        with self.worker_process(interval_ms=100) as first, \
                self.worker_process(interval_ms=100) as second:
            workers = {}
            for process, output, errors in (first, second):
                worker_id = self.wait_for_registration(process, output, errors)
                self.wait_for_worker_event('heartbeat_received', worker_id)
                workers[worker_id] = (process, output, errors)
            self.assertEqual(len(workers), 2)
            job_id = self.accepted_id(self.submit_cli('sleep', '--args', '3000',
                                                      '--max-retries', '1'))
            started = self.wait_transition(job_id, 'job_started', state='RUNNING',
                                           attempt=1, retry_count=0)
            owner_id = int(started['worker_id'])
            owner, owner_output, owner_errors = workers[owner_id]
            survivor_id, = workers.keys() - {owner_id}
            survivor, survivor_output, survivor_errors = workers[survivor_id]
            self.assertNotIn('worker job_assigned', read_output(survivor_output))
            self.wait_next_heartbeat(owner_id, owner)

            # Keep this worker stopped through recovery and completion. No close,
            # shutdown, exit, or SIGCONT is used to trigger reassignment.
            try:
                self.pause_worker(owner)
                self.assertFalse(any(entry['event'] == 'job_completed'
                                     for entry in self.job_events(job_id)))
                before_expiry = time.monotonic() + 0.3
                while time.monotonic() < before_expiry:
                    self.assertIsNone(owner.poll(), 'Paused worker exited')
                    self.assertEqual(self.worker_events('worker_dead', owner_id), [])
                    self.assertEqual(self.worker_events('heartbeat_timeout', owner_id), [])
                    events = self.job_events(job_id)
                    self.assertEqual([entry['event'] for entry in events],
                                     ['job_submitted', 'job_assigned', 'job_started'])
                    self.assertEqual(events[-1]['state'], 'RUNNING')
                    time.sleep(0.01)
                # The coordinator and healthy worker still make progress while
                # the paused worker's registration and assignment remain alive.
                self.wait_next_heartbeat(survivor_id, survivor)
                self.assert_pong(self.run_cli())

                self.wait_transition(job_id, 'job_worker_lost', timeout=8, state='QUEUED',
                                     worker_id=0, attempt=1, retry_count=1, pending=1)
                dead = self.wait_for_worker_event('worker_dead', owner_id)
                timeouts = self.worker_events('heartbeat_timeout', owner_id)
                self.assertEqual(len(timeouts), 1)
                expired = timeouts[0]
                self.assertEqual(dead['state'], 'DEAD')
                self.assertEqual(dead['reason'], 'heartbeat_timeout')
                self.assertEqual(expired['fd'], dead['fd'])
                self.assertEqual(int(expired['timeout_ms']), 6000)
                silence_ms = int(expired['silence_ms'])
                self.assertEqual(silence_ms, int(expired['detected_at_ms']) -
                                 int(dead['last_heartbeat_ms']))
                self.assertGreaterEqual(silence_ms, 6000)
                self.assertLess(silence_ms, 7500)  # Allow local scheduling slack.
                latest = self.worker_events('heartbeat_received', owner_id)[-1]
                self.assertEqual(latest['last_heartbeat_ms'], dead['last_heartbeat_ms'])
                self.assertIsNone(owner.poll(), 'Expiry must occur while the owner is still stopped')

                self.wait_transition(job_id, 'job_started', state='RUNNING',
                                     worker_id=survivor_id, attempt=2, retry_count=1)
                self.wait_transition(job_id, 'job_completed', timeout=6, state='DONE',
                                     worker_id=survivor_id, attempt=2, retry_count=1,
                                     pending=0, result_bytes=13, result='"slept_ms=3000"')
                followup = self.accepted_id(self.submit_cli('fibonacci', '--args', '10'))
                self.assertNotEqual(followup, job_id)
                self.wait_transition(followup, 'job_completed', state='DONE',
                                     worker_id=survivor_id, attempt=1, retry_count=0,
                                     pending=0, result_bytes=2, result='"55"')
                self.wait_next_heartbeat(survivor_id, survivor)
                self.assert_pong(self.run_cli())

                self.assert_recovery_transitions(job_id, owner_id, survivor_id)
                self.assertEqual(len(self.worker_events('worker_dead', owner_id)), 1)
                self.assertEqual(self.worker_events('worker_dead', survivor_id), [])
                self.assertEqual(len(self.worker_events('worker_registered', survivor_id)), 1)
                self.assertIsNone(survivor.poll())
                self.assertIsNone(owner.poll(), 'Original worker must stay paused through completion')
                self.assertEqual(read_output(owner_errors), '')
                self.assertNotIn('job_completed_sent', read_output(owner_output))
                self.assertNotIn('job_failed_sent', read_output(owner_output))
                print(f'job {job_id}: SIGSTOP worker {owner_id}; heartbeat_timeout after '
                      f'{silence_ms}ms of silence; connected worker {survivor_id} completed '
                      f'attempt 2, result=slept_ms=3000; follow-up job {followup}=55', flush=True)
            finally:
                # SIGTERM cannot be handled by a stopped process. Kill only this
                # test-owned child after assertions, or on failure, to guarantee cleanup.
                # Resuming an old attempt is deliberately a separate acceptance scenario.
                if owner.poll() is None:
                    owner.kill()
                    owner.wait(timeout=2)
            self.stop_worker(survivor, survivor_errors)


class OldAttemptRecoveryTests(RecoveryTestCase):
    def check_resumed_worker(self, resume_after_completion):
        with self.worker_process(interval_ms=100) as first, \
                self.worker_process(interval_ms=100) as second:
            workers = {}
            for process, output, errors in (first, second):
                worker_id = self.wait_for_registration(process, output, errors)
                self.wait_for_worker_event('heartbeat_received', worker_id)
                workers[worker_id] = (process, output, errors)
            self.assertEqual(len(workers), 2)
            job_id = self.accepted_id(self.submit_cli('sleep', '--args', '3000',
                                                      '--max-retries', '1'))
            started = self.wait_transition(job_id, 'job_started', state='RUNNING',
                                           attempt=1, retry_count=0)
            owner_id = int(started['worker_id'])
            owner, owner_output, owner_errors = workers[owner_id]
            survivor_id, = workers.keys() - {owner_id}
            survivor, survivor_output, survivor_errors = workers[survivor_id]
            self.assertNotIn('worker job_assigned', read_output(survivor_output))
            self.wait_next_heartbeat(owner_id, owner)

            try:
                self.pause_worker(owner)
                self.wait_transition(job_id, 'job_worker_lost', timeout=8, state='QUEUED',
                                     worker_id=0, attempt=1, retry_count=1, pending=1)
                dead = self.wait_for_worker_event('worker_dead', owner_id)
                self.assertEqual(dead['reason'], 'heartbeat_timeout')
                self.wait_transition(job_id, 'job_started', state='RUNNING',
                                     worker_id=survivor_id, attempt=2, retry_count=1)
                completed_fields = dict(state='DONE', worker_id=survivor_id, attempt=2,
                                        retry_count=1, pending=0, result_bytes=13,
                                        result='"slept_ms=3000"')
                if resume_after_completion:
                    self.wait_transition(job_id, 'job_completed', timeout=6, **completed_fields)

                expected_state = 'DONE' if resume_after_completion else 'RUNNING'
                before_resume = self.job_events(job_id)
                self.assertEqual(before_resume[-1]['state'], expected_state)
                self.assertEqual(before_resume[-1]['worker_id'], str(survivor_id))
                old_heartbeats = self.worker_events('heartbeat_received', owner_id)
                self.assertIsNone(owner.poll())
                # Resume the SAME process, with its old socket and attempt-1 assignment.
                owner.send_signal(signal.SIGCONT)
                self.assertEqual(owner.wait(timeout=2), 1, read_output(owner_errors))
                # A real connection error, not SIGTERM/SIGKILL, must end the resumed worker.
                self.assertRegex(read_output(owner_errors),
                                 r'worker: (coordinator (disconnected|connection)|send (heartbeat|job report))')
                self.assertEqual(self.job_events(job_id), before_resume)
                self.assertEqual(self.worker_events('heartbeat_received', owner_id), old_heartbeats)

                self.wait_transition(job_id, 'job_completed', timeout=6, **completed_fields)
                completed_events = self.job_events(job_id)
                followup = self.accepted_id(self.submit_cli('fibonacci', '--args', '10'))
                self.assertNotEqual(followup, job_id)
                self.wait_transition(followup, 'job_completed', state='DONE',
                                     worker_id=survivor_id, attempt=1, retry_count=0,
                                     pending=0, result_bytes=2, result='"55"')
                self.wait_next_heartbeat(survivor_id, survivor)
                self.assert_pong(self.run_cli())

                self.assertEqual(self.job_events(job_id), completed_events)
                self.assert_recovery_transitions(job_id, owner_id, survivor_id)
                self.assertEqual(self.worker_events('heartbeat_received', owner_id), old_heartbeats)
                self.assertEqual(len(self.worker_events('heartbeat_timeout', owner_id)), 1)
                self.assertEqual(len(self.worker_events('worker_dead', owner_id)), 1)
                self.assertEqual(len(self.worker_events('worker_registered', owner_id)), 1)
                self.assertEqual(len(self.worker_events('worker_registered', survivor_id)), 1)
                self.assertEqual(self.worker_events('worker_dead', survivor_id), [])
                self.assertIsNone(survivor.poll())
                # The old socket may accept a local send even after peer closure.
                # Do not confuse that worker log with acceptance by the coordinator.
                local_report = 'job_completed_sent' in read_output(owner_output)
                print(f'job {job_id}: resumed worker {owner_id} while attempt 2 was '
                      f'{expected_state}; old worker exited on connection error; '
                      f'old_local_completion_send={local_report}; worker {survivor_id} '
                      f'result=slept_ms=3000 preserved; follow-up job {followup}=55', flush=True)
            finally:
                # Also cleans up a still-stopped child if an earlier assertion fails.
                if owner.poll() is None:
                    owner.kill()
                    owner.wait(timeout=2)
            self.stop_worker(survivor, survivor_errors)

    def test_resumed_worker_cannot_change_running_retry(self):
        self.check_resumed_worker(resume_after_completion=False)

    def test_resumed_worker_cannot_overwrite_completed_retry(self):
        self.check_resumed_worker(resume_after_completion=True)


class RetryExhaustionTests(RecoveryTestCase):
    def check_worker_loss_exhaustion(self, max_retries):
        # One worker per allowed attempt, plus a healthy spare. Available
        # capacity must not turn an exhausted job into another assignment.
        with ExitStack() as stack:
            workers = {}
            for _ in range(max_retries + 2):
                worker = stack.enter_context(self.worker_process(interval_ms=100))
                process, output, errors = worker
                worker_id = self.wait_for_registration(process, output, errors)
                self.assertNotIn(worker_id, workers)
                self.wait_for_worker_event('heartbeat_received', worker_id)
                workers[worker_id] = worker

            # Long enough that every attempt must be interrupted, not finish
            # while the harness is checking registrations or logs.
            job_id = self.accepted_id(self.submit_cli('sleep', '--args', '60000',
                                                      '--max-retries', str(max_retries)))
            expected = [('job_submitted', 'QUEUED', '0', '0', '0', '1')]
            interrupted = []
            for attempt in range(1, max_retries + 2):
                started = self.wait_transition(job_id, 'job_started', state='RUNNING',
                                               attempt=attempt, retry_count=attempt - 1,
                                               pending=0, result_bytes=0)
                owner_id = int(started['worker_id'])
                self.assertIn(owner_id, workers)
                self.assertNotIn(owner_id, interrupted)
                owner, owner_output, owner_errors = workers[owner_id]
                self.wait_next_heartbeat(owner_id, owner)
                self.assertEqual(self.job_events(job_id)[-1], started)

                owner.send_signal(signal.SIGKILL)
                self.assertEqual(owner.wait(timeout=2), -signal.SIGKILL,
                                 read_output(owner_errors))
                self.assertEqual(read_output(owner_errors), '')
                self.assertNotIn('job_completed_sent', read_output(owner_output))
                self.assertNotIn('job_failed_sent', read_output(owner_output))
                interrupted.append(owner_id)

                exhausted = attempt == max_retries + 1
                state = 'FAILED' if exhausted else 'QUEUED'
                # Terminal records retain the last owner for history; only
                # requeued records release ownership back to zero.
                recorded_owner = owner_id if exhausted else 0
                retries = max_retries if exhausted else attempt
                pending = 0 if exhausted else 1
                self.wait_transition(job_id, 'job_worker_lost', state=state,
                                     worker_id=recorded_owner, attempt=attempt,
                                     retry_count=retries, pending=pending, result_bytes=0)
                dead = self.wait_for_worker_event('worker_dead', owner_id)
                self.assertEqual(dead['state'], 'DEAD')
                self.assertIn(dead['reason'], ('eof', 'recv_error', 'truncated_message'))
                self.assertEqual(self.worker_events('heartbeat_timeout', owner_id), [])
                expected.extend([
                    ('job_assigned', 'ASSIGNED', str(owner_id), str(attempt), str(attempt - 1), '0'),
                    ('job_started', 'RUNNING', str(owner_id), str(attempt), str(attempt - 1), '0'),
                    ('job_worker_lost', state, str(recorded_owner), str(attempt), str(retries), str(pending)),
                ])

            failed_events = self.job_events(job_id)
            transitions = [(entry['event'], entry['state'], entry['worker_id'],
                            entry['attempt'], entry['retry_count'], entry['pending'])
                           for entry in failed_events]
            self.assertEqual(transitions, expected)
            survivor_id, = workers.keys() - set(interrupted)
            survivor, survivor_output, survivor_errors = workers[survivor_id]
            self.assertIsNone(survivor.poll())

            # Exercise actual scheduler progress after exhaustion. An extra
            # sleep retry would occupy the only live worker and block this job.
            followup = self.accepted_id(self.submit_cli('fibonacci', '--args', '10'))
            self.assertNotEqual(followup, job_id)
            self.wait_transition(followup, 'job_completed', state='DONE',
                                 worker_id=survivor_id, attempt=1, retry_count=0,
                                 pending=0, result_bytes=2, result='"55"')
            self.wait_next_heartbeat(survivor_id, survivor)
            self.assert_pong(self.run_cli())
            self.assertEqual(self.job_events(job_id), failed_events)
            self.assertNotIn(f'worker job_assigned job_id={job_id} ', read_output(survivor_output))
            self.assertEqual(self.worker_events('worker_dead', survivor_id), [])
            for worker_id in workers:
                self.assertEqual(len(self.worker_events('worker_registered', worker_id)), 1)
            for worker_id in interrupted:
                self.assertEqual(len(self.worker_events('worker_dead', worker_id)), 1)
            print(f'job {job_id}: max_retries={max_retries}; SIGKILL workers {interrupted}; '
                  f'FAILED at attempt {max_retries + 1}, retry_count={max_retries}, pending=0; '
                  f'no further assignment; worker {survivor_id} completed follow-up job {followup}=55',
                  flush=True)
            self.stop_worker(survivor, survivor_errors)

    def test_repeated_worker_crashes_exhaust_two_retries(self):
        self.check_worker_loss_exhaustion(max_retries=2)

    def test_zero_retries_fails_after_first_worker_crash(self):
        self.check_worker_loss_exhaustion(max_retries=0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=Path('build/debug'))
    parser.add_argument('--port', type=int, default=None)
    arguments = parser.parse_args()
    test_ping.BIN_DIR = arguments.bin_dir
    test_ping.TEST_PORT = arguments.port
    unittest.main(argv=[__file__], verbosity=2)
