#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a pinned user-supplied GBVisualizer checkout; never download or vendor it."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

PIN=Path(__file__).resolve().parents[1]/'tests/fixtures/external-reference/gbvisualizer.json'


def adapt(text):
    text,definitions=re.subn(r'(?m)^(\s*)([A-Za-z_]\w*)(\s+EQU\b)',r'\1DEF \2\3',text)
    text,indirect=re.subn(r'(?im)^(\s*)ld(\s+\[c\]\s*,\s*a\b)',r'\1ldh\2',text)
    absolute=0
    def address(match):
        nonlocal absolute
        if match[2].strip().lower()=='c':return match[0]
        absolute+=1
        return match[1]+'[$FF00 | ('+match[2]+')]'
    text=re.sub(r'(?im)^(\s*ldh\s+[^;\n]*?)\[([^]\n]+)\]',address,text)
    return text,dict(definitions=definitions,indirect_ldh=indirect,absolute_ldh=absolute)


def build(source,output):
    pin=json.loads(PIN.read_text());source=source.resolve();output=output.resolve()
    def git(*args):return subprocess.check_output(['git','-C',str(source),*args],text=True).strip()
    if git('rev-parse','HEAD')!=pin['commit'] or git('status','--porcelain','--untracked-files=no'):
        raise ValueError('GBVisualizer checkout must match the clean pinned commit')
    versions={tool:subprocess.check_output([tool,'--version'],text=True).strip() for tool in ('rgbasm','rgblink','rgbfix')}
    if any(value!=tool+' '+pin['rgbds_version'] for tool,value in versions.items()):
        raise ValueError('RGBDS tool versions do not match the reference pin')
    if output.exists():raise ValueError('output directory must not already exist')
    if output==source or source in output.parents:raise ValueError('output must be outside upstream checkout')
    with tempfile.TemporaryDirectory(prefix='gbb-gbvisualizer-build-') as tmp:
        work=Path(tmp);changes={};inputs={}
        for name in ('visualizer.asm','gbhw.asm','music.gbm'):
            raw=(source/name).read_bytes();inputs[name]=hashlib.sha256(raw).hexdigest()
            if name.endswith('.asm'):
                text,changes[name]=adapt(raw.decode());(work/name).write_text(text)
            else:(work/name).write_bytes(raw)
        commands=[['rgbasm','-o','visualizer.o','visualizer.asm'],
                  ['rgblink','-o','visualizer.gbc','visualizer.o'],
                  ['rgbfix','-Cv','-p','0','-i','GBVP','-t','GBvisualizer','-m','25','visualizer.gbc']]
        for command in commands:subprocess.run(command,cwd=work,check=True,capture_output=True,timeout=60)
        rom=(work/'visualizer.gbc').read_bytes();digest=hashlib.sha256(rom).hexdigest()
        if len(rom)!=pin['rom_size'] or digest!=pin['rom_sha256']:
            raise ValueError('built ROM does not match the pinned size/hash')
        manifest=dict(schema='gbb-gbvisualizer-build-v1',qualification=False,upstream=pin,
                      tools=versions,input_sha256=inputs,adaptations=changes,commands=commands,rom_sha256=digest)
        output.mkdir(parents=True)
        (output/'visualizer.gbc').write_bytes(rom)
        (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    return manifest


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True);parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:print(json.dumps(build(args.source,args.output)))
    except (OSError,ValueError,subprocess.SubprocessError):parser.exit(2,'external GBVisualizer build failed; verify checkout, tool versions and output path\n')


if __name__=='__main__':main()
