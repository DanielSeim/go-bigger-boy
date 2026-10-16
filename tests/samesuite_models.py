#!/usr/bin/env python3
"""Resolve SameSuite APU filename applicability without substituting revisions."""
from dataclasses import dataclass
from pathlib import Path
import re
import sys


@dataclass(frozen=True)
class Coverage:
    models: tuple[str, ...]
    unsupported: tuple[str, ...] = ()


def coverage(name: str) -> Coverage:
    stem = Path(name).stem.casefold()
    if stem in {'div_write_trigger', 'div_write_trigger_10'}:
        return Coverage(('dmg',))
    if '-' not in stem:
        # Upstream's general digital APU tests target the CGB-E research lane.
        return Coverage(('cgb-e',))
    tag = stem.rsplit('-', 1)[1]
    if tag == 'a':
        return Coverage(('agb',))
    if re.fullmatch(r'cgb[0a-e]+', tag):
        supported = {'0': 'cgb0', 'c': 'cgb-c', 'e': 'cgb-e'}
        models = tuple(dict.fromkeys(supported[r] for r in tag[3:] if r in supported))
        missing = tuple(dict.fromkeys('cgb-' + r for r in tag[3:] if r not in supported))
        return Coverage(models, missing)
    # A new explicit suffix must be reviewed rather than run on CGB-E.
    return Coverage((), ('unknown-' + re.sub(r'[^a-z0-9]+', '_', tag),))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: samesuite_models.py ROM_FILENAME')
    result = coverage(sys.argv[1])
    print(';'.join((*result.models, *('unsupported-' + r for r in result.unsupported))))
