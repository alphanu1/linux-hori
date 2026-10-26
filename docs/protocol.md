# HORI FFB Racing Wheel: what is known

## USB (PC mode, `0f0d:015d`)

One HID interface (class 3), two endpoints: `0x81` IN and `0x02` OUT, 64 bytes
each. Product string "FFB Racing Wheel". In Xbox mode it re-enumerates as
`0f0d:015c` "HORI FFB Racing Wheel Series X" (GIP).

## HID report descriptor (149 bytes)

```
05 01 09 04 a1 01 85 01 15 00 25 07 35 00 46 3b 01 65 14 09 39 75 04 95 01
81 42 65 00 25 01 45 01 05 09 19 01 29 13 75 01 95 13 81 02 95 01 81 01 27
ff ff 00 00 47 ff ff 00 00 75 10 95 05 05 01 09 30 09 31 09 32 09 35 09 36
81 02 26 ff 00 46 ff 00 06 00 ff 09 01 75 08 95 04 81 02 c0 06 20 ff 09 01
a1 01 85 20 15 00 26 ff 00 75 08 95 1f 09 02 81 00 09 02 91 00 c0 06 21 ff
09 03 a1 01 85 21 15 00 26 ff 00 75 08 95 3f 09 04 81 00 09 04 91 00 c0
```

| Report | Direction | Contents |
|---|---|---|
| `0x01` | in | hat switch (4 bits, 0-7, null state), 19 buttons, 1 pad bit, five 16-bit axes X, Y, Z, Rz, Slider (0-65535), 4 vendor bytes (page `0xff00`) |
| `0x20` | in + out | 31 bytes, vendor page `0xff20` |
| `0x21` | in + out | 63 bytes, vendor page `0xff21` |

There is **no PID (usage page `0x0f`) section**, so Linux's PID force-feedback
drivers can't drive it.

## Controls as Linux reports them (hid-generic, measured)

| Control | Linux |
|---|---|
| Steering | `ABS_X`, 0..65535, centre 32768 |
| Accelerator | `ABS_Y`, 0 released .. 65535 pressed |
| Brake | `ABS_Z`, 0 released .. 65535 pressed |
| Clutch | `ABS_RZ`, 0 released .. 65535 pressed |
| A, B, X, Y | `BTN_TRIGGER` 288, `BTN_THUMB2` 290, `BTN_THUMB` 289, `BTN_TOP` 291 |
| LB, RB | 293, 292 |
| View, Menu | 295, 294 |
| D-pad | `ABS_HAT0X/Y` in the descriptor, but no change was seen in PC mode |

## Force feedback protocol (from hid-hori PR #18, Truck Control System `0f0d:017a`)

**Confirmed on this wheel (2026-10-26, `tools/probe_ffb.py`):** every command
below was acked, and the forces were felt on the wheel (firmware centring with
host control off, free wheel with it on, a spring at 40% and 80%, constant
force at 25% and 60% both ways). Acks are the class byte | 0x80 echoing the
command, then `00 ea` in bytes 5-6 on every ack (meaning not known yet):

```
20 01 02 00  ->  20 81 02 00 00 ea     host control off
20 01 03 01  ->  20 81 03 01 00 ea     free all slots
20 02 41 10  ->  20 82 41 10 00 ea     spring, create slot 0
20 02 10 41  ->  20 82 10 41 00 ea     constant, play slot 1
```

Not yet checked: which way a positive constant force turns the wheel.

- Effect packets on report `0x20`: `20 02 <type> <op|slot> <params>`.
- Types: constant `0x10`, ramp `0x20`, square / sine / triangle / saw up /
  saw down `0x31`-`0x35`, spring / damper / inertia / friction `0x41`-`0x44`.
- Operations: create `0x10`, update `0x00`, play `0x40`, stop `0x80`,
  free `0x20`.
- 16 effect slots (0-15), played by the wheel itself.
- Wheel settings (range, strength, sensitivities) on report `0x21`: write an
  output report, then read the reply with GET_REPORT (input).
