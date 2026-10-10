#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Optional caller-owned Donkey Kong playback gate; exports aggregate metadata only."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_vendor_music import build, ROOT

TITLE_SHA256 = 'b490c89efe718633b07381def66ce0ed58a5075aabe40c6e644baf2b408a76f4'
SCRIPT_SHA256 = 'a5d37081cc52b8bfe72284f5cd836b0ccea9bfc3ddf25fcc1ac2769b3b2e802b'
CLOCKS = 1000000000


def validate(report, model):
    if (not isinstance(report, dict) or report.get('schema') != 'gbb-sgb-vendor-music-v1' or
            report.get('qualification') is not False or report.get('model') != model or
            report.get('mode') != 'native' or report.get('reset_equal') is not True or
            report.get('restore_equal') is not True):
        raise ValueError('invalid private title evidence identity')
    for key, expected in dict(firmware_state=1, transfer_error=0, transfers=2,
                              adoptions=3, sounds=3, starts=2, completes=1,
                              notes=2, selected=1, rejected=0, flg=0xE0,
                              host_status=0, external=0).items():
        if type(report.get(key)) is not int or report[key] != expected:
            raise ValueError('private title gate failed: ' + key)
    for key in ('clocks','frames','nonzero','last_nonzero_clock','restores','unread_restores','gb_frames','pcm_fnv64'):
        if type(report.get(key)) is not int or report[key] <= 0:
            raise ValueError('missing positive title observation: '+key)
    if (not CLOCKS <= report['clocks'] <= CLOCKS+256 or
            not 1300000 <= report['frames'] <= 1600000 or
            not 2472 <= report['gb_frames'] <= 3000 or
            not report['last_nonzero_clock'] < report['clocks']-1000000 or
            report['nonzero'] >= report['frames'] or
            report['unread_restores'] > report['restores'] or report['restores'] > 2048):
        raise ValueError('incomplete bounded title playback/lifecycle evidence')
    # Forward only this explicit aggregate allowlist, never arbitrary child data.
    fields = ('model','clocks','gb_frames','transfers','adoptions','sounds','starts',
              'completes','notes','frames','nonzero','last_nonzero_clock','pcm_fnv64',
              'restores','unread_restores','reset_equal','restore_equal')
    return {key: report[key] for key in fields}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--game', type=Path, required=True)
    parser.add_argument('--input-script', type=Path,
                        default=ROOT/'tests/fixtures/sgb/titles/donkey-kong-gameplay.script')
    args = parser.parse_args()
    try:
        # This gate pins one caller-owned input, never modifies the game or save.
        if args.game.stat().st_size != 524288 or hashlib.sha256(args.game.read_bytes()).hexdigest() != TITLE_SHA256:
            raise ValueError('title does not match the pinned caller-owned game')
        if args.input_script.stat().st_size > 4096 or hashlib.sha256(args.input_script.read_bytes()).hexdigest() != SCRIPT_SHA256:
            raise ValueError('input script does not match the pinned sequence')
        image = build()
        with tempfile.TemporaryDirectory(prefix='gbb-vendor-title-') as directory:
            firmware = Path(directory)/'owned.rom'
            firmware.write_bytes(image)
            reports = []
            for model in ('sgb','sgb2'):
                child = subprocess.run([str(args.probe.resolve()),str(firmware),str(args.game.resolve()),
                                        model,str(CLOCKS),'native','bundled',str(args.input_script.resolve())],
                                       capture_output=True,timeout=360)
                if child.returncode or len(child.stdout) > 4096:
                    raise ValueError('private title child failed or exceeded report bound')
                reports.append(validate(json.loads(child.stdout),model))
        print(json.dumps(dict(schema='gbb-sgb-vendor-music-title-v1',qualification=False,
                              experimental_playback=True,firmware_sha256=hashlib.sha256(image).hexdigest(),
                              title_sha256=TITLE_SHA256,input_script_sha256=SCRIPT_SHA256,
                              cases=reports),sort_keys=True))
    except (OSError,ValueError,subprocess.SubprocessError) as error:
        parser.error(str(error))


if __name__ == '__main__':
    main()
