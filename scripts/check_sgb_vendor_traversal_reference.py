#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe fixed authored root, pattern-table and first-note read addresses."""
from check_sgb_vendor_directory_reference import summarize as directory_summary

WINDOWS=((0x2B10,0x2B1B),(0x2B20,0x2B4F),(0x2B60,0x2B6C),
         (0x2B90,0x2B9C),(0x2BC0,0x2BCC))


def summarize(data,profile,state):
    result=directory_summary(data,profile,state)
    records=data.get('selection_traversal_reads')
    notes={e['request']:e for e in data['events'] if e['register']==0x4C}
    if not isinstance(records,list) or not 1<=len(records)<=512:
        raise ValueError('missing bounded traversal reads')
    grouped={r:[] for r in notes}; last=-1
    for read in records:
        if (not isinstance(read,dict) or set(read)!={'request','half','address'} or
                any(type(v) is not int or v<0 for v in read.values()) or
                read['request'] not in notes or read['half']<last or
                not any(low<=read['address']<=high for low,high in WINDOWS)):
            raise ValueError('invalid traversal fields/order/window')
        request=read['request'];last=read['half']
        if not data['requests'][request-1]<=last<notes[request]['half']:
            raise ValueError('traversal outside request/key-on interval')
        grouped[request].append(read)
    for item,request in zip(result,notes):
        start=data['requests'][request-1]
        setup=next(p['half'] for p in data['selection_pitch_writes']
                   if p['request']==request and p['pitch']==notes[request]['pitch'])
        slot=data['music'][request-1]-1
        root=0x2B10+4*slot;table=0x2B20+16*slot;track=0x2B60+48*slot
        stages={};previous=max(r['half'] for r in data['selection_directory_reads']
            if r['request']==request and r['half']<=setup and
               r['address'] in (0x2B00+2*slot,0x2B01+2*slot))
        for name,low,high,required in (
                ('root',root,root+3,{root,root+1}),
                ('pattern',table,table+15,{table+4,table+5}),
                ('track',track,track+12,{track,track+12})):
            selected=[r for r in grouped[request] if low<=r['address']<=high and r['half']<=setup]
            # Require a complete traversal after the last directory lookup;
            # earlier validation visits are retained in the exported sequence.
            live=[r for r in selected if r['half']>=previous]
            if not required<={r['address'] for r in live}:
                raise ValueError('missing selected traversal stage before pitch setup')
            first=min(r['half'] for r in live);last=max(r['half'] for r in live)
            stages[name]=dict(reads=len(selected),first_read_ms=(selected[0]['half']-start)/2048,
                last_pass_first_read_ms=(first-start)/2048,last_read_ms=(last-start)/2048,
                after_previous_stage_ms=(first-previous)/2048)
            previous=last
        item['traversal_reads']=[dict(address=r['address'],read_ms=(r['half']-start)/2048)
                                 for r in grouped[request]]
        item['traversal_stages']=stages
        item['pitch_setup_after_track_read_ms']=(setup-previous)/2048
    return result


if __name__=='__main__':
    from check_sgb_vendor_handoff_reference import main
    main(traversal=True)
