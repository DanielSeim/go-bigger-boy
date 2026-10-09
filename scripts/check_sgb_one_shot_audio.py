#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Owned natural one-shot completion/retrigger in actual 48-kHz combined PCM."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from build_sgb_one_shot_audio_fixture import build,PROFILES
from build_sgb_score_transport import build as program
from check_sgb_host_sample_pitch import OPTIONS,IMAGE_HASH
from check_sgb_polyphony_audio import observe as observe_polyphony, SUM_TOLERANCE
from check_sgb_audio_transitions import RATE,MASTER,FRAMES,pitch,digest


def observe(meta,raw,profile,held_voice):
    result=observe_polyphony(meta,raw,profile,held_voice)
    completions=meta.get('completions')
    if not isinstance(completions,list) or len(completions)!=5:
        raise ValueError('missing natural completion timeline')
    mask=0
    for index,(note,completion) in enumerate(zip(meta['notes'],completions)):
        if not isinstance(completion,list) or len(completion)!=4 or any(type(x) is not int for x in completion):
            raise ValueError('invalid natural completion fields')
        if note[0]==held_voice:
            if any(completion):raise ValueError('looping voice reported as one-shot')
            continue
        mask|=1<<index
        end_half,end_frame,zero_half,zero_frame=completion
        if not note[3]<end_half<=zero_half<note[4] or not note[6]<end_frame<=zero_frame<note[7] or (
                zero_frame-note[6]>768 or zero_frame-end_frame>32):
            raise ValueError('natural ENDX/zero must precede note-gate release')
    if any(type(meta.get(k)) is not int or meta[k]!=mask for k in ('restore_ends','restore_natural_zeros')):
        raise ValueError('missing natural-completion restore coverage')
    result['completions']=[c for n,c in zip(meta['notes'],completions) if n[0]!=held_voice]
    return result


def compare(controls,model,held_voice):
    both=controls['both'];meta=both['meta']
    for profile in PROFILES[1:]:
        other=controls[profile]
        if any(meta[k]!=other['meta'][k] for k in ('frames','clocks','edges','notes','completions','gb_samples','native_samples')):
            raise ValueError('one-shot controls do not share exact timeline')
        if other['left']!=both['left']:raise ValueError('one-shot controls disturb GB PCM')
    gb=controls['gb']
    if any(gb['right']):raise ValueError('silent source control is audible')
    maxima={}
    for channel in ('left','right'):
        maximum=max(abs(a-b-c+d) for a,b,c,d in zip(both[channel],controls['voice2'][channel],controls['voice3'][channel],gb[channel]))
        if maximum>SUM_TOLERANCE:raise ValueError('one-shot sum differs from matched sources')
        maxima[channel]=maximum
    held=controls[f'voice{held_voice}'];peer=controls[f'voice{5-held_voice}']
    transients=[];held_pitches=[]
    for index,(note,completion) in enumerate(zip(both['peer'],both['completions'])):
        begin=note[6];stop=both['peer'][index+1][6]-4 if index<3 else note[7]-4
        values=peer['right'][begin:stop]
        active=[i for i,v in enumerate(values) if v]
        if len(active)<32 or active[0]>128 or not 32<=active[-1]<=768 or max(abs(v) for v in values)<100:
            raise ValueError('one-shot onset or retrigger is silent, stale or too long')
        quiet=completion[3]+32
        if stop-quiet<512 or any(peer['right'][quiet:stop]) or both['right'][quiet:stop]!=held['right'][quiet:stop]:
            raise ValueError('natural completion leaves stale PCM or disturbs looping peer')
        # Check held pitch across each transient/retrigger and natural completion.
        start=begin+32
        if start+FRAMES>=both['held'][7]:raise ValueError('held pitch crosses release')
        held_pitches.append(pitch(held['right'][start:start+FRAMES],440))
        transients.append(dict(on_frame=begin,first_audible_frame=begin+active[0],last_audible_frame=begin+active[-1],
                               nonzero_frames=len(active),peak=max(abs(v) for v in values),
                               endx_frame=completion[1],natural_zero_frame=completion[3],gate_off_frame=note[7],
                               quiet_begin=quiet,quiet_end=stop,pcm_sha256=digest(values)))
    begin=both['held'][6]+128
    window=both['left'][begin:begin+FRAMES];midpoint=(max(window)+min(window))//2
    pulse=pitch([v-midpoint for v in window],(MASTER/5 if model=='sgb' else 4194304)/8192)
    return dict(maximum_sum_error_lsb=maxima,sum_tolerance_lsb=SUM_TOLERANCE,transients=transients,
                held_pitch_windows=held_pitches,gb_pulse=pulse,matched_timeline=True,uninterrupted_gb_equal=True,final_silent=True)


def run(probe,host,directory,model,held_voice,profile,mode):
    game=directory/'game.gb';pcm=directory/'output.pcm';report=directory/'output.json'
    image=build(profile,held_voice);game.write_bytes(image)
    child=subprocess.run([str(probe),str(host),str(game),model,mode,str(pcm),str(report),str(held_voice)],
                         capture_output=True,text=True,timeout=90)
    if child.returncode or child.stdout or child.stderr:raise ValueError('bounded one-shot probe failed: '+child.stderr[:256])
    if pcm.stat().st_size>600000 or report.stat().st_size>8192:raise ValueError('one-shot output bound')
    raw=pcm.read_bytes();meta=json.loads(report.read_text());observed=observe(meta,raw,profile,held_voice)
    return observed,dict(model=model,held_voice=held_voice,profile=profile,mode=mode,
                         fixture_sha256=hashlib.sha256(image).hexdigest(),pcm_sha256=hashlib.sha256(raw).hexdigest(),**meta)


def measure(probe):
    image=program(**OPTIONS)
    if hashlib.sha256(image).hexdigest()!=IMAGE_HASH:raise ValueError('DA image changed')
    runs=[];comparisons=[]
    with tempfile.TemporaryDirectory(prefix='gbb-one-shot-audio-') as name:
        directory=Path(name);host=directory/'host.rom';host.write_bytes(image)
        for model in ('sgb','sgb2'):
            for held_voice in (2,3):
                controls={}
                for profile in PROFILES:
                    controls[profile],summary=run(probe,host,directory,model,held_voice,profile,'combined');runs.append(summary)
                comparisons.append(dict(model=model,held_voice=held_voice,**compare(controls,model,held_voice)))
                scalar,summary=run(probe,host,directory,model,held_voice,'both','scalar')
                if scalar!=controls['both']:raise ValueError('scalar one-shot PCM/timeline differs')
                runs.append(summary)
    return dict(schema='gbb-sgb-one-shot-audio-v1',qualification=False,playback=False,
                evidence='owned_48k_whole_host_natural_completion',image_sha256=IMAGE_HASH,runs=runs,comparisons=comparisons)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--probe',type=Path,required=True)
    args=parser.parse_args()
    try:report=measure(args.probe.resolve())
    except (OSError,ValueError,subprocess.TimeoutExpired) as error:parser.error(str(error))
    print(json.dumps(report,indent=2))
