#!/usr/bin/env python3
"""Fail-closed private validation gate. Standard library only; no ROM data downloaded."""
import argparse
from datetime import datetime, timezone
from email.utils import parsedate_to_datetime
import json
import os
from pathlib import Path
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

REGISTRATION_SECONDS = 10 * 60
QUEUE_SECONDS = 120 * 60
EXECUTION_SECONDS = 75 * 60
JOB_NAME = 'Private ROM deployment and regression'
RETRY_STATUSES = {429, 500, 502, 503, 504}
MAX_ATTEMPTS = 6


class ValidationError(RuntimeError):
    pass


class AmbiguousDispatch(ValidationError):
    pass


class HttpError(Exception):
    def __init__(self, status, headers=None):
        self.status = status
        self.headers = {k.lower(): v for k, v in (headers or {}).items()}


def request(method, url, token, body, timeout):
    data = None if body is None else json.dumps(body).encode('utf-8')
    req = urllib.request.Request(url, data=data, method=method, headers={
        'Accept': 'application/vnd.github+json', 'Authorization': 'Bearer ' + token,
        'X-GitHub-Api-Version': '2022-11-28', 'Content-Type': 'application/json',
        'User-Agent': 'gbb-private-validation',
    })
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            raw = response.read()
            return json.loads(raw) if raw else None
    except urllib.error.HTTPError as error:
        # Never print response bodies: they can expose private repository details.
        raise HttpError(error.code, dict(error.headers)) from None
    except (urllib.error.URLError, TimeoutError, OSError) as error:
        raise ConnectionError('GitHub API transport failed') from error
    except (ValueError, UnicodeError) as error:
        if method == 'POST':
            raise AmbiguousDispatch('Dispatch response could not be decoded') from error
        raise ValidationError('Invalid GitHub API JSON response') from error


class Api:
    def __init__(self, token, *, transport=request, clock=time.monotonic,
                 sleep=time.sleep, wall=time.time):
        if not token:
            raise ValidationError('Missing GitHub API token')
        self.token, self.transport = token, transport
        self.clock, self.sleep, self.wall = clock, sleep, wall

    def pause(self, seconds, deadline):
        remaining = deadline - self.clock()
        if seconds >= remaining:
            raise ValidationError('GitHub API retry would exceed phase deadline')
        self.sleep(seconds)

    def backoff(self, error, attempt):
        delay = min(2 ** attempt, 60)
        value = error.headers.get('retry-after')
        if value:
            try:
                retry_after = float(value)
            except ValueError:
                try:
                    retry_after = parsedate_to_datetime(value).timestamp() - self.wall()
                except (ValueError, TypeError, OverflowError):
                    raise ValidationError('Invalid Retry-After header') from None
            delay = max(delay, retry_after)
        reset = error.headers.get('x-ratelimit-reset')
        if reset and error.headers.get('x-ratelimit-remaining') == '0':
            try:
                delay = max(delay, float(reset) - self.wall())
            except ValueError:
                raise ValidationError('Invalid rate-limit reset header') from None
        return max(0, delay)

    def call(self, path, deadline, *, method='GET', body=None):
        for attempt in range(MAX_ATTEMPTS):
            remaining = deadline - self.clock()
            if remaining <= 0:
                raise ValidationError('GitHub API phase deadline exceeded')
            try:
                return self.transport(method, 'https://api.github.com/' + path,
                                      self.token, body, min(30, remaining))
            except HttpError as error:
                rate_limited = error.status == 429 or (error.status == 403 and (
                    'retry-after' in error.headers or error.headers.get('x-ratelimit-remaining') == '0'))
                if error.status == 401:
                    raise ValidationError('GitHub authentication failed (HTTP 401)') from None
                if error.status == 403 and not rate_limited:
                    raise ValidationError('GitHub permission denied (HTTP 403)') from None
                if error.status == 404:
                    raise ValidationError('GitHub resource missing or token lacks access (HTTP 404)') from None
                if method == 'POST' and error.status in {500, 502, 503, 504}:
                    raise AmbiguousDispatch('Dispatch response was inconclusive; reconciling run') from None
                if error.status not in RETRY_STATUSES and not rate_limited:
                    raise ValidationError(f'GitHub API rejected request (HTTP {error.status})') from None
                if attempt == MAX_ATTEMPTS - 1:
                    raise ValidationError('GitHub API retry budget exhausted') from None
                print(f'GitHub API temporary HTTP {error.status}; retry {attempt+1}/{MAX_ATTEMPTS-1}', flush=True)
                self.pause(self.backoff(error, attempt), deadline)
            except ConnectionError:
                if method == 'POST':
                    raise AmbiguousDispatch('Dispatch transport result unknown; reconciling run') from None
                if attempt == MAX_ATTEMPTS - 1:
                    raise ValidationError('GitHub API transport retry budget exhausted') from None
                self.pause(min(2 ** attempt, 60), deadline)
        raise AssertionError('Unreachable retry state')


def timestamp(value):
    try:
        return datetime.fromisoformat(value.replace('Z', '+00:00')).timestamp()
    except (ValueError, AttributeError, TypeError):
        raise ValidationError('Missing or invalid GitHub timestamp') from None


class Gate:
    def __init__(self, api, repo, workflow, commit, correlation, state_file, lookup_since=None, allow_dispatch=True):
        if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo):
            raise ValidationError('Invalid private repository')
        if not re.fullmatch(r'[A-Za-z0-9_.-]+\.ya?ml', workflow):
            raise ValidationError('Invalid private workflow filename')
        if not re.fullmatch(r'[a-fA-F0-9]{40}', commit):
            raise ValidationError('Invalid commit SHA')
        if not re.fullmatch(r'[0-9]+-[0-9]+', correlation):
            raise ValidationError('Invalid correlation ID')
        self.api, self.repo, self.workflow = api, repo, workflow
        self.commit, self.correlation = commit.lower(), correlation
        self.allow_dispatch = allow_dispatch
        self.state_file = Path(state_file)
        self.lookup_since = lookup_since if lookup_since is not None else api.wall() - 60
        self.prefix = f'repos/{repo}/actions'
        self.title = f'GBB validation / {correlation} / {self.commit}'
        self.identity = dict(repo=repo, workflow=workflow, commit=self.commit, correlation=correlation)

    def verify_run(self, run):
        if (run.get('display_title') != self.title or run.get('event') != 'workflow_dispatch'
                or run.get('path') != '.github/workflows/' + self.workflow
                or run.get('head_branch') != 'main'
                or run.get('repository', {}).get('full_name', '').lower() != self.repo.lower()):
            raise ValidationError('Private run does not match workflow, correlation and release commit')
        if run.get('run_attempt', 1) != 1:
            raise ValidationError('Private run was rerun; create an explicit new validation request')
        if not isinstance(run.get('id'), int):
            raise ValidationError('Invalid private run ID')
        return run

    def find_run(self, deadline):
        matches = []
        # At most 1,000 recent dispatches, with explicit failure on incomplete search.
        for page in range(1, 11):
            response = self.api.call(
                f'{self.prefix}/workflows/{self.workflow}/runs?event=workflow_dispatch&per_page=100&page={page}'
                '&created=' + urllib.parse.quote('>=' + datetime.fromtimestamp(
                    self.lookup_since, timezone.utc).isoformat()), deadline)
            runs = response['workflow_runs']
            for run in runs:
                title = run.get('display_title', '')
                if title.startswith(f'GBB validation / {self.correlation} / '):
                    if title != self.title:
                        raise ValidationError('Correlation ID already belongs to a different commit')
                    matches.append(self.verify_run(run))
            if len(runs) < 100:
                break
        else:
            raise ValidationError('Run lookup exceeded bounded pagination; refusing uncertain dispatch')
        if len(matches) > 1:
            raise ValidationError('Multiple private runs match the validation request')
        return matches[0] if matches else None

    def save_state(self, **extra):
        self.state_file.parent.mkdir(parents=True, exist_ok=True)
        content = dict(self.identity, **extra)
        # Exclusive marker BEFORE the POST: a local restart must reconcile, not resend.
        with self.state_file.open('x', encoding='utf-8') as output:
            json.dump(content, output)
            output.flush()
            os.fsync(output.fileno())
        if os.name == 'posix':
            directory = os.open(self.state_file.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)

    def register(self):
        deadline = self.api.clock() + REGISTRATION_SECONDS
        previous = None
        saved_deadline = deadline
        if self.state_file.exists():
            try:
                previous = json.loads(self.state_file.read_text())
            except (ValueError, OSError):
                raise ValidationError('Invalid saved dispatch marker; refusing to resend') from None
            if any(previous.get(k) != v for k, v in self.identity.items()):
                raise ValidationError('Saved dispatch marker belongs to a different request')
            # Restarts share the original deadline, including time spent offline.
            saved_deadline = min(deadline, self.api.clock() + REGISTRATION_SECONDS -
                           max(0, self.api.wall() - previous['requested_at']))
        run = self.find_run(deadline)
        if run:
            return run
        deadline = saved_deadline
        if previous is None and not self.allow_dispatch:
            raise ValidationError('Public rerun has no registered private run; refusing a second dispatch')
        if previous is None:
            self.save_state(requested_at=self.api.wall())
            try:
                self.api.call(f'{self.prefix}/workflows/{self.workflow}/dispatches', deadline,
                              method='POST', body={'ref': 'main', 'inputs': {
                                  'commit': self.commit, 'correlation_id': self.correlation}})
            except AmbiguousDispatch as error:
                print(str(error), flush=True)
        while self.api.clock() < deadline:
            run = self.find_run(deadline)
            if run:
                return run
            self.api.sleep(min(10, max(0, deadline - self.api.clock())))
        raise ValidationError('Private workflow registration exceeded 10 minutes; dispatch not repeated')

    def wait(self, run):
        run_id = self.verify_run(run)['id']
        created = timestamp(run['created_at'])
        started = None
        phase = None
        while True:
            wall = self.api.wall()
            end = (started + EXECUTION_SECONDS) if started is not None else (created + QUEUE_SECONDS)
            deadline = self.api.clock() + max(0, end - wall)
            if phase is None and run['status'] in {'in_progress', 'completed'}:
                # A public rerun can find an already running/completed private job.
                # Read its actual timestamps before applying queue/execution budgets.
                deadline = self.api.clock() + REGISTRATION_SECONDS
            run = self.verify_run(self.api.call(f'{self.prefix}/runs/{run_id}', deadline))
            if run['id'] != run_id or timestamp(run['created_at']) != created:
                raise ValidationError('Private run identity changed during polling')
            response = self.api.call(f'{self.prefix}/runs/{run_id}/jobs?filter=latest&per_page=100', deadline)
            jobs = response['jobs']
            if len(jobs) > 1 or response.get('total_count', len(jobs)) > 1:
                raise ValidationError('Private workflow must contain exactly one deployment-and-test job')
            job = jobs[0] if jobs else None
            if job and job.get('name') != JOB_NAME:
                raise ValidationError('Unexpected private validation job')
            if job and job['status'] in ('in_progress', 'completed'):
                actual_start = timestamp(job.get('started_at'))
                if started is not None and started != actual_start:
                    raise ValidationError('Private execution start changed')
                started = actual_start
                if started < created or started > self.api.wall() + 1:
                    raise ValidationError('Invalid private execution start')
                if started - created > QUEUE_SECONDS:
                    raise ValidationError('Private workflow queue wait exceeded 120 minutes')
                elapsed = timestamp(job['completed_at']) - started if job['status'] == 'completed' else self.api.wall() - started
                if elapsed < 0:
                    raise ValidationError('Invalid private execution duration')
                if elapsed > EXECUTION_SECONDS:
                    raise ValidationError('Private execution exceeded 75 minutes')
            elif started is not None:
                raise ValidationError('Private execution unexpectedly returned to queue')
            else:
                if self.api.wall() - created >= QUEUE_SECONDS:
                    raise ValidationError('Private workflow queue wait exceeded 120 minutes')
            next_phase = 'in_progress' if started is not None else 'queued'
            if phase != next_phase:
                print(f'Private validation {next_phase}: run={run_id} commit={self.commit}', flush=True)
                phase = next_phase
            if run['status'] == 'completed':
                if run.get('conclusion') != 'success' or not job or job['status'] != 'completed' or job.get('conclusion') != 'success':
                    raise ValidationError('Private validation did not complete successfully')
                print(f'Private ROM regression passed for {self.commit}', flush=True)
                return
            if run['status'] not in {'queued', 'in_progress', 'requested', 'waiting', 'pending'}:
                raise ValidationError('Unexpected private run status')
            end = (started + EXECUTION_SECONDS) if started is not None else (created + QUEUE_SECONDS)
            remaining = end - self.api.wall()
            if remaining <= 0:
                raise ValidationError('Private validation phase deadline exceeded')
            self.api.sleep(min(10, remaining))


def verify_tag(api, repo, tag, commit):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo):
        raise ValidationError('Invalid public repository')
    if not re.fullmatch(r'v[0-9]+\.[0-9]+\.[0-9]+([-.+][A-Za-z0-9.-]+)?', tag):
        raise ValidationError('Invalid release tag')
    deadline = api.clock() + REGISTRATION_SECONDS
    obj = api.call(f'repos/{repo}/git/ref/tags/{urllib.parse.quote(tag, safe="")}', deadline)['object']
    if obj['type'] == 'tag':
        obj = api.call(f'repos/{repo}/git/tags/{obj["sha"]}', deadline)['object']
    if obj['type'] != 'commit' or obj['sha'].lower() != commit.lower():
        raise ValidationError('Release tag does not reference requested commit')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--commit', required=True)
    parser.add_argument('--correlation', required=True)
    parser.add_argument('--state-file', required=True)
    parser.add_argument('--private-repo', default='DanielSeim/gbb-private-tests')
    parser.add_argument('--workflow', default='gbb-validation.yml')
    parser.add_argument('--public-repo')
    parser.add_argument('--tag')
    args = parser.parse_args()
    try:
        api = Api(os.environ.get('GH_TOKEN', ''))
        public_api = Api(os.environ.get('PUBLIC_TOKEN', ''))
        # The public run creation time survives hosted-runner replacement and reruns.
        # Search from that time rather than a sliding recent-runs window.
        public_repo = os.environ['GITHUB_REPOSITORY']
        public_id = os.environ['GITHUB_RUN_ID']
        if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', public_repo) or not public_id.isdigit():
            raise ValidationError('Invalid public run identity')
        public_run = public_api.call(f'repos/{public_repo}/actions/runs/{public_id}',
                                     public_api.clock() + REGISTRATION_SECONDS)
        if args.correlation != f"{public_run['repository']['id']}-{public_run['id']}":
            raise ValidationError('Correlation does not match originating public workflow run')
        gate = Gate(api, args.private_repo, args.workflow, args.commit, args.correlation,
                    args.state_file, timestamp(public_run['created_at']) - 60,
                    allow_dispatch=os.environ.get('GITHUB_RUN_ATTEMPT', '1') == '1')
        if args.tag:
            verify_tag(Api(os.environ.get('PUBLIC_TOKEN', '')), args.public_repo or '', args.tag, gate.commit)
        elif args.public_repo:
            raise ValidationError('--public-repo requires --tag')
        gate.wait(gate.register())
    except (ValidationError, KeyError, TypeError, ValueError, OSError) as error:
        print(f'Private validation failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
