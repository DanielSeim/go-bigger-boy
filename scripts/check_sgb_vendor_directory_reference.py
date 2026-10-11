#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Observe owned score-directory read addresses before distinct-note key-on."""
from check_sgb_vendor_handoff_reference import summarize as handoff_summary


def summarize(data, profile, state):
    result = handoff_summary(data,profile,state)
    records = data.get('selection_directory_reads')
    notes = {e['request']:e for e in data['events'] if e['register']==0x4C}
    if not isinstance(records,list) or not 1 <= len(records) <= 128:
        raise ValueError('missing bounded score-directory reads')
    grouped = {r:[] for r in notes}; last = -1
    for read in records:
        if (not isinstance(read,dict) or set(read) != {'request','half','address'} or
                any(type(v) is not int or v < 0 for v in read.values()) or
                read['request'] not in notes or not 0x2B00 <= read['address'] <= 0x2B05 or
                read['half'] < last):
            raise ValueError('invalid directory read fields/order')
        request=read['request']; last=read['half']
        if not data['requests'][request-1] <= last < notes[request]['half']:
            raise ValueError('directory read outside request/key-on interval')
        grouped[request].append(read)
    for item,request in zip(result,notes):
        start=data['requests'][request-1]
        setup=next(p['half'] for p in data['selection_pitch_writes']
                   if p['request']==request and p['pitch']==notes[request]['pitch'])
        low=0x2B00+2*(data['music'][request-1]-1)
        selected=[r for r in grouped[request] if r['address'] in (low,low+1)]
        if {r['address'] for r in selected if r['half'] <= setup} != {low,low+1}:
            raise ValueError('selected directory word not read before expected pitch setup')
        item['directory_reads']=[dict(address=r['address'],read_ms=(r['half']-start)/2048)
                                 for r in grouped[request]]
        item['first_directory_read_ms']=(selected[0]['half']-start)/2048
        item['last_directory_read_ms']=(selected[-1]['half']-start)/2048
        item['pitch_setup_after_last_directory_read_ms']=(setup-selected[-1]['half'])/2048
    return result


if __name__ == '__main__':
    from check_sgb_vendor_handoff_reference import main
    # Keep fixture construction, private execution bounds and report provenance
    # identical to the handoff experiment; substitute only observation/summary.
    main(directory=True)
