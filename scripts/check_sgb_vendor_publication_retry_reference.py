#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Paired original/owned gate for unreplied publications, retries and root waits."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
from build_sgb_vendor_tick_selection_fixture import build
from build_sgb_vendor_music import build as firmware
from check_sgb_vendor_music_reference import capture
from check_sgb_vendor_delayed_tempo_reference import summarize

# Freeze these before observing the new holdouts. SGB1 is the paired model control.
SCHEDULES={'baseline':((7,2048),(8,0),(8,512),(8,1024)),
           'holdout':((7,1792),(8,256),(8,768),(8,1280))}
WAIT_TOLERANCE_MS=2


def cases():
    return [(group,model,live,gap+(4 if model=='sgb' else 0),spin)
            for group,schedules in SCHEDULES.items() for model in ('sgb','sgb2')
            for live in (32,128) for gap,spin in schedules]


def fixture(case):
    group,model,live,gap,spin=case
    if case not in cases() or any(type(n) is not int for n in (live,gap,spin)):
        raise ValueError('invalid publication retry coordinate')
    return build(128,live_tempo=live,change_after=3,boundary=True,
                 request_gap=gap,selection_spin=spin,prefix_duration=8)


def order(delta):
    if delta is None:return 'unobserved'
    if type(delta) not in (int,float) or not math.isfinite(delta):
        raise ValueError('invalid publication delta')
    return 'before' if delta<0 else 'after' if delta>0 else 'shared'


def observe(data,live):
    result=summarize(data,live,8)
    first=next(e['half'] for e in data['events'] if e['register']==0x4C)
    return dict(**result,first_old_keyon_half=first,
                prior_keyon_age_ms=(result['request_half']-first)/2048)


def profile(summary):
    writes=summary['handoff']['music_writes']
    # These are observation categories, never assertions about an acceptance PC.
    return dict(startup_overlap=summary['startup_overlap'],operand_count=len(summary['operand_reads']),
        publication_order=summary['publication_order'],handoff_category=summary['handoff']['category'],
        writes=[dict(port=w['port'],matching_reply_count=w['matching_reply_count'],
                     operand_after_write=order(w['operand_after_write_ms']),
                     reply_after_operand=order(w['reply_after_operand_ms']),
                     directory_after_reply=order(w['directory_after_reply_ms'])) for w in writes])


def compare(original,replacement):
    for result in (original,replacement):
        if (type(result['prior_keyon_age_ms']) not in (int,float) or not math.isfinite(result['prior_keyon_age_ms']) or
                not 0<result['prior_keyon_age_ms']<=10000 or
                type(result['wait_ms']) not in (int,float) or not math.isfinite(result['wait_ms']) or
                not 0<=result['wait_ms']<=100 or result['startup_overlap'] is not False or
                result['established_before_publication'] is not True):
            raise ValueError('comparison requires bounded established playback')
    a,b=profile(original),profile(replacement)
    error=abs(original['wait_ms']-replacement['wait_ms'])
    state_equal=(original['old_note_count']==replacement['old_note_count'] and
                 [r['request'] for r in original['operand_reads']]==[r['request'] for r in replacement['operand_reads']])
    return dict(wait_error_ms=error,wait_within_2ms=error<=WAIT_TOLERANCE_MS,
                prior_score_state_equal=state_equal,
                prior_keyon_age_difference_ms=abs(original['prior_keyon_age_ms']-replacement['prior_keyon_age_ms']),
                handoff_profile_equal=a==b,original_profile=a,replacement_profile=b)


def anchor(summary,live):
    """Frozen SGB2 baseline observation; holdouts are never fitted to this shape."""
    p=profile(summary);writes=p['writes']
    if not summary['first_publication_before_operand'] or summary['operand_before_directory'] is not True:
        return False
    if live==32:
        return (summary['first_publication_evidence']=='later_tempo_only' and
                p['publication_order']=='writes_straddle_operand' and p['handoff_category']=='ambiguous_music_writes' and
                len(writes)==2 and writes[0]['port']==writes[1]['port']==0 and
                writes[0]['matching_reply_count']==0 and writes[0]['operand_after_write']=='after' and
                writes[1]['matching_reply_count']>0 and writes[1]['operand_after_write']=='before' and
                writes[1]['reply_after_operand']=='after' and writes[1]['directory_after_reply']=='before')
    return (live==128 and summary['first_publication_evidence']=='control_agrees' and
            summary['unique_publication_crossing'] is True and len(writes)==1 and
            writes[0]['port']==0 and writes[0]['matching_reply_count']>0 and
            writes[0]['operand_after_write']=='after' and writes[0]['reply_after_operand']=='after' and
            writes[0]['directory_after_reply']=='before')


def corpus(rows):
    expected=set(cases());seen=set();anchors=[]
    groups={name:[] for name in SCHEDULES}
    for row in rows:
        key=tuple(row[k] for k in ('group','model','live','gap','spin'))
        if (any(type(row[k]) is not int for k in ('live','gap','spin')) or key not in expected or key in seen or
                row['fixture_sha256']!=hashlib.sha256(fixture(key)).hexdigest()):
            raise ValueError('invalid or duplicate retry corpus identity')
        seen.add(key)
        comparison=compare(row['original'],row['replacement'])
        groups[row['group']].append(comparison)
        if row['group']=='baseline' and row['model']=='sgb2':anchors.append(anchor(row['original'],row['live']))
    if seen!=expected or len(anchors)!=8:raise ValueError('incomplete retry corpus')
    group_results={name:dict(cases=len(results),waits_within_2ms=all(r['wait_within_2ms'] for r in results),
        max_wait_error_ms=max(r['wait_error_ms'] for r in results),
        handoff_profiles_equal=sum(r['handoff_profile_equal'] for r in results),
        prior_score_states_equal=sum(r['prior_score_state_equal'] for r in results),
        max_prior_keyon_age_difference_ms=max(r['prior_keyon_age_difference_ms'] for r in results),
        wait_mismatches_with_different_prior_state=sum(not r['wait_within_2ms'] and not r['prior_score_state_equal'] for r in results)) for name,results in groups.items()}
    return dict(scope='root_wait_agreement_and_frozen_original_retry_anchors',
                passed=all(anchors) and all(g['waits_within_2ms'] for g in group_results.values()),
                original_retry_anchors_passed=all(anchors),original_retry_anchor_cases=len(anchors),
                handoff_equivalent=all(r['handoff_profile_equal'] for results in groups.values() for r in results),
                groups=group_results)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True);parser.add_argument('--firmware-dir',type=Path,required=True)
    args=parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='gbb-publication-retry-') as tmp:
            root=Path(tmp);inputs=root/'none.script';inputs.write_text('GBB SGB input v1\n0 none\n')
            probe=root/args.probe.name;shutil.copy2(args.probe.resolve(),probe)
            programs={}
            for name in ('sgb1.program.rom','sgb2.program.rom'):
                programs[name]=root/name;shutil.copy2(args.firmware_dir/name,programs[name])
            replacement=root/'replacement.rom';replacement.write_bytes(firmware(selection_wait=True))
            def run(case):
                group,model,live,gap,spin=case;image=fixture(case)
                game=root/f'{group}-{model}-{live}-{gap}-{spin}.gb';game.write_bytes(image)
                results=[]
                for program in (programs['sgb1.program.rom' if model=='sgb' else 'sgb2.program.rom'],replacement):
                    data=capture(probe,program,game,model,300000000,inputs,tempo_publication=True)
                    results.append(observe(data,live))
                return dict(group=group,model=model,live=live,gap=gap,spin=spin,
                    fixture_sha256=hashlib.sha256(image).hexdigest(),original=results[0],replacement=results[1],
                    comparison=compare(*results))
            with ThreadPoolExecutor(max_workers=4) as pool:rows=list(pool.map(run,cases()))
            report=dict(schema='gbb-sgb-vendor-publication-retry-v1',qualification=False,parameters_fitted=False,
                holdout_scope='new_authored_schedules_only_not_guaranteed_distinct_physical_phases',
                probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),
                program_sha256={n:hashlib.sha256(p.read_bytes()).hexdigest() for n,p in programs.items()},
                replacement_sha256=hashlib.sha256(replacement.read_bytes()).hexdigest(),
                runs=rows,gate=corpus(rows))
        print(json.dumps(report))
        return 0 if report['gate']['passed'] else 1
    except (OSError,ValueError,KeyError,TypeError,IndexError,StopIteration,subprocess.SubprocessError):
        parser.exit(2,'bounded publication-retry comparison failed; child diagnostics suppressed\n')


if __name__=='__main__':raise SystemExit(main())
