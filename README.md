# hori-linux

Force feedback for the **HORI FFB Racing Wheel** (Series X / DLX) on Linux,
with the standard Linux force-feedback interface, as one input device. The
target is MiSTer as well as desktop Linux.

**Status: `hori-ffb` works on desktop Linux.** Constant, ramp, periodic,
spring, damper, inertia, friction, gain, autocentre and rumble are all
uploaded and acked by the wheel, and constant, sine, damper, gain,
autocentre, delayed and repeated playback measurably move it (see
[Testing](#testing)). The MiSTer build compiles but hasn't been run on a
board yet.

## The problem

In PC mode (USB `0f0d:015d`, a long press of PROFILE switches modes) the wheel
is a plain HID joystick to Linux:

- `hid-generic` binds it. Linux offers **no force feedback** for it on any
  kernel, because its HID description has no standard PID (force feedback)
  section that `hid-pidff` / `hid-universal-pidff` could drive.
- It shows up as **three** input devices: the controls, plus one each for its
  two vendor reports.
- Its force feedback goes through HORI's own protocol on the vendor reports
  `0x20` and `0x21` (see [docs/protocol.md](docs/protocol.md)).

In Xbox mode (`0f0d:015c`) it speaks Microsoft's GIP protocol, which needs the
out-of-tree `xone` driver; this project is about PC mode.

## hori-ffb

A small **userspace** driver. It:

1. Finds the wheel's `hidraw` node and its three input devices, and grabs the
   input devices so nothing else sees them.
2. Creates **one** virtual wheel with `uinput`, named
   "HORI FFB Racing Wheel (hori-ffb)": the same steering, pedals, hat and 19
   buttons, with the wheel's own vendor and product ID (`0f0d:015d`) and
   version `0x8001` (the real wheel reports `0x0111`).
3. Offers the standard Linux force-feedback effects on that virtual wheel and
   translates each upload / play / stop into the wheel's `0x20` effect
   packets. The wheel holds the effects in its 16 slots and plays them
   itself.

Userspace rather than a kernel module, so it can't crash the kernel, runs on
any kernel with `uinput` and `hidraw` (desktop and MiSTer alike), and needs
nothing built against a particular kernel tree.

### What the wheel does when no game is driving it

The wheel's firmware sweeps it full left, full right and back to centre at
power-up, and then holds it centred with its own spring. **hori-ffb keeps
that**:

- Nothing is sent to the wheel when hori-ffb starts, so the power-up sweep
  is never interrupted.
- The firmware keeps the wheel until an application **plays** an effect.
  Uploading one doesn't take control (the wheel accepts uploads either way).
- When no application effect has been playing for 250 ms (`--hold`), the
  wheel goes back to its firmware centring. Games that stop and restart
  effects in quick succession don't get the firmware spring cutting in
  between.
- On exit (Ctrl-C, `SIGTERM`, `SIGHUP`) the wheel goes back to its firmware
  centring, and its slots are emptied.

### Force feedback

| Linux | Wheel |
|---|---|
| `FF_CONSTANT`, `FF_RAMP` | constant `0x10`, ramp `0x20`, with envelope |
| `FF_PERIODIC`: square, sine, triangle, saw up, saw down | `0x31`-`0x35`, with envelope |
| `FF_RUMBLE` | the kernel turns it into a 50 ms sine (`FF_PERIODIC`) |
| `FF_SPRING`, `FF_DAMPER`, `FF_INERTIA`, `FF_FRICTION` | `0x41`-`0x44`, axis 0 (`condition[0]`) only |
| `FF_GAIN` | scales every effect, re-sent to the wheel on change |
| `FF_AUTOCENTER` | a spring in slot 15, played only while an application effect is playing (otherwise the firmware centres the wheel); not scaled by the gain |

15 effects at a time (`ff_effects_max` 15; slot 15 is kept for the
autocentre spring). Start delays and repeat counts are timed by hori-ffb, as
HORI's own Windows driver does.

**Direction.** On this wheel a positive level turns it right and a negative
one left (measured). As in hid-hori, a Linux direction of `0x4000` gives a
negative level, so an `FF_CONSTANT` with direction `0x4000` turns the wheel
**left** and `0xc000` turns it **right**. If a game pulls the wrong way, run
with `--invert`.

## Releases

Ready-built binaries are on the
[releases page](https://github.com/alphanu1/linux-hori/releases):
`hori-ffb-<version>-linux-x86_64` for desktop Linux and
`hori-ffb-<version>-mister-armhf` for MiSTer, both statically linked. A
release is built automatically whenever `VERSION` changes on `main`
(`.github/workflows/release.yml`). `hori-ffb --version` prints the version.

## Build

```
make            # hori-ffb, for this machine
make arm        # hori-ffb-arm, static armhf binary for MiSTer
```

`make arm` needs the `arm-none-linux-gnueabihf-` toolchain (the one MiSTer's
Main is built with, e.g. Arm's gcc-arm-10.2-2020.11) on `PATH`, or:

```
make arm CROSS_COMPILE=/path/to/toolchain/bin/arm-none-linux-gnueabihf-
```

The ARM binary is statically linked, so it doesn't depend on the board's C
library. No dependencies beyond libc, libm and the Linux headers.

## Usage

Run as root (it needs `/dev/hidraw*`, `/dev/input/event*` and `/dev/uinput`):

```
sudo ./hori-ffb [options]
```

| Option | |
|---|---|
| `-v`, `--verbose` | log every packet sent and every ack from the wheel, with timestamps; twice also logs each upload |
| `-i`, `--invert` | mirror all forces left/right |
| `-H`, `--hold MS` | how long to keep the motor after the last effect stops before handing back to the firmware centring (default 250, 0..60000) |
| `-r`, `--reset` | at start, empty the wheel's slots and hand the motor to the firmware: recovers a wheel left limp by a `kill -9`'d hori-ffb. Don't use it while the wheel is doing its power-up sweep. |
| `-d`, `--hidraw PATH` | use this hidraw node instead of searching for `0f0d:015d` |

Games should then use the virtual wheel, "HORI FFB Racing Wheel (hori-ffb)".
SDL games may need `SDL_JOYSTICK_WHEEL_DEVICES=0x0f0d/0x015d` to treat it as
a wheel.

If hori-ffb is killed with `SIGKILL` while an effect plays, the wheel is left
under host control with a slack motor: start hori-ffb with `--reset`, or
replug the wheel.

The wheel must be plugged in, in PC mode, when hori-ffb starts. It exits
when the wheel is unplugged (handled in the code, not yet tried).

## MiSTer

- Copy `hori-ffb-arm` to the board and run it as root (MiSTer runs as root).
  It needs `/dev/uinput` (MiSTer's Main itself uses uinput, so the kernel has
  it) and `/dev/hidraw*` (not yet checked on the MiSTer kernel).
- The virtual wheel keeps `0f0d:015d`, so a Main with the HORI wheel entry
  (`setup_wheels()` in `input.cpp`) maps it as a wheel.
- Main also lists the three grabbed original devices, which stay silent;
  the virtual wheel is the one that moves.
- Main grabs input devices while the OSD is open. If it holds the wheel's
  devices when hori-ffb starts, the grab fails and hori-ffb exits: start it
  before Main opens the wheel, or with the OSD closed.
- Main sends rumble to wheels as `FF_RUMBLE` (direction `0x4000`), which
  hori-ffb plays as a 50 ms sine. It sends no autocentre or spring to this
  wheel, so the firmware centring stays in charge between rumbles.
- Start/stop scripts for MiSTer aren't written yet.

## Testing

`tools/ffcheck` (`make tools/ffcheck`) plays weak effects (20 % at most) on
the virtual wheel and prints how the steering axis moved:

```
sudo ./hori-ffb -v &
sudo tools/ffcheck              # or name tests: spring left right sine rumble
                                # damper gain autocentre replay update repeat
```

Measured on 2026-10-26, wheel unheld, ABS_X 0..65535 with centre 32768:

| Test | ABS_X |
|---|---|
| constant 15 %, direction `0x4000` | 32795 to 220 in 1 s (left stop) |
| constant 15 %, direction `0xc000` | 32642 to 65328 (right stop) |
| after each: release to the firmware | back to 32642 / 32890 within 1.5 s |
| constant 15 % + damper 20 % | 32897 to 16648 (half as far as without) |
| constant 15 % at gain 25 % | 32645 to 43917 |
| constant 15 % + autocentre 20 % | 32879 to 9922 (spring holds it off the stop) |
| sine 15 %, 250 ms | 31792 to 33513 around centre |
| the same effect played, handed back, played again | 32651 to 16664, centred, 32646 to 16835 |
| constant left, re-uploaded as right while playing | 32764 to 18670, then back up to 40383 |
| 300 ms, delay 300 ms, played 3 times | three plays 300 ms apart, to 64507 |
| rumble 20 % (50 ms sine) | acked and played to the end; too fast to move ABS_X |

Every packet sent in these runs was acked (69 of 69 over the full set).

For a full test by hand, `fftest` from linuxconsoletools works on the
virtual wheel's `/dev/input/eventN`. **Hold the wheel:** fftest's effects
are much stronger than ffcheck's.

## Credits

The vendor protocol was reverse engineered by the
[hid-hori](https://github.com/LinuxGamesTV/hori_control_systems) project
(GPL-2.0), in particular its force feedback
[pull request #18](https://github.com/LinuxGamesTV/hori_control_systems/pull/18),
for the HORI Truck Control System (`0f0d:017a`): the effect packets, their
parameters and scaling, and the direction mapping. This project is written
from that protocol description, not from its code.

## Licence

GPL-2.0. See [LICENSE](LICENSE).
