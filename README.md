# hori-linux

Force feedback for the **HORI FFB Racing Wheel** (Series X / DLX) on Linux,
with the standard Linux force-feedback interface, as one input device. The
target is MiSTer as well as desktop Linux.

**Status: not started.** Nothing here drives a wheel yet.

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

## The plan

A small **userspace** driver, `hori-ffb`:

1. Grabs the wheel's three input devices exclusively and opens its `hidraw`.
2. Creates **one** virtual wheel with `uinput`: steering, pedals and buttons,
   with the wheel's own vendor and product ID.
3. Offers the standard Linux force-feedback effects on that virtual wheel
   (constant, spring, damper, periodic, gain, autocentre) and translates each
   upload / play / stop into the wheel's `0x20` effect packets.

Userspace rather than a kernel module, so it can't crash the kernel, runs on
any kernel with `uinput` and `hidraw` (desktop and MiSTer alike), and needs
nothing built against a particular kernel tree.

## Steps

1. Confirm the wheel answers the `0x20` effect protocol: one hand-made packet.
2. `hori-ffb` on desktop Linux, tested with `evtest` and `fftest`.
3. Cross-compiled for MiSTer (armhf), with on/off scripts.

## Credits

The vendor protocol was reverse engineered by the
[hid-hori](https://github.com/LinuxGamesTV/hori_control_systems) project
(GPL-2.0), in particular its force feedback pull request #18, for the HORI
Truck Control System (`0f0d:017a`). This project is written from that protocol
description, not from its code.

## Licence

GPL-2.0. See [LICENSE](LICENSE).
