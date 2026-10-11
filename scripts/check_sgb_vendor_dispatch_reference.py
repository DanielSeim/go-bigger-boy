#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe bounded host writes/readback equality; no private port values."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_music import build as firmware
from build_sgb_vendor_dispatch_fixture import build, PROFILES, STATES
from check_sgb_vendor_music_reference import capture

FIELDS = {'request','port','half','first_read','last_read','first_equal',
          'last_equal','equal_reads','unequal_reads'}


def summarize(data, profile, state):
    if profile not in PROFILES or state not in STATES:
        raise ValueError('invalid dispatch profile/state')
    expected = [1,2,3] if profile == 'change' else [1,1,1]
    requests = [i+1 for i,music in enumerate(data['music']) if music]
    notes = [e for e in data['events'] if e['register'] == 0x4C]
    pitches = [1437,1357,1281] if profile == 'change' else [1437]*3
    if ([data['music'][i-1] for i in requests] != expected or
            len(notes) != 3 or [n['request'] for n in notes] != requests or
            [n['pitch'] for n in notes] != pitches or
            any([n[k] for k in ('srcn','adsr1','adsr2','gain')] != [2,255,224,184] for n in notes)):
        raise ValueError('distinct-note dispatch ownership changed')
    for note, request in zip(notes,requests[1:]):
        completed = any(e['register'] == 0x5C and e['value'] == 255 and
                        note['half'] < e['half'] < data['requests'][request-1]
                        for e in data['events'])
        if completed != (state == 'completed'):
            raise ValueError('preceding dispatch state changed')
    if data.get('final') != dict(flg=0,kof=0,echo_left=0,echo_right=0):
        raise ValueError('final dispatch did not release')
    records = data.get('port_handshakes')
    if not isinstance(records,list) or not 1 <= len(records) <= 128:
        raise ValueError('missing bounded handshake observations')
    last = -1
    by_request = {r:[] for r in requests}
    for index,record in enumerate(records):
        if (not isinstance(record,dict) or set(record) != FIELDS or
                any(type(v) is not int or v < 0 for v in record.values()) or
                record['port'] > 3 or record['half'] < last):
            raise ValueError('invalid handshake fields/order')
        last = record['half']
        request = record['request']
        if request not in by_request:
            raise ValueError('handshake belongs to another request')
        keyon = notes[requests.index(request)]['half']
        if not data['requests'][request-1] <= record['half'] < keyon:
            raise ValueError('handshake outside request/key-on window')
        following = next((r['half'] for r in records[index+1:]
                          if isinstance(r,dict) and r.get('request') == request and
                          r.get('port') == record['port']),keyon)
        reads = record['equal_reads'] + record['unequal_reads']
        if reads > 1000000:
            raise ValueError('read observation bound exceeded')
        if reads:
            if not record['half'] <= record['first_read'] <= record['last_read'] <= following:
                raise ValueError('read outside latest-write interval')
        elif record['first_read'] or record['last_read']:
            raise ValueError('read timestamps without reads')
        if record['equal_reads']:
            if not record['first_read'] <= record['first_equal'] <= record['last_equal'] <= record['last_read']:
                raise ValueError('equal read outside observed reads')
        elif record['first_equal'] or record['last_equal']:
            raise ValueError('equality timestamps without equal reads')
        by_request[request].append(dict(port=record['port'],
            write_ms=(record['half']-data['requests'][request-1])/2048,
            first_equal_after_write_ms=(record['first_equal']-record['half'])/2048
                if record['equal_reads'] else None,
            equal_reads=record['equal_reads'],unequal_reads=record['unequal_reads']))
    if any(not records for records in by_request.values()):
        raise ValueError('missing request writes')
    return [dict(music=music,pitch=note['pitch'],
                 selection_ms=(note['half']-data['requests'][request-1])/2048,
                 writes=by_request[request])
            for music,note,request in zip(expected,notes,requests)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--model',choices=('sgb','sgb2'),action='append')
    parser.add_argument('--profile',choices=PROFILES,action='append')
    parser.add_argument('--state',choices=STATES,action='append')
    args = parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='gbb-dispatch-') as directory:
            root = Path(directory)
            owned = root/'owned.rom'; owned.write_bytes(firmware())
            inputs = root/'none.script'; inputs.write_text('GBB SGB input v1\n0 none\n')
            fixtures = {(p,s):build(p,s) for p in args.profile or PROFILES for s in args.state or STATES}
            reports = []
            for model in args.model or ('sgb','sgb2'):
                original = args.firmware_dir/('sgb1.program.rom' if model == 'sgb' else 'sgb2.program.rom')
                cases = {}
                for (profile,state),image in fixtures.items():
                    game = root/'game.gb'; game.write_bytes(image)
                    measured = {kind:summarize(capture(args.probe.resolve(),program.resolve(),game,model,
                                260000000,inputs,port_handshakes=True),profile,state)
                                for kind,program in (('original',original),('replacement',owned))}
                    cases[profile+'-'+state] = dict(fixture_sha256=hashlib.sha256(image).hexdigest(),**measured)
                reports.append(dict(model=model,program_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),cases=cases))
    except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError):
        parser.error('private dispatch experiment failed; child diagnostics suppressed')
    print(json.dumps(dict(schema='gbb-sgb-vendor-dispatch-v1',qualification=False,
                          firmware_sha256=hashlib.sha256(firmware()).hexdigest(),runs=reports)))


if __name__ == '__main__': main()
