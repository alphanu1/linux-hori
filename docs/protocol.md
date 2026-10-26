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

## Force feedback protocol

From hid-hori PR #18 (HORI Truck Control System, `0f0d:017a`), and
**confirmed on this wheel** on 2026-10-26, first with `tools/probe_ffb.py`
(forces felt by hand) and then with `hori-ffb` and `tools/ffcheck` (the
steering axis measured while each effect played, wheel unheld).

All commands are 32-byte output reports on report `0x20` (`20` + 31 bytes),
written to the wheel's hidraw node. Values are little-endian 16-bit.

### Device control: `20 01 <cmd> <arg>`

| Command | Meaning |
|---|---|
| `20 01 01 01` | **stop all**: stops every effect and **empties every slot** |
| `20 01 01 00` | release the stop |
| `20 01 02 01` | host control on: the motor plays the host's effects |
| `20 01 02 00` | host control off |
| `20 01 03 01` | free all slots |

What the motor does, measured by turning the wheel off-centre with a
constant force and then sending each combination:

| Stop | Host control | Motor |
|---|---|---|
| set (`01 01`) | off | **firmware centring**: back to centre within ~1.5 s |
| released (`01 00`) | off | slack: the wheel stays where it was left |
| released | on | the host's effects |

- The order of the two commands doesn't matter; host off alone (stop
  released) leaves the wheel slack, and sending stop all afterwards starts
  the centring. Free all has no effect on it.
- At power-up the firmware sweeps the wheel full left, full right and back
  to centre, then centres it (stop set, host off).
- Stop all empties the slots: an update, play or free of a slot afterwards
  is acked with status `01` and does nothing, while a create works.

### Effects: `20 02 <type> <op|slot> <params>`

- Types: constant `0x10`, ramp `0x20`, square / sine / triangle / saw up /
  saw down `0x31`-`0x35`, spring / damper / inertia / friction `0x41`-`0x44`.
- Operations: create `0x10`, update `0x00`, play `0x40`, stop `0x80`,
  free `0x20`, in the high nibble; the slot (0-15) in the low nibble.
- Parameters (all 16-bit): magnitudes in ±10000 (100 %), times in ms,
  duration 0 = infinite. Envelope = attack level, attack time, fade level,
  fade time; HORI's driver sends full levels and zero times when there is
  none.
  - constant: level, duration, envelope
  - ramp: start level, end level, duration, envelope
  - periodic: period, magnitude (unsigned), phase (ms), offset, duration,
    envelope
  - condition: negative coefficient, negative saturation, deadband, positive
    coefficient, positive saturation, deadband, centre, duration
- The wheel plays one run of an effect; delays and repeats are the host's to
  time.
- Slots can be created and updated while host control is off, and keep their
  contents across host control off and on (but not across stop all).
  Creating over a slot that is already allocated replaces it.
- Several slots play at once (a constant and a damper together, a constant
  and a spring together).

**Direction:** a positive level turns the wheel **right** (ABS_X rises) and a
negative one **left**. A constant 15 % takes the unheld wheel from centre to
its stop in under a second.

Example, constant force 15 % to the left for 1 s, in slot 0:

```
20 02 10 10 24 fa e8 03 10 27 00 00 10 27   create: level -1500, 1000 ms,
                                            attack 10000 / 0 ms, fade 10000 / 0 ms
20 02 10 40 24 fa e8 03 10 27 00 00 10 27   play (same parameters)
```

### Replies on report `0x20` (input)

```
20 81 <cmd> <arg> <status> <x>          ack of device control 20 01 <cmd> <arg>
20 82 <type> <op|slot> <status> <x>     ack of an effect command
20 03 <type> <slot> 00 ea               effect finished (unsolicited)
```

- Every command is acked within a few ms.
- `<status>`: `00` ok; `01` when the slot is empty (seen for update, play
  and free after stop all).
- `<x>` is usually `ea`, sometimes `00`, on the same command; meaning not
  known.
- `20 03 <type> <slot>` arrives when an effect with a finite duration ends
  (1.0 s after a play with a 1000 ms duration), including each run of a
  repeated effect.
- The hidraw node also carries the controls report `0x01`; it is ignored
  there (the controls are read through evdev).

### Wheel settings

Range, strength and sensitivities are on report `0x21`: write an output
report, then read the reply with GET_REPORT (input). Not used yet.
