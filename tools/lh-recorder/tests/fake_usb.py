#!/usr/bin/env python3
"""Synthetic USB producer for supervisor lifetime tests. Never accesses USB."""
import gzip
import json
import os
from pathlib import Path
import signal
import struct
import sys
import time
if '--help' in sys.argv:
    print('SYNTHETIC libsurvive fixture --usbmon-stats --usbmon-only-record --usbmon-record')
    sys.exit(0)
if os.getenv("LH_TEST_DENY_CAPTURE"):
    print("SYNTHETIC pcap_open_live: permission denied", file=sys.stderr)
    sys.exit(1)
args = sys.argv
path = Path(args[args.index('--usbmon-record')+1])
stats = Path(args[args.index('--usbmon-stats')+1])
running = True

def stop(_sig, _frame):
    global running
    running = False

signal.signal(signal.SIGINT, stop)
signal.signal(signal.SIGTERM, stop)
path.with_name(path.name+'.usbdevs').write_text('10462 8449 1 7 WM\n')
count = 0
with gzip.open(path, 'wb') as f:
    f.write(struct.pack('<IHHIIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 220))
    while running:
        t = time.time_ns()
        payload = b'\xff\x00\x00\x00'
        packet = struct.pack('<QBBBBHBBqiiII8siiII', count+1, ord('C'), 1, 0x81, 7, 1,
                             ord('-'), 0, t//10**9, t//1000%10**6, 0, 4, 4,
                             b'\x00'*8, 1, 0, 0, 0)+payload
        f.write(struct.pack('<IIII', t//10**9, t//1000%10**6, len(packet), len(packet)))
        f.write(packet)
        count += 1
        time.sleep(0.02)
stats.write_text(json.dumps(dict(stats_available=True, received=count, dropped=0, interface_dropped=0, synthetic=True)))
