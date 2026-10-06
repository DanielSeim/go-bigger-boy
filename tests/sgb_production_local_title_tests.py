"""Private, exact-output production adapter regression; never distributes ROMs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

# Same-platform adapter/host equality is mandatory on every compiler. These
# extra Linux/GCC pins freeze the validated replay, not hardware accuracy.
PINS = {
    'sgb1': dict(samples=2875286, nonzero=4808006, audio_hash=3728511357718129461,
                 video_hash=11975354874673858101, gb_state_hash=18355069243313942973),
    'sgb2': dict(samples=2876085, nonzero=4871258, audio_hash=5196738866791710056,
                 video_hash=9422694177143503245, gb_state_hash=10384803269984123713),
}


def validate(reports):
    if [report['model'] for report in reports] != ['sgb1', 'sgb2']:
        raise ValueError('Expected both production model reports')
    for report in reports:
        for key in ('output_hz', 'frames', 'restores', 'inputs', 'nonzero', 'samples',
                    'audio_hash', 'video_hash', 'gb_state_hash'):
            if type(report[key]) is not int or not 0 <= report[key] < 2**64:
                raise ValueError(f'Invalid production counter/hash: {key}')
        if (report['format'] != 'gbb-sgb-production-v1' or report['boot'] != 'bundled'
                or report['input_clock'] != 'presentation-frame'
                or report['output_hz'] != 48000 or report['frames'] != 3600
                or report['restores'] != 27 or report['inputs'] != 11
                or report['nonzero'] <= 0 or report['samples'] <= 0
                or report['nonzero'] > report['samples'] * 2
                or report['pin_profile'] not in ('linux-gcc', 'same-platform')):
            raise ValueError('Incomplete production coverage')
        if report['pin_profile'] == 'linux-gcc':
            for key, value in PINS[report['model']].items():
                if report[key] != value:
                    raise ValueError(f"Production {report['model']} {key} regression")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runner', required=True, type=Path)
    parser.add_argument('--roms', required=True, type=Path)
    parser.add_argument('--bundled-ipl', action='store_true')
    args = parser.parse_args()
    game = args.roms / 'Donkey Kong (JU) (V1.1) [S][!].gb'
    save = game.with_suffix('.sav')
    for path, expected in (
        (game, 'b490c89efe718633b07381def66ce0ed58a5075aabe40c6e644baf2b408a76f4'),
        (save, '59123d52ecfd6686b62823ed0e534b6d560e1ed1bf102e63489574305b838a34'),
    ):
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError(f'Unexpected private input: {path.name}')
    script = Path(__file__).parent / 'fixtures/sgb/titles/donkey-kong-gameplay.script'
    # Keep sizeable private copies out of memory-backed /tmp. Never stage GB
    # override images: this test specifically exercises the bundled bootstrap.
    with tempfile.TemporaryDirectory(prefix='sgb-production-', dir=args.runner.resolve().parent) as temporary:
        firmware = Path(temporary)
        names = ['sgb1.program.rom', 'sgb2.program.rom']
        if not args.bundled_ipl:
            names.append('spc700.rom')
        for name in names:
            shutil.copyfile(args.roms / name, firmware / name)
        result = subprocess.run([str(args.runner.resolve()), '--local-production', str(firmware),
                                 str(game.resolve()), str(save.resolve()), str(script.resolve())],
                                check=False, text=True, capture_output=True, timeout=900)
        if result.returncode:
            raise RuntimeError(f'Production runner exited {result.returncode}:\n{result.stderr}\n{result.stdout}')
    reports = [json.loads(line) for line in result.stdout.splitlines()]
    validate(reports)
    for report in reports:
        print(json.dumps(report, sort_keys=True), flush=True)


if __name__ == '__main__':
    main()
