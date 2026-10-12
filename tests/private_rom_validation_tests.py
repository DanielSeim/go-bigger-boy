#!/usr/bin/env python3
"""Offline contract tests: virtual clocks, fake GitHub, and real NAS file locks."""
from contextlib import redirect_stdout
from datetime import datetime, timezone
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


v = load('validation', 'scripts/private_rom_validation.py')
nas = load('nas', 'docs/private-rom-validation/tools/serialized_validation.py') if os.name == 'posix' else None
SHA = 'a' * 40


def stamp(seconds):
    return datetime.fromtimestamp(seconds, timezone.utc).isoformat()


class Clock:
    def __init__(self):
        self.now = 1000000
        self.sleeps = []

    def sleep(self, seconds):
        self.sleeps.append(seconds)
        self.now += seconds


class GateTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.clock = Clock()
        self.origin = self.clock.now
        self.run = dict(id=12, display_title=f'GBB validation / 9-10 / {SHA}',
                        event='workflow_dispatch', path='.github/workflows/gbb-validation.yml',
                        repository={'full_name': 'owner/private'}, head_branch='main', run_attempt=1,
                        created_at=stamp(self.origin), status='queued', conclusion=None)
        self.calls = []
        self.visible = False
        self.post_error = None
        self.queue = 1200
        self.execution = 3600
        self.conclusion = 'success'
        self.api = v.Api('test-token', transport=self.transport, clock=lambda: self.clock.now,
                         wall=lambda: self.clock.now, sleep=self.clock.sleep)
        self.gate = v.Gate(self.api, 'owner/private', 'gbb-validation.yml', SHA, '9-10',
                           Path(self.directory.name) / 'marker.json')
        self.output = io.StringIO()
        self.redirect = redirect_stdout(self.output)
        self.redirect.__enter__()
        self.addCleanup(self.redirect.__exit__, None, None, None)

    def transport(self, method, url, token, body, timeout):
        self.calls.append((method, url))
        if method == 'POST':
            self.visible = True
            if self.post_error:
                raise self.post_error
            return None
        elapsed = self.clock.now - self.origin
        if '/jobs?' in url:
            if elapsed < self.queue:
                return {'jobs': [], 'total_count': 0}
            completed = elapsed >= self.queue + self.execution
            return {'jobs': [dict(name=v.JOB_NAME, status='completed' if completed else 'in_progress',
                                 started_at=stamp(self.origin + self.queue),
                                 completed_at=stamp(self.origin + self.queue + self.execution) if completed else None,
                                 conclusion=self.conclusion if completed else None)]}
        if '/workflows/' in url:
            return {'workflow_runs': [self.run] if self.visible else []}
        return dict(self.run, status='completed' if elapsed >= self.queue + self.execution else 'in_progress',
                    conclusion=self.conclusion if elapsed >= self.queue + self.execution else None)

    def test_queue_twenty_minutes_then_execution_sixty(self):
        self.gate.wait(self.gate.register())
        self.assertEqual(self.clock.now - self.origin, 80 * 60)
        self.assertIn('queued:', self.output.getvalue())
        self.assertIn('in_progress:', self.output.getvalue())
        self.assertEqual(sum(method == 'POST' for method, _ in self.calls), 1)

    def test_reuse_completed_run_after_parent_runner_restart(self):
        self.gate.register()
        self.clock.now += 10000
        self.run['status'] = 'completed'
        self.gate.wait(self.gate.register())
        self.assertEqual(sum(method == 'POST' for method, _ in self.calls), 1)

    def test_resume_execution_after_queue_budget_has_passed(self):
        self.queue = 6000
        self.execution = 3600
        self.clock.now += 8000
        self.gate.wait(dict(self.run, status='in_progress'))
        self.assertEqual(self.clock.now - self.origin, 9600)

    def test_in_progress_run_can_still_have_queued_job(self):
        self.queue = 7201
        with self.assertRaises(v.ValidationError):
            self.gate.wait(dict(self.run, status='in_progress'))
        self.assertEqual(self.clock.now - self.origin, 7200)

    def test_ambiguous_dispatch_reconciled_without_resending(self):
        for error in (ConnectionError('timeout'), v.HttpError(503), v.AmbiguousDispatch('bad JSON')):
            with self.subTest(error=type(error).__name__):
                self.post_error = error
                self.gate.register()
                self.gate.state_file.unlink()
                self.visible = False
        self.assertEqual(sum(method == 'POST' for method, _ in self.calls), 3)

    def test_unanswered_dispatch_is_not_success_and_not_repeated(self):
        def absent(method, *args):
            if method == 'POST':
                raise ConnectionError('unknown')
            return {'workflow_runs': []}
        self.api.transport = absent
        with self.assertRaises(v.ValidationError):
            self.gate.register()
        self.assertEqual(self.clock.now - self.origin, 600)
        with self.assertRaises(v.ValidationError):
            self.gate.register()
        self.assertTrue(self.gate.state_file.exists())

    def test_hosted_runner_replacement_does_not_redispatch(self):
        self.gate.allow_dispatch = False
        with self.assertRaisesRegex(v.ValidationError, 'second dispatch'):
            self.gate.register()
        self.assertFalse(any(method == 'POST' for method, _ in self.calls))
        self.visible = True
        self.assertEqual(self.gate.register()['id'], 12)

    def test_private_failure_is_not_success(self):
        self.visible = True
        self.queue = self.execution = 0
        self.conclusion = 'failure'
        with self.assertRaises(v.ValidationError):
            self.gate.wait(self.run)

    def test_registration_queue_and_execution_limits(self):
        for queue, execution, maximum in ((7201, 0, 7200), (0, 4501, 4500)):
            with self.subTest(queue=queue):
                self.clock.now = self.origin
                self.queue, self.execution = queue, execution
                with self.assertRaises(v.ValidationError):
                    self.gate.wait(self.run)
                self.assertEqual(self.clock.now - self.origin, maximum)

    def test_commit_and_workflow_identity_checked(self):
        for change in ({'display_title': self.run['display_title'].replace(SHA, 'b'*40)},
                       {'path': '.github/workflows/other.yml'}, {'head_branch': 'other'},
                       {'event': 'push'}, {'run_attempt': 2},
                       {'repository': {'full_name': 'other/private'}}):
            with self.subTest(change=change), self.assertRaises(v.ValidationError):
                self.gate.verify_run(dict(self.run, **change))
        self.visible = True
        self.run['display_title'] = self.run['display_title'].replace(SHA, 'b'*40)
        with self.assertRaisesRegex(v.ValidationError, 'different commit'):
            self.gate.register()

    def test_delayed_registration_and_503_after_ambiguous_dispatch(self):
        original = self.transport
        reads_after_dispatch = 0
        def delayed(method, url, *args):
            nonlocal reads_after_dispatch
            if method == 'GET' and '/workflows/' in url and self.visible:
                reads_after_dispatch += 1
                if reads_after_dispatch == 1:
                    raise v.HttpError(503)
                if reads_after_dispatch < 4:
                    return {'workflow_runs': []}
            return original(method, url, *args)
        self.api.transport = delayed
        self.post_error = ConnectionError('accepted but response lost')
        self.assertEqual(self.gate.register()['id'], 12)
        self.assertGreater(self.clock.now - self.origin, 10)
        self.assertEqual(sum(method == 'POST' for method, _ in self.calls), 1)

    def test_historical_run_must_obey_both_actual_phase_limits(self):
        self.clock.now += 20000
        for queue, execution in ((7201, 10), (10, 4501)):
            with self.subTest(queue=queue):
                self.queue, self.execution = queue, execution
                with self.assertRaises(v.ValidationError):
                    self.gate.wait(dict(self.run, status='completed'))

    def test_paginated_lookup_finds_matching_commit(self):
        calls = []
        unrelated = dict(self.run, display_title='Other validation')
        def pages(method, url, *args):
            calls.append(url)
            return {'workflow_runs': [unrelated] * 100 if 'page=1&' in url else [self.run]}
        self.api.transport = pages
        self.assertEqual(self.gate.find_run(self.clock.now + 100)['id'], 12)
        self.assertEqual(len(calls), 2)
        self.assertTrue(all('created=' in url for url in calls))

    def test_duplicate_matches_rejected(self):
        self.api.transport = lambda *args: {'workflow_runs': [self.run, dict(self.run, id=13)]}
        with self.assertRaisesRegex(v.ValidationError, 'Multiple'):
            self.gate.register()

    def test_completed_workflow_without_job_fails(self):
        self.run['status'] = 'completed'
        self.api.transport = lambda method, url, *args: {'jobs': []} if '/jobs?' in url else self.run
        with self.assertRaises(v.ValidationError):
            self.gate.wait(self.run)


class ApiTests(unittest.TestCase):
    def api(self, responses):
        self.clock = Clock()
        self.calls = 0
        def transport(*args):
            self.calls += 1
            response = responses.pop(0)
            if isinstance(response, Exception):
                raise response
            return response
        return v.Api('token', transport=transport, clock=lambda: self.clock.now,
                     wall=lambda: self.clock.now, sleep=self.clock.sleep)

    def test_503_backoff_and_recovery(self):
        api = self.api([v.HttpError(503), v.HttpError(503), {'ok': True}])
        with redirect_stdout(io.StringIO()):
            self.assertEqual(api.call('test', self.clock.now + 100), {'ok': True})
        self.assertEqual(self.clock.sleeps, [1, 2])

    def test_all_transient_read_statuses_retry(self):
        for status in (429, 500, 502, 503, 504):
            with self.subTest(status=status):
                api = self.api([v.HttpError(status), {'ok': True}])
                with redirect_stdout(io.StringIO()):
                    self.assertEqual(api.call('test', self.clock.now + 100), {'ok': True})
                self.assertEqual(self.calls, 2)

    def test_retry_after_and_rate_limit(self):
        for status, headers, expected in ((429, {'Retry-After': '7'}, 7),
                 (503, {'Retry-After': 'Mon, 12 Jan 1970 13:46:49 GMT'}, 9),
                 (403, {'X-RateLimit-Remaining': '0', 'X-RateLimit-Reset': '1000011'}, 11)):
            with self.subTest(status=status):
                api = self.api([v.HttpError(status, headers), {}])
                with redirect_stdout(io.StringIO()):
                    api.call('test', self.clock.now + 100)
                self.assertEqual(self.clock.sleeps, [expected])

    def test_bounded_retries_and_phase_deadline(self):
        api = self.api([v.HttpError(503)] * 6)
        with redirect_stdout(io.StringIO()), self.assertRaises(v.ValidationError):
            api.call('test', self.clock.now + 100)
        self.assertEqual(self.calls, 6)
        api = self.api([v.HttpError(429, {'Retry-After': '1000'})])
        with redirect_stdout(io.StringIO()), self.assertRaises(v.ValidationError):
            api.call('test', self.clock.now + 600)
        self.assertEqual(self.clock.sleeps, [])

    def test_authentication_permission_missing_resource(self):
        for status in (401, 403, 404):
            with self.subTest(status=status):
                api = self.api([v.HttpError(status)])
                with self.assertRaises(v.ValidationError):
                    api.call('test', self.clock.now + 100)
                self.assertEqual(self.calls, 1)

    def test_tag_must_match_commit(self):
        api = self.api([{'object': {'type': 'tag', 'sha': 'tag'}},
                        {'object': {'type': 'commit', 'sha': SHA}}])
        v.verify_tag(api, 'owner/public', 'v1.2.3', SHA)
        api = self.api([{'object': {'type': 'commit', 'sha': 'b'*40}}])
        with self.assertRaises(v.ValidationError):
            v.verify_tag(api, 'owner/public', 'v1.2.3', SHA)


@unittest.skipUnless(os.name == 'posix', 'NAS transaction requires the Linux/POSIX runner')
class NasTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.old_cwd = Path.cwd()
        os.chdir(self.directory.name)
        self.addCleanup(os.chdir, self.old_cwd)
        self.root = Path('staging/state')
        self.binary = Path('runner')
        self.binary.write_bytes(b'fake executable')
        case = dict(model='cgb', frames=900, frame_sha256_by_interval={str(n): str(n) for n in (300, 600, 900)})
        self.baseline = {str(n): dict(case) for n in range(100)}
        Path('baseline.json').write_text(json.dumps(self.baseline))
        self.report = {'total': 100, 'passed': 100,
                       'results': [dict(case, id=name, ok=True) for name in self.baseline]}
        self.events = []
        self.environment = patch.dict(os.environ, GBB_DEPLOY_TOKEN='test', GBB_TEST_TOKEN='test')
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def http(self, url, token, payload, timeout):
        self.events.append((url, payload))
        return self.report if url.endswith('/test') else {}

    def execute(self, correlation='1-1', http=None):
        return nas.transact(self.root, SHA, correlation, self.binary, 'baseline.json', http=http or self.http)

    def test_several_pushes_do_not_displace_release_or_deploy_during_test(self):
        # Release holds the actual shared file lock while four push requests arrive.
        def running_test(url, *args):
            if url.endswith('/test'):
                for n in range(2, 6):
                    with self.assertRaises(BlockingIOError):
                        self.execute(f'1-{n}')
                self.assertEqual(len(self.events), 1)  # only release deployment
            return self.http(url, *args)
        self.execute(http=running_test)
        # Their durable queued requests can run in turn after the release completes.
        for n in range(2, 6):
            self.execute(f'1-{n}')
        self.assertEqual(len(self.events), 10)
        self.assertTrue(all((self.root / f'1-{n}.json').exists() for n in range(1, 6)))

    def test_successful_request_is_idempotent(self):
        self.execute()
        self.execute()
        self.assertEqual(len(self.events), 2)
        self.assertFalse((self.root / 'active.json').exists())

    def test_unknown_nas_outcome_quarantines_all_following_deployments(self):
        def uncertain(*args):
            raise TimeoutError('service may still be running')
        with self.assertRaises(TimeoutError):
            self.execute(http=uncertain)
        self.assertTrue((self.root / 'active.json').exists())
        with self.assertRaisesRegex(RuntimeError, 'quarantined'):
            self.execute('1-2')
        self.assertEqual(self.events, [])

    def test_failed_baseline_fails_and_cannot_be_replayed_as_success(self):
        self.report['passed'] = 99
        with self.assertRaises(RuntimeError):
            self.execute()
        self.assertFalse((self.root / 'active.json').exists())
        with self.assertRaises(RuntimeError):
            self.execute()
        self.assertEqual(len(self.events), 2)

    def test_wrong_commit_or_changed_baseline_cannot_reuse_success(self):
        self.execute()
        with self.assertRaises(RuntimeError):
            nas.transact(self.root, 'b'*40, '1-1', self.binary, 'baseline.json', http=self.http)
        Path('baseline.json').write_text('{}')
        with self.assertRaises(RuntimeError):
            self.execute()


class WorkflowTests(unittest.TestCase):
    def test_queue_and_timeout_contract(self):
        private = (ROOT / 'docs/private-rom-validation/gbb-validation.yml').read_text()
        release = (ROOT / '.github/workflows/release.yml').read_text()
        pushes = (ROOT / '.github/workflows/private-rom-tests.yml').read_text()
        self.assertIn('queue: max', private)
        self.assertIn('queue: max', release)
        self.assertNotIn('\nconcurrency:', pushes)
        self.assertIn('timeout-minutes: 75', private)
        for public in (release, pushes):
            self.assertIn('timeout-minutes: 240', public)
            self.assertIn('${{ github.repository_id }}-${{ github.run_id }}', public)
            self.assertNotIn('${{ github.run_attempt }}', public)
        self.assertIn('group: gbb-private-rom-execution', private)
        self.assertIn('runs-on: [self-hosted, Linux, X64, gbb-rom-tests]', private)


if __name__ == '__main__':
    unittest.main()
