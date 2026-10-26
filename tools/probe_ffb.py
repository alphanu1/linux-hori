#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Step 1: does the HORI FFB Racing Wheel (PC mode, 0f0d:015d) answer the
0x20 effect protocol documented by hid-hori (see docs/protocol.md)?

Sends a short, gentle sequence to the wheel's hidraw node and prints every
ack the wheel sends back. Hands on the wheel: it will pull, gently.

    sudo ./probe_ffb.py /dev/hidrawN [spring %] [constant %]   (default 40 25)
"""
import os, select, struct, sys, time

REPORT = 0x20
LEN = 32                      # report id + 31 bytes

def pkt(*b):
    p = bytes([REPORT]) + bytes(b)
    return p + bytes(LEN - len(p))

def le16(v):
    return struct.pack('<h' if v < 0 else '<H', v)

def effect(typ, op_slot, params):
    return pkt(0x02, typ, op_slot, *params)

def spring(op, slot, coeff, sat):
    # neg coeff, neg sat, deadband, pos coeff, pos sat, deadband, centre, duration (0 = infinite)
    p = le16(coeff) + le16(sat) + le16(0) + le16(coeff) + le16(sat) + le16(0) + le16(0) + le16(0)
    return effect(0x41, op | slot, p)

def constant(op, slot, level, ms):
    p = le16(level) + le16(ms) + le16(0) * 4      # level, duration, envelope (none)
    return effect(0x10, op | slot, p)

CREATE, UPDATE, PLAY, STOP, FREE = 0x10, 0x00, 0x40, 0x80, 0x20

def main():
    dev = sys.argv[1] if len(sys.argv) > 1 else None
    if not dev:
        sys.exit(__doc__)
    sp = max(0, min(100, int(sys.argv[2]))) * 100 if len(sys.argv) > 2 else 4000
    cf = max(0, min(100, int(sys.argv[3]))) * 100 if len(sys.argv) > 3 else 2500
    ss = min(10000, sp * 3 // 2)
    fd = os.open(dev, os.O_RDWR | os.O_NONBLOCK)

    def drain(label):
        acks = []
        t = time.time()
        while time.time() - t < 0.15:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                d = os.read(fd, 64)
                if d and d[0] in (0x20, 0x21):
                    acks.append(d[:8].hex(' '))
        print(f'   {label:44s} acks: {acks if acks else "none"}', flush=True)

    def send(p, label):
        os.write(fd, p)
        drain(label)

    def step(msg, secs):
        print(f'-- {msg} ({secs} s)', flush=True)
        time.sleep(secs)

    print('Hands on the wheel. Starting in 3 s.', flush=True)
    time.sleep(3)

    send(pkt(0x01, 0x02, 0x00), 'host control OFF (firmware centring)')
    step('the wheel should centre itself (its own spring)', 4)

    send(pkt(0x01, 0x03, 0x01), 'free all slots')
    send(pkt(0x01, 0x01, 0x00), 'release stop')
    send(pkt(0x01, 0x02, 0x01), 'host control ON')
    step('the wheel should now turn freely', 4)

    send(spring(CREATE, 0, sp, ss), f'spring: create slot 0 ({sp // 100}%)')
    send(spring(PLAY, 0, sp, ss), 'spring: play')
    step('turn the wheel: it should spring back to centre', 6)
    send(spring(STOP, 0, sp, ss), 'spring: stop')
    send(spring(FREE, 0, sp, ss), 'spring: free')

    send(constant(CREATE, 1, cf, 1500), f'constant +{cf // 100}%: create slot 1')
    send(constant(PLAY, 1, cf, 1500), 'constant: play (1.5 s)')
    step('the wheel should pull one way', 2)
    send(constant(FREE, 1, cf, 1500), 'constant: free')
    send(constant(CREATE, 1, -cf, 1500), f'constant -{cf // 100}%: create slot 1')
    send(constant(PLAY, 1, -cf, 1500), 'constant: play (1.5 s)')
    step('the wheel should pull the other way', 2)
    send(constant(FREE, 1, -cf, 1500), 'constant: free')

    send(pkt(0x01, 0x03, 0x01), 'free all slots')
    send(pkt(0x01, 0x02, 0x00), 'host control OFF (back to firmware)')
    print('Done.', flush=True)
    os.close(fd)

if __name__ == '__main__':
    main()
