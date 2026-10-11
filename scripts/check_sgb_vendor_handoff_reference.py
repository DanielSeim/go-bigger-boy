#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Correlate controlled SOUND-field equality with distinct-note pitch setup."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_music import build as firmware
from build_sgb_vendor_handoff_fixture import build, CONTROLS, PROFILES, STATES
from check_sgb_vendor_music_reference import capture
from check_sgb_vendor_reply_reference import summarize as reply_summary

FIELDS = {'request','port','half','fields','first_music_reply','music_reply_writes'}


def summarize(data, profile, state):
    if profile not in PROFILES or state not in STATES:
        raise ValueError('invalid handoff profile/state')
    result = reply_summary(data,'change',state)
    hosts = data['port_handshakes']; replies = data['spc_reply_handshakes']
    records = data.get('handoff_writes'); pitches = data.get('selection_pitch_writes')
    notes = {e['request']:e for e in data['events'] if e['register'] == 0x4C}
    if not isinstance(records,list) or len(records) != len(hosts):
        raise ValueError('missing or incomplete field observations')
    if not isinstance(pitches,list) or not 1 <= len(pitches) <= 128:
        raise ValueError('missing bounded pitch setup observations')
    by_request = {r:[] for r in notes}
    for record,host,reply in zip(records,hosts,replies):
        if (not isinstance(record,dict) or set(record) != FIELDS or
                any(type(v) is not int or v < 0 for v in record.values()) or
                any(record[k] != host[k] for k in ('request','port','half')) or
                record['music_reply_writes'] > reply['reply_writes']):
            raise ValueError('invalid field observation identity/counts')
        values = (data['music'][host['request']-1],*CONTROLS[profile],0)
        masks = {0} | {sum(1<<i for i,v in enumerate(values) if v==value) for value in values}
        if record['fields'] not in masks:
            raise ValueError('field equality contradicts owned command values')
        if record['fields'] & 1:
            if (record['music_reply_writes'] != reply['equal_reply_writes'] or
                    record['first_music_reply'] != reply['first_equal_reply']):
                raise ValueError('music/equal publication identities disagree')
        elif record['music_reply_writes'] + reply['equal_reply_writes'] > reply['reply_writes']:
            raise ValueError('disjoint publication counts exceed total writes')
        if record['music_reply_writes']:
            if not reply['first_reply'] <= record['first_music_reply'] <= reply['last_reply']:
                raise ValueError('music-value publication outside reply interval')
        elif record['first_music_reply']:
            raise ValueError('music-value publication time without writes')
        by_request[host['request']].append(record)
    last = -1
    setups = {}
    for pitch in pitches:
        if (not isinstance(pitch,dict) or set(pitch) != {'request','half','pitch'} or
                any(type(v) is not int or v < 0 for v in pitch.values()) or
                pitch['request'] not in notes or pitch['pitch'] > 65535 or pitch['half'] < last):
            raise ValueError('invalid pitch setup fields/order')
        last = pitch['half']; request = pitch['request']; note = notes[request]
        if not data['requests'][request-1] <= pitch['half'] < note['half']:
            raise ValueError('pitch setup outside delivered-request/key-on interval')
        if pitch['pitch'] == note['pitch']:
            setups.setdefault(request,pitch['half'])
    if set(setups) != set(notes):
        raise ValueError('missing expected distinct-note pitch setup')
    for item,request in zip(result,notes):
        start = data['requests'][request-1]; setup = setups[request]
        writes = by_request[request]
        candidates = [w for w in writes if w['fields'] & 1 and w['half'] <= setup]
        candidate = candidates[-1] if candidates else None
        item.update(pitch_setup_ms=(setup-start)/2048,
                    keyon_after_pitch_setup_ms=(notes[request]['half']-setup)/2048,
                    last_music_value_write_before_setup=None if candidate is None else
                        dict(port=candidate['port'],write_ms=(candidate['half']-start)/2048,
                             setup_after_write_ms=(setup-candidate['half'])/2048),
                    field_writes=[dict(port=w['port'],write_ms=(w['half']-start)/2048,fields=w['fields'],
                        music_reply_writes=w['music_reply_writes'],
                        first_music_reply_after_write_ms=(w['first_music_reply']-w['half'])/2048
                            if w['music_reply_writes'] else None) for w in writes])
    return result


def main(*, directory=False, traversal=False):
    if traversal:
        from check_sgb_vendor_traversal_reference import summarize as summary
    elif directory:
        from check_sgb_vendor_directory_reference import summarize as summary
    else:
        summary = summarize
    name = 'traversal' if traversal else 'directory' if directory else 'handoff'
    parser = argparse.ArgumentParser(description='Observe owned score traversal before key-on.' if traversal else
        'Observe owned score-directory reads before distinct-note key-on.' if directory else __doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--model',choices=('sgb','sgb2'),action='append')
    parser.add_argument('--profile',choices=PROFILES,action='append')
    parser.add_argument('--state',choices=STATES,action='append')
    args = parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='gbb-'+name+'-') as temporary:
            root = Path(temporary)
            owned = root/'owned.rom'; owned.write_bytes(firmware())
            inputs = root/'none.script'; inputs.write_text('GBB SGB input v1\n0 none\n')
            fixtures = {(p,s):build(p,s) for p in args.profile or PROFILES for s in args.state or STATES}
            reports = []
            for model in args.model or ('sgb','sgb2'):
                original = args.firmware_dir/('sgb1.program.rom' if model == 'sgb' else 'sgb2.program.rom')
                cases = {}
                for (profile,state),image in fixtures.items():
                    game = root/'game.gb'; game.write_bytes(image)
                    measured = {kind:summary(capture(args.probe.resolve(),program.resolve(),game,model,
                                260000000,inputs,selection_handoff=not (directory or traversal),selection_directory=directory,
                                selection_traversal=traversal),profile,state)
                                for kind,program in (('original',original),('replacement',owned))}
                    cases[profile+'-'+state] = dict(fixture_sha256=hashlib.sha256(image).hexdigest(),**measured)
                reports.append(dict(model=model,program_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),cases=cases))
    except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError):
        parser.error('private '+name+' experiment failed; child diagnostics suppressed')
    print(json.dumps(dict(schema='gbb-sgb-vendor-'+name+'-v1',qualification=False,
                          firmware_sha256=hashlib.sha256(firmware()).hexdigest(),runs=reports)))


if __name__ == '__main__': main()
