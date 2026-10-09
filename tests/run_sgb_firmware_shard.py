#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build and run one complete, deterministic shard of public SGB diagnostics."""
import argparse
import json
import math
from pathlib import Path
import re
import subprocess

LABEL='sgb-firmware-extended'


def select_tests(document):
    if not isinstance(document,dict) or not isinstance(document.get('tests'),list):
        raise ValueError('invalid CTest discovery document')
    selected=[];names=set()
    for test in document['tests']:
        props={p['name']:p['value'] for p in test.get('properties',[])}
        labels=props.get('LABELS',[])
        if LABEL not in labels:continue
        name=test.get('name');command=test.get('command')
        if not isinstance(name,str) or not name or name in names or not isinstance(command,list) or not command or any(
                not isinstance(x,str) for x in command):
            raise ValueError('invalid or duplicate diagnostic test')
        if set(labels)&{'local','private-reference'}:raise ValueError('private tests cannot enter public CI shards')
        timeout=props.get('TIMEOUT',600)
        if type(timeout) not in (int,float) or not math.isfinite(timeout) or not 0<timeout<=7200:
            raise ValueError('invalid diagnostic time budget')
        names.add(name);selected.append(dict(name=name,command=command,budget_seconds=timeout))
    if not selected:raise ValueError('no public SGB diagnostic tests discovered')
    return selected


def partition(tests,count):
    if type(count) is not int or not 1<=count<=32 or count>len(tests):raise ValueError('requires 1..32 nonempty shards')
    if len({t['name'] for t in tests})!=len(tests):raise ValueError('duplicate diagnostic test')
    shards=[[] for _ in range(count)];budgets=[0]*count
    for test in sorted(tests,key=lambda t:(-t['budget_seconds'],t['name'])):
        index=min(range(count),key=lambda i:(budgets[i],i))
        shards[index].append(test);budgets[index]+=test['budget_seconds']
    return [sorted(shard,key=lambda t:t['name']) for shard in shards]


def targets_for(tests,build_dir):
    targets=set();root=build_dir.resolve()
    for test in tests:
        found=False
        for argument in test['command']:
            path=Path(argument)
            if path.is_absolute() and path.parent.resolve()==root:
                name=path.stem if path.suffix=='.exe' else path.name
                if re.fullmatch(r'gameboy_[A-Za-z0-9_]+',name):targets.add(name);found=True
        if not found:raise ValueError('diagnostic lacks a discoverable build target: '+test['name'])
    return sorted(targets)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,required=True)
    parser.add_argument('--shard-index',type=int,required=True)
    parser.add_argument('--shard-count',type=int,default=8)
    parser.add_argument('--parallel',type=int,default=2)
    parser.add_argument('--plan-only',action='store_true')
    args=parser.parse_args()
    try:
        if not 0<=args.shard_index<args.shard_count or not 1<=args.parallel<=16:raise ValueError('invalid shard or parallelism')
        root=args.build_dir.resolve()
        discovery=subprocess.run(['ctest','--test-dir',str(root),'-C','Release','--show-only=json-v1'],check=True,capture_output=True,text=True,timeout=60)
        tests=select_tests(json.loads(discovery.stdout));shards=partition(tests,args.shard_count)
        selected=shards[args.shard_index];targets=targets_for(selected,root)
        plan=dict(schema='gbb-sgb-firmware-shard-v1',shard_index=args.shard_index,shard_count=args.shard_count,
                  total_tests=len(tests),tests=[t['name'] for t in selected],targets=targets,
                  shards=[dict(tests=[t['name'] for t in s],budget_seconds=sum(t['budget_seconds'] for t in s)) for s in shards])
        (root/'sgb-firmware-shard.json').write_text(json.dumps(plan,indent=2)+'\n')
        print(json.dumps(plan,indent=2),flush=True)
        if args.plan_only:return 0
        subprocess.run(['cmake','--build',str(root),'--config','Release','--parallel',str(args.parallel),'--target',*targets],check=True)
        pattern='^('+'|'.join(re.escape(t['name']) for t in selected)+')$'
        return subprocess.run(['ctest','--test-dir',str(root),'-C','Release','--label-regex','^'+LABEL+'$',
                               '--tests-regex',pattern,'--no-tests=error','--output-on-failure','--parallel',str(args.parallel),
                               '--output-junit',str(root/'sgb-firmware-ctest.xml')]).returncode
    except (OSError,ValueError,subprocess.SubprocessError) as error:parser.error(str(error))


if __name__=='__main__':raise SystemExit(main())
