#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build bounded solo/paired channel-2/3 phrase playback."""
import argparse
import hashlib
from pathlib import Path
from build_sgb_prototype import assemble
from build_sgb_score_bank import ROOT
from build_sgb_score_list import source as list_source


def source():
    text = list_source()
    first,last = 'phrase_begin:\n','phrase_read:\n'
    if text.count(first)!=1 or text.count(last)!=1 or text.index(first)>=text.index(last):
        raise ValueError('sparse parser boundary changed')
    text = text[:text.index(first)]+(ROOT/'firmware/sgb/score_sparse.asm').read_text()+'\n'+text[text.index(last):]
    first,last = 'multi_pattern:\n','multi_load2:\n'
    if text.count(first)!=1 or text.count(last)!=1 or text.index(first)>=text.index(last):
        raise ValueError('sparse pattern boundary changed')
    text = text[:text.index(first)]+text[text.index(last):]
    old = 'pair_tick:\n    call list_tick_budget\n'
    if text.count(old)!=1: raise ValueError('sparse tick hook changed')
    text = text.replace(old,'pair_tick:\n    jmp sparse_tick\nsparse_pair_tick:\n')
    old = 'mix_song:\n    ; Global song volume can be selected once, in the first channel-2 prefix.\n    mov a, $60\n    bne mix_bad\n'
    if text.count(old)!=1: raise ValueError('sparse song-volume hook changed')
    text = text.replace(old,'mix_song:\n    jmp sparse_song\nsparse_song_guard:\n')
    old = '    mov $fa, #$10\n    mov $f1, #$81\n'
    if text.count(old)!=1: raise ValueError('sparse initial timer hook changed')
    text = text.replace(old,'    ; Timer starts when the initial voices are prepared.\n')
    return text+'\n.org $1480\n'+(ROOT/'firmware/sgb/score_sparse_runtime.asm').read_text()


def build():
    image = bytes(assemble(source(),'spc',0x0800))
    if len(image)>4096: raise ValueError('sparse program exceeds diagnostic code bound')
    return image


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        image=build()
        with args.output.open('xb') as output: output.write(image)
    except (OSError,ValueError) as error: parser.error(str(error))
    print(f'Experimental sparse score: {hashlib.sha256(image).hexdigest()}')


if __name__=='__main__': main()
