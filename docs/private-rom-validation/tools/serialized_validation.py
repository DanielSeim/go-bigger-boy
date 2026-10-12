#!/usr/bin/env python3
"""Private NAS transaction: one deployment + regression, durable fail-closed ledger."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import urllib.request


def write_json(path, value):
    temporary = path.with_suffix('.tmp')
    with temporary.open('w', encoding='utf-8') as output:
        json.dump(value, output)
        output.flush()
        os.fsync(output.fileno())
    os.replace(temporary, path)
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def post(url, token, payload, timeout):
    request = urllib.request.Request(url, data=json.dumps(payload).encode(), headers={
        'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'}, method='POST')
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.load(response)


def strict_report(report, baseline):
    results = report['results']
    cases = {case['id']: case for case in results}
    if (len(cases) != len(results) or report['total'] != 100 or report['passed'] != 100
            or set(cases) != set(baseline)):
        raise RuntimeError('Private regression case set or totals differ from approved baseline')
    for name, expected in baseline.items():
        case = cases[name]
        hashes = case.get('frame_sha256_by_interval', {})
        if (case.get('ok') is not True or case.get('model') != expected['model']
                or case.get('frames') != expected['frames']
                or set(hashes) != {'300', '600', '900'}
                or hashes != expected['frame_sha256_by_interval']):
            raise RuntimeError('Private regression differs from approved baseline')


def transact(root, commit, correlation, binary, baseline_path, *, http=post, compare=None):
    if not re.fullmatch(r'[a-f0-9]{40}', commit) or not re.fullmatch(r'[0-9]+-[0-9]+', correlation):
        raise RuntimeError('Invalid validation identity')
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True, mode=0o750)
    baseline_path, binary = Path(baseline_path), Path(binary)
    identity = {'commit': commit, 'correlation': correlation,
                'baseline_sha256': hashlib.sha256(baseline_path.read_bytes()).hexdigest(),
                'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest()}
    with (root / 'transaction.lock').open('a') as lock:
        # Never deploy if another cooperating writer owns the NAS transaction.
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        active = root / 'active.json'
        record = root / (correlation + '.json')
        report_file = root / (correlation + '-report.json')
        if active.exists():
            raise RuntimeError('NAS transaction quarantined; confirm service idle before operator recovery')
        if record.exists():
            saved = json.loads(record.read_text())
            if saved.get('identity') != identity or saved.get('status') != 'success':
                raise RuntimeError('Request already recorded with different identity or unsuccessful outcome')
            shutil.copyfile(report_file, 'rom-report.json')
            if compare:
                compare()
            return
        write_json(active, identity)
        idle = False
        try:
            staging = root.parent / commit
            staging.mkdir(exist_ok=True, mode=0o750)
            shutil.copyfile(binary, staging / 'gbb_test_runner')
            (staging / 'gbb_test_runner').chmod(0o750)
            (staging / 'commit.txt').write_text(commit + '\n')
            (staging / 'gbb_test_runner.sha256').write_text(identity['binary_sha256'] + '  gbb_test_runner\n')
            for name in ('commit.txt', 'gbb_test_runner.sha256'):
                (staging / name).chmod(0o640)
            # Never retry mutating NAS requests: an unanswered POST may still execute.
            http('http://deploy-service:8788/deploy', os.environ['GBB_DEPLOY_TOKEN'], {'commit': commit}, 120)
            report = http('http://rom-service:8787/test', os.environ['GBB_TEST_TOKEN'], {}, 3500)
            # Service contract: returning the report means the worker has stopped.
            idle = True
            write_json(report_file, report)
            shutil.copyfile(report_file, 'rom-report.json')
            strict_report(report, json.loads(baseline_path.read_text()))
            if compare:
                compare()
            write_json(record, {'identity': identity, 'status': 'success'})
        except BaseException:
            # If deployment/test outcome is unknown, retain active.json indefinitely.
            # Also catches normal Python interruptions; SIGKILL leaves active.json too.
            if idle:
                write_json(record, {'identity': identity, 'status': 'failed'})
            raise
        finally:
            if idle and record.exists():
                active.unlink()
                directory = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(directory)
                finally:
                    os.close(directory)


def main():
    os.umask(0o027)
    def compare():
        subprocess.run(['python3', 'tools/compare_regression.py', 'rom-report.json',
                        'baselines/frame-sha256.json', 'regression-summary.md'], check=True)
    transact('/staging/.gbb-validation', os.environ['GBB_COMMIT'].lower(),
             os.environ['CORRELATION_ID'], 'gbb-source/build-nas/gbb_test_runner',
             'baselines/frame-sha256.json', compare=compare)


if __name__ == '__main__':
    main()
