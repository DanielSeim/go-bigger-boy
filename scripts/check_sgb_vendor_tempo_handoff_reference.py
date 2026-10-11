#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Correlate an authored tempo read with public music-value writes and replies."""
from check_sgb_vendor_dispatch_reference import FIELDS as HOST_FIELDS
from check_sgb_vendor_reply_reference import FIELDS as REPLY_FIELDS
from check_sgb_vendor_handoff_reference import FIELDS as MUSIC_FIELDS
from check_sgb_vendor_tempo_boundary_reference import summarize as boundary_summary


def summarize(data,live):
    result=boundary_summary(data,live)
    hosts=data.get('port_handshakes');replies=data.get('spc_reply_handshakes');music=data.get('handoff_writes')
    if (not isinstance(hosts,list) or not 1<=len(hosts)<=128 or
            not isinstance(replies,list) or not isinstance(music,list) or
            len(hosts)!=len(replies) or len(hosts)!=len(music)):
        raise ValueError('missing bounded handoff observations')
    first=next(e['half'] for e in data['events'] if e['register']==0x4C)
    ends={1:first,2:result['selected_half']};last=(-1,0);seen=set();candidates=[]
    for i,(host,reply,field) in enumerate(zip(hosts,replies,music)):
        for record,keys in ((host,HOST_FIELDS),(reply,REPLY_FIELDS),(field,MUSIC_FIELDS)):
            if (not isinstance(record,dict) or set(record)!=keys or
                    any(type(v) is not int or v<0 for v in record.values())):
                raise ValueError('invalid handoff fields')
        request=host['request'];half=host['half'];port=host['port']
        if (request not in ends or port>3 or (half,request)<last or
                not data['requests'][request-1]<=half<ends[request] or
                any(record[k]!=host[k] for record in (reply,field) for k in ('request','port','half'))):
            raise ValueError('invalid handoff identity or ownership')
        last=(half,request);seen.add(request)
        following=next((h['half'] for h in hosts[i+1:] if h['request']==request and h['port']==port),ends[request])
        reads=host['equal_reads']+host['unequal_reads']
        if reads>1000000 or not reply['equal_reply_writes']<=reply['reply_writes']<=1000000:
            raise ValueError('handoff count exceeded')
        for count,low,high,lower,upper in (
                (reads,host['first_read'],host['last_read'],half,following),
                (host['equal_reads'],host['first_equal'],host['last_equal'],host['first_read'],host['last_read']),
                (reply['reply_writes'],reply['first_reply'],reply['last_reply'],half,following),
                (reply['equal_reply_writes'],reply['first_equal_reply'],reply['last_equal_reply'],reply['first_reply'],reply['last_reply'])):
            if (count and not lower<=low<=high<=upper) or (not count and (low or high)):
                raise ValueError('handoff timestamp outside latest-write interval')
        # Authored SOUND controls are all zero, while music is 1 or 2.
        if field['fields'] not in (0,1,14) or field['music_reply_writes']>reply['reply_writes']:
            raise ValueError('invalid owned-field equality')
        if field['fields']==1:
            if (field['music_reply_writes']!=reply['equal_reply_writes'] or
                    field['first_music_reply']!=reply['first_equal_reply']):
                raise ValueError('music/equal reply mismatch')
        elif field['music_reply_writes']+reply['equal_reply_writes']>reply['reply_writes']:
            raise ValueError('disjoint reply counts overlap')
        if ((field['music_reply_writes'] and not reply['first_reply']<=field['first_music_reply']<=reply['last_reply']) or
                (not field['music_reply_writes'] and field['first_music_reply'])):
            raise ValueError('music reply outside publication interval')
        if request==2 and field['fields']==1:
            operand=result['operand_reads'][0]['half'] if len(result['operand_reads'])==1 else None
            matching=field['first_music_reply'] if field['music_reply_writes'] else None
            candidates.append(dict(port=port,write_half=half,first_matching_reply_half=matching,
                matching_reply_count=field['music_reply_writes'],
                operand_after_write_ms=None if operand is None else (operand-half)/2048,
                reply_after_operand_ms=None if operand is None or matching is None else (matching-operand)/2048,
                directory_after_write_ms=(result['directory_half']-half)/2048,
                directory_after_reply_ms=None if matching is None else (result['directory_half']-matching)/2048))
    if seen!={1,2}:raise ValueError('missing request handoff writes')
    category='missing_music_write' if not candidates else 'ambiguous_music_writes'
    if len(candidates)==1:
        candidate=candidates[0];write=candidate['write_half'];reply=candidate['first_matching_reply_half']
        if len(result['operand_reads'])!=1:category='ambiguous_operand' if result['operand_reads'] else 'operand_not_observed'
        elif reply is None:category='missing_matching_reply'
        else:
            operand=result['operand_reads'][0]['half']
            # Coarse host callback clocks can share a timestamp with SPC events.
            if operand==write or operand==reply:category='shared_timestamp'
            elif operand<write:category='operand_before_write'
            elif operand<reply:category='operand_between_write_and_reply'
            else:category='operand_after_reply'
    result['handoff']=dict(category=category,music_writes=candidates)
    return result


def main():
    from check_sgb_vendor_tempo_boundary_reference import main as run
    run(handoff=True)


if __name__=='__main__':main()
