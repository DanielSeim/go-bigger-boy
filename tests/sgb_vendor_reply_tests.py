#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""SPC publication bounds, host-poll separation and owned replay contracts."""
import argparse
import copy
from pathlib import Path
import sys
import unittest
import sgb_vendor_dispatch_tests as dispatch
from check_sgb_vendor_reply_reference import summarize


def synthetic():
    data = dispatch.synthetic()
    data['spc_reply_handshakes'] = [
        dict(request=h['request'],port=h['port'],half=h['half'],
             first_reply=h['half']+5,last_reply=h['half']+15,
             first_equal_reply=h['half']+5,last_equal_reply=h['half']+15,
             reply_writes=2,equal_reply_writes=2)
        for h in data['port_handshakes']]
    return data


class ReplyContracts(dispatch.DispatchContracts):
    def test_reply_summary_and_guards(self):
        data = synthetic()
        record = summarize(data,'change','active')[0]['replies'][0]
        self.assertEqual(record['first_equal_reply_after_write_ms'],5/2048)
        self.assertEqual(record['first_equal_read_after_equal_reply_ms'],15/2048)
        missing_publication = copy.deepcopy(data)
        reply = missing_publication['spc_reply_handshakes'][0]
        for key in ('first_reply','last_reply','first_equal_reply','last_equal_reply','reply_writes','equal_reply_writes'):
            reply[key] = 0
        self.assertIsNone(summarize(missing_publication,'change','active')[0]['replies'][0]['first_equal_read_after_equal_reply_ms'])
        stale = copy.deepcopy(data)
        stale['spc_reply_handshakes'][0].update(first_reply=140,last_reply=150,first_equal_reply=140,last_equal_reply=150)
        self.assertEqual(summarize(stale,'change','active')[0]['replies'][0]['first_equal_read_after_equal_reply_ms'],-10/2048)
        for fault in ('missing','length','extra','type','ownership','port','write',
                      'early','late','equal-early','equal-late','no-writes',
                      'no-equals','counts','bound'):
            with self.subTest(fault=fault):
                changed = copy.deepcopy(data)
                reply = changed['spc_reply_handshakes'][0]
                if fault == 'missing': changed.pop('spc_reply_handshakes')
                elif fault == 'length': changed['spc_reply_handshakes'].pop()
                elif fault == 'extra': reply['value'] = 1
                elif fault == 'type': reply['half'] = True
                elif fault == 'ownership': reply['request'] = 2
                elif fault == 'port': reply['port'] = 1
                elif fault == 'write': reply['half'] += 1
                elif fault == 'early': reply['first_reply'] = 109
                elif fault == 'late': reply['last_reply'] = 1001
                elif fault == 'equal-early': reply['first_equal_reply'] = 114
                elif fault == 'equal-late': reply['last_equal_reply'] = 126
                elif fault == 'no-writes': reply['reply_writes'] = reply['equal_reply_writes'] = 0
                elif fault == 'no-equals': reply['equal_reply_writes'] = 0
                elif fault == 'counts': reply['equal_reply_writes'] = 3
                elif fault == 'bound': reply['reply_writes'] = 1000001
                with self.assertRaises(ValueError): summarize(changed,'change','active')
        replaced = copy.deepcopy(data)
        host = replaced['port_handshakes'][0]
        inserted = dict(host,half=120,first_read=0,last_read=0,first_equal=0,last_equal=0,
                        equal_reads=0,unequal_reads=0)
        host.update(first_read=115,last_read=119,first_equal=119,last_equal=119)
        replaced['port_handshakes'].insert(1,inserted)
        replaced['spc_reply_handshakes'].insert(1,dict(replaced['spc_reply_handshakes'][0],
            half=120,first_reply=0,last_reply=0,first_equal_reply=0,last_equal_reply=0,
            reply_writes=0,equal_reply_writes=0))
        with self.assertRaises(ValueError): summarize(replaced,'change','active')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe',type=Path)
    parser.add_argument('--timeline-probe',type=Path)
    args,rest = parser.parse_known_args()
    dispatch.PROBE = args.probe.resolve() if args.probe else None
    dispatch.TIMELINE = args.timeline_probe.resolve() if args.timeline_probe else None
    dispatch.REPLIES = True
    unittest.main(argv=[sys.argv[0]]+rest)
