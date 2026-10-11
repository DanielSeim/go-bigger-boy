#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Separate SPC reply publication from host readback without exporting bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_music import build as firmware
from build_sgb_vendor_dispatch_fixture import build, PROFILES, STATES
from check_sgb_vendor_music_reference import capture
from check_sgb_vendor_dispatch_reference import summarize as dispatch_summary

FIELDS = {'request','port','half','first_reply','last_reply','first_equal_reply',
          'last_equal_reply','reply_writes','equal_reply_writes'}


def summarize(data, profile, state):
    result = dispatch_summary(data,profile,state)
    hosts = data['port_handshakes']
    replies = data.get('spc_reply_handshakes')
    if not isinstance(replies,list) or len(replies) != len(hosts):
        raise ValueError('missing or incomplete SPC reply observations')
    notes = {e['request']:e['half'] for e in data['events'] if e['register'] == 0x4C}
    by_request = {request:[] for request in notes}
    for index,(reply,host) in enumerate(zip(replies,hosts)):
        if (not isinstance(reply,dict) or set(reply) != FIELDS or
                any(type(v) is not int or v < 0 for v in reply.values()) or
                any(reply[k] != host[k] for k in ('request','port','half')) or
                not reply['equal_reply_writes'] <= reply['reply_writes'] <= 1000000):
            raise ValueError('invalid SPC reply fields/identity/counts')
        following = next((h['half'] for h in hosts[index+1:]
                          if h['port'] == host['port'] and h['request'] == host['request']),
                         notes[host['request']])
        if reply['reply_writes']:
            if not host['half'] <= reply['first_reply'] <= reply['last_reply'] <= following:
                raise ValueError('SPC reply outside latest host-write interval')
        elif reply['first_reply'] or reply['last_reply']:
            raise ValueError('SPC publication times without writes')
        if reply['equal_reply_writes']:
            if not reply['first_reply'] <= reply['first_equal_reply'] <= reply['last_equal_reply'] <= reply['last_reply']:
                raise ValueError('equal SPC reply outside publication interval')
        elif reply['first_equal_reply'] or reply['last_equal_reply']:
            raise ValueError('equal SPC publication times without writes')
        by_request[host['request']].append(dict(port=host['port'],
            write_ms=(host['half']-data['requests'][host['request']-1])/2048,
            first_reply_after_write_ms=(reply['first_reply']-host['half'])/2048 if reply['reply_writes'] else None,
            first_equal_reply_after_write_ms=(reply['first_equal_reply']-host['half'])/2048 if reply['equal_reply_writes'] else None,
            first_equal_read_after_equal_reply_ms=(host['first_equal']-reply['first_equal_reply'])/2048
                if host['equal_reads'] and reply['equal_reply_writes'] else None,
            reply_writes=reply['reply_writes'],equal_reply_writes=reply['equal_reply_writes']))
    for item,request in zip(result,notes):
        item['replies'] = by_request[request]
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe',type=Path,required=True)
    parser.add_argument('--firmware-dir',type=Path,required=True)
    parser.add_argument('--model',choices=('sgb','sgb2'),action='append')
    parser.add_argument('--profile',choices=PROFILES,action='append')
    parser.add_argument('--state',choices=STATES,action='append')
    args = parser.parse_args()
    try:
        with tempfile.TemporaryDirectory(prefix='gbb-reply-') as directory:
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
                                260000000,inputs,spc_replies=True),profile,state)
                                for kind,program in (('original',original),('replacement',owned))}
                    cases[profile+'-'+state] = dict(fixture_sha256=hashlib.sha256(image).hexdigest(),**measured)
                reports.append(dict(model=model,program_sha256=hashlib.sha256(original.read_bytes()).hexdigest(),cases=cases))
    except (OSError,ValueError,KeyError,TypeError,subprocess.SubprocessError):
        parser.error('private reply experiment failed; child diagnostics suppressed')
    print(json.dumps(dict(schema='gbb-sgb-vendor-reply-v1',qualification=False,
                          firmware_sha256=hashlib.sha256(firmware()).hexdigest(),runs=reports)))


if __name__ == '__main__': main()
