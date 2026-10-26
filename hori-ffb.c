// SPDX-License-Identifier: GPL-2.0
/*
 * hori-ffb: userspace force feedback driver for the HORI FFB Racing Wheel
 * in PC mode (USB 0f0d:015d).
 *
 * The wheel's three input devices (controls plus two vendor reports) are
 * grabbed, and one virtual wheel is created with uinput: the same buttons
 * and axes, plus the standard Linux force feedback interface. Effects
 * uploaded to the virtual wheel are translated into the wheel's own effect
 * protocol (vendor report 0x20) and written to its hidraw node; the wheel
 * keeps up to 16 effects in slots and plays them itself.
 *
 * The protocol was reverse engineered by the hid-hori project
 * (https://github.com/LinuxGamesTV/hori_control_systems, GPL-2.0), pull
 * request #18, for the HORI Truck Control System. This driver is written
 * from that protocol description; see docs/protocol.md.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <linux/uinput.h>

#define HORI_VID	0x0f0d
#define HORI_PID	0x015d
#define HID_ID_STR	"HID_ID=0003:00000F0D:0000015D"
#define VIRT_VERSION	0x8001		/* the real wheel reports 0x0111 */
#define VIRT_NAME	"HORI FFB Racing Wheel (hori-ffb)"

/* ---- wheel protocol (docs/protocol.md) ---------------------------------- */

#define RPT_ID		0x20
#define RPT_LEN		32		/* report id + 31 bytes */
#define FULL		10000		/* device units for 100 % */

#define CLS_DEVCTL	0x01
#define CLS_EFFECT	0x02

#define DEV_STOPALL	0x01		/* arg 1 = stop all, 0 = release */
#define DEV_HOST	0x02		/* arg 1 = host drives the motor */
#define DEV_FREEALL	0x03		/* arg 1 = free every slot */

#define OP_UPDATE	0x00
#define OP_CREATE	0x10
#define OP_FREE		0x20
#define OP_PLAY		0x40
#define OP_STOP		0x80

#define SLOTS		16
#define USER_EFFECTS	15		/* slot 15 holds the autocentre spring */
#define AC_SLOT		15

/* ---- state -------------------------------------------------------------- */

struct slot {
	struct ff_effect eff;
	bool uploaded;			/* the application has this effect */
	bool on_wheel;			/* the wheel holds it in its slot */
	bool active;
	bool waiting_delay;
	unsigned int repeats;		/* plays left, including the current one */
	long long deadline;		/* ms, CLOCK_MONOTONIC; 0 = no timer */
};

static int verbose;
static bool invert;
static bool reset_at_start;
static int hold_ms = 250;
static volatile sig_atomic_t quit;

static int hid_fd = -1, ui_fd = -1;
static int ev_fd[3] = { -1, -1, -1 };
static int ev_count, ev_main = -1;
static bool wheel_gone;

static struct slot slot[SLOTS];
static unsigned int gain = 0xffff;
static unsigned int ac_level;		/* FF_AUTOCENTER from the application */
static bool motor_on, cleaned;
static long long release_at;		/* ms; 0 = no release pending */
static long long t_start;
static unsigned long sent, acked;

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void logv(int level, const char *fmt, ...)
{
	va_list ap;

	if (verbose < level)
		return;
	fprintf(stderr, "%7.3f ", (now_ms() - t_start) / 1000.0);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void hexdump(const char *pfx, const uint8_t *p, int n, const char *what)
{
	char buf[3 * RPT_LEN + 1];
	int i, len = 0;

	while (n > 6 && p[n - 1] == 0)	/* trim the zero padding */
		n--;
	for (i = 0; i < n; i++)
		len += sprintf(buf + len, "%s%02x", i ? " " : "", p[i]);
	logv(1, "%s %-48s %s\n", pfx, buf, what ? what : "");
}

static void put16(uint8_t *p, long v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
}

static void send_report(const uint8_t *r, const char *what)
{
	if (wheel_gone)
		return;
	if (verbose)
		hexdump("->", r, RPT_LEN, what);
	if (write(hid_fd, r, RPT_LEN) != RPT_LEN) {
		if (errno == ENODEV || errno == EIO) {
			fprintf(stderr, "hori-ffb: wheel gone (%s)\n", strerror(errno));
			wheel_gone = true;
			quit = 1;
		} else {
			fprintf(stderr, "hori-ffb: hidraw write: %s\n", strerror(errno));
		}
		return;
	}
	sent++;
}

static void devctl(uint8_t cmd, uint8_t arg, const char *what)
{
	uint8_t r[RPT_LEN] = { RPT_ID, CLS_DEVCTL, cmd, arg };

	send_report(r, what);
}

/*
 * Who drives the motor. While idle the wheel is left to its firmware, which
 * centres it (and sweeps it at power-up); host control is taken only while
 * an application effect is playing. Measured on the wheel (docs/protocol.md):
 *
 * - the firmware centring runs while the stop flag is set (20 01 01 01) and
 *   host control is off; host control off alone leaves the motor slack
 * - "stop all" also empties every slot (later updates and plays of them are
 *   acked with status 01), so effects are created again after a release
 * - slots can be created while host control is off; host off/on keeps them
 */
static void slot_op(int idx, uint8_t op);

/* make sure the wheel holds the effect in slot idx */
static void slot_load(int idx)
{
	if (!slot[idx].on_wheel) {
		slot_op(idx, OP_CREATE);
		slot[idx].on_wheel = true;
	}
}

/* once per run, before the first effect goes to the wheel */
static void ensure_clean(void)
{
	if (cleaned)
		return;
	devctl(DEV_FREEALL, 1, "free all slots");
	cleaned = true;
}

static void motor_take(void)
{
	release_at = 0;
	if (motor_on)
		return;
	devctl(DEV_STOPALL, 0, "release stop");
	devctl(DEV_HOST, 1, "host control on");
	motor_on = true;
	if (ac_level) {
		slot_load(AC_SLOT);
		slot_op(AC_SLOT, OP_PLAY);
	}
}

static void motor_release(void)
{
	int i;

	release_at = 0;
	if (!motor_on)
		return;
	devctl(DEV_STOPALL, 1, "stop all");
	devctl(DEV_HOST, 0, "host control off (firmware centring)");
	motor_on = false;
	for (i = 0; i < SLOTS; i++)
		slot[i].on_wheel = false;	/* stop all emptied them */
}

static bool any_playing(void)
{
	int i;

	for (i = 0; i < USER_EFFECTS; i++)
		if (slot[i].active && !slot[i].waiting_delay)
			return true;
	return false;
}

/*
 * Nothing playing: give the wheel back to the firmware after a short hold,
 * so a game that stops and restarts its effects doesn't make the firmware
 * centring cut in between.
 */
static void check_idle(void)
{
	if (motor_on && !release_at && !any_playing())
		release_at = now_ms() + hold_ms;
}

/* ---- effect encoding ---------------------------------------------------- */

/*
 * Scale v in [-in_max, in_max] to device units [-10000, 10000], times the
 * gain (0..0xffff), rounded and clamped.
 */
static long scale(double v, double in_max, unsigned int g)
{
	double r = v * FULL / in_max * g / 0xffff;

	r = r < 0 ? r - 0.5 : r + 0.5;
	if (r > FULL)
		r = FULL;
	if (r < -FULL)
		r = -FULL;
	return (long)r;
}

/*
 * How much of an effect's direction lies along the steering axis, -1..1.
 * Linux directions run 0 (down) .. 0x4000 (left) .. 0x8000 (up) ..
 * 0xc000 (right). Following hid-hori (which matched HORI's DirectInput
 * driver), 0x4000 becomes a negative level. On this wheel a negative level
 * turns it left (ABS_X falls) and a positive one right (measured), so a
 * force with direction 0x4000 turns the wheel left. --invert mirrors it.
 */
static double dir_factor(uint16_t direction)
{
	double f = -sin(direction * (2.0 * M_PI / 65536.0));

	if (fabs(f) < 1e-9)
		f = 0;
	return invert ? -f : f;
}

static const char *const type_name[] = {
	[FF_CONSTANT - FF_RUMBLE] = "constant",
	[FF_RAMP - FF_RUMBLE] = "ramp",
	[FF_PERIODIC - FF_RUMBLE] = "periodic",
	[FF_SPRING - FF_RUMBLE] = "spring",
	[FF_DAMPER - FF_RUMBLE] = "damper",
	[FF_INERTIA - FF_RUMBLE] = "inertia",
	[FF_FRICTION - FF_RUMBLE] = "friction",
	[FF_RUMBLE - FF_RUMBLE] = "rumble",
};

static const char *op_name(uint8_t op)
{
	switch (op) {
	case OP_UPDATE:	return "update";
	case OP_CREATE:	return "create";
	case OP_FREE:	return "free";
	case OP_PLAY:	return "play";
	case OP_STOP:	return "stop";
	}
	return "?";
}

/* envelope: attack level, attack time, fade level, fade time */
static void put_envelope(uint8_t *p, const struct ff_envelope *env)
{
	/* without an attack/fade, HORI's driver sends full level, zero time */
	unsigned int al = env->attack_length ? env->attack_level : 0x7fff;
	unsigned int fl = env->fade_length ? env->fade_level : 0x7fff;

	put16(p, scale(al, 0x7fff, gain));
	put16(p + 2, env->attack_length);
	put16(p + 4, scale(fl, 0x7fff, gain));
	put16(p + 6, env->fade_length);
}

/* build `20 02 <type> <op|slot> <params>`; returns 0 or -EINVAL */
static int build_effect(const struct ff_effect *e, int idx, uint8_t op,
			unsigned int g, uint8_t *r)
{
	uint8_t *p = r + 4;
	unsigned int dur = e->replay.length;	/* ms, 0 = infinite */
	double f;

	memset(r, 0, RPT_LEN);
	r[0] = RPT_ID;
	r[1] = CLS_EFFECT;
	r[3] = op | idx;

	switch (e->type) {
	case FF_CONSTANT:
		f = dir_factor(e->direction);
		r[2] = 0x10;
		put16(p, scale(e->u.constant.level * f, 0x7fff, g));
		put16(p + 2, dur);
		put_envelope(p + 4, &e->u.constant.envelope);
		break;

	case FF_RAMP:
		f = dir_factor(e->direction);
		r[2] = 0x20;
		put16(p, scale(e->u.ramp.start_level * f, 0x7fff, g));
		put16(p + 2, scale(e->u.ramp.end_level * f, 0x7fff, g));
		put16(p + 4, dur);
		put_envelope(p + 6, &e->u.ramp.envelope);
		break;

	case FF_PERIODIC: {
		const struct ff_periodic_effect *pe = &e->u.periodic;
		unsigned int period, phase = pe->phase;
		long mag = pe->magnitude, offset = pe->offset;

		switch (pe->waveform) {
		case FF_SQUARE:		r[2] = 0x31; break;
		case FF_SINE:		r[2] = 0x32; break;
		case FF_TRIANGLE:	r[2] = 0x33; break;
		case FF_SAW_UP:		r[2] = 0x34; break;
		case FF_SAW_DOWN:	r[2] = 0x35; break;
		default:
			return -EINVAL;
		}
		/* period in ms, kept to what HORI's driver allows */
		period = pe->period ? pe->period : 1;
		if (pe->period && period < 4)
			period = 4;
		if (period > 10000)
			period = 10000;
		/*
		 * A vibration uses only the side of its direction, not the
		 * projection, so one along the vertical axis still plays.
		 */
		if (dir_factor(e->direction) < 0) {
			mag = -mag;
			offset = -offset;
		}
		if (mag < 0) {		/* the wheel's magnitude is unsigned */
			mag = -mag;
			phase = (phase + 0x8000) & 0xffff;
		}
		put16(p, period);
		put16(p + 2, scale(mag, 0x7fff, g));
		put16(p + 4, (unsigned long)period * phase >> 16);	/* ms */
		put16(p + 6, scale(offset, 0x7fff, g));
		put16(p + 8, dur);
		put_envelope(p + 10, &pe->envelope);
		break;
	}

	case FF_SPRING:
	case FF_DAMPER:
	case FF_INERTIA:
	case FF_FRICTION: {
		/* the wheel has one axis: condition[0] (x) */
		const struct ff_condition_effect *c = &e->u.condition[0];
		long lc = c->left_coeff, rc = c->right_coeff;
		unsigned long ls = c->left_saturation, rs = c->right_saturation;
		long centre = c->center, t;
		long db = (long)c->deadband * FULL / 0xffff;

		r[2] = e->type == FF_SPRING ? 0x41 :
		       e->type == FF_DAMPER ? 0x42 :
		       e->type == FF_INERTIA ? 0x43 : 0x44;
		if (invert) {
			t = lc; lc = rc; rc = t;
			t = ls; ls = rs; rs = t;
			centre = -centre;
		}
		put16(p, scale(lc, 0x7fff, g));
		put16(p + 2, scale(ls, 0xffff, g));
		put16(p + 4, db);
		put16(p + 6, scale(rc, 0x7fff, g));
		put16(p + 8, scale(rs, 0xffff, g));
		put16(p + 10, db);
		put16(p + 12, scale(centre, 0x7fff, 0xffff));
		put16(p + 14, dur);
		break;
	}

	default:
		return -EINVAL;
	}
	return 0;
}

static void slot_op(int idx, uint8_t op)
{
	uint8_t r[RPT_LEN];
	char what[48];
	const struct ff_effect *e = &slot[idx].eff;
	unsigned int g = idx == AC_SLOT ? 0xffff : gain;

	if (build_effect(e, idx, op, g, r))
		return;
	snprintf(what, sizeof(what), "%s %s slot %d",
		 idx == AC_SLOT ? "autocentre" : type_name[e->type - FF_RUMBLE],
		 op_name(op), idx);
	send_report(r, what);
}

/* ---- force feedback requests from the virtual device ------------------- */

static int ff_upload(struct ff_effect *e)
{
	struct slot *s;
	uint8_t r[RPT_LEN];

	if (e->id < 0 || e->id >= USER_EFFECTS)
		return -EINVAL;
	s = &slot[e->id];
	if (build_effect(e, e->id, OP_CREATE, gain, r))
		return -EINVAL;
	logv(2, "upload id %d type %#x dir %#06x len %u delay %u\n", e->id,
	     e->type, e->direction, e->replay.length, e->replay.delay);
	/* host control stays as it is: the wheel accepts uploads either way */
	ensure_clean();
	s->eff = *e;
	s->eff.u.periodic.custom_data = NULL;	/* not ours to keep */
	slot_op(e->id, s->on_wheel ? OP_UPDATE : OP_CREATE);
	s->on_wheel = true;
	s->uploaded = true;
	return 0;
}

static int ff_erase(int id)
{
	struct slot *s;

	if (id < 0 || id >= USER_EFFECTS)
		return -EINVAL;
	s = &slot[id];
	if (s->uploaded) {
		s->active = false;
		s->deadline = 0;
		if (s->on_wheel)
			slot_op(id, OP_FREE);
		s->on_wheel = false;
		s->uploaded = false;
		check_idle();
	}
	return 0;
}

/*
 * The wheel plays one run of an effect (and reports `20 03 <type> <slot>`
 * when it ends). Start delays, repeats and the end of play are timed here,
 * as HORI's driver does, so the motor can be handed back when idle.
 */
static void slot_start(int id)
{
	struct slot *s = &slot[id];

	s->waiting_delay = false;
	s->deadline = 0;
	motor_take();
	slot_load(id);
	slot_op(id, OP_PLAY);
	if (s->eff.replay.length)
		s->deadline = now_ms() + s->eff.replay.length;
}

static void ff_play(int id, int value)
{
	struct slot *s;

	if (id < 0 || id >= USER_EFFECTS || !slot[id].uploaded)
		return;
	s = &slot[id];
	s->deadline = 0;
	if (value > 0) {
		s->active = true;
		s->repeats = value;
		if (s->eff.replay.delay) {
			s->waiting_delay = true;
			s->deadline = now_ms() + s->eff.replay.delay;
		} else {
			slot_start(id);
		}
	} else {
		bool was = s->active;

		s->active = false;
		s->waiting_delay = false;
		if (was && motor_on && s->on_wheel)
			slot_op(id, OP_STOP);
		check_idle();
	}
}

static void slot_timer(int id)
{
	struct slot *s = &slot[id];

	s->deadline = 0;
	if (!s->active || !s->uploaded)
		return;
	if (s->waiting_delay) {
		slot_start(id);
	} else if (s->repeats > 1) {
		s->repeats--;
		motor_take();
		slot_load(id);
		slot_op(id, OP_PLAY);
		s->deadline = now_ms() + s->eff.replay.length;
	} else {
		s->active = false;	/* the wheel has finished it */
		check_idle();
	}
}

static void ff_set_gain(unsigned int g)
{
	int i;

	gain = g > 0xffff ? 0xffff : g;
	logv(1, "   gain %u\n", gain);
	for (i = 0; i < USER_EFFECTS; i++)
		if (slot[i].uploaded && slot[i].on_wheel)
			slot_op(i, OP_UPDATE);	/* the rest get it when created */
}

/*
 * Autocentre: a spring in the reserved slot, not scaled by the gain. It
 * plays only while host control is on (an application effect is playing);
 * when idle the firmware's own centring does the job.
 */
static void ff_set_autocentre(unsigned int value)
{
	struct slot *s = &slot[AC_SLOT];

	logv(1, "   autocentre %u\n", value);
	if (value == ac_level)
		return;
	ac_level = value;
	if (value) {
		struct ff_condition_effect *c = &s->eff.u.condition[0];

		memset(&s->eff, 0, sizeof(s->eff));
		s->eff.type = FF_SPRING;
		c->left_coeff = c->right_coeff = value >> 1;
		c->left_saturation = c->right_saturation = 0xffff;
		if (s->on_wheel) {
			slot_op(AC_SLOT, OP_UPDATE);
		} else if (motor_on) {
			slot_load(AC_SLOT);
			slot_op(AC_SLOT, OP_PLAY);
		}
		/* else: created and played when host control is next taken */
	} else if (s->on_wheel) {
		if (motor_on)
			slot_op(AC_SLOT, OP_STOP);
		slot_op(AC_SLOT, OP_FREE);
		s->on_wheel = false;
	}
}

static void handle_uinput(void)
{
	struct input_event ev[32];
	ssize_t n;
	int i;

	n = read(ui_fd, ev, sizeof(ev));
	if (n < 0) {
		if (errno != EAGAIN && errno != EINTR) {
			perror("hori-ffb: uinput read");
			quit = 1;
		}
		return;
	}
	for (i = 0; i < n / (ssize_t)sizeof(ev[0]); i++) {
		if (ev[i].type == EV_UINPUT && ev[i].code == UI_FF_UPLOAD) {
			struct uinput_ff_upload up;

			memset(&up, 0, sizeof(up));
			up.request_id = ev[i].value;
			if (ioctl(ui_fd, UI_BEGIN_FF_UPLOAD, &up) < 0) {
				perror("hori-ffb: UI_BEGIN_FF_UPLOAD");
				continue;
			}
			up.retval = ff_upload(&up.effect);
			if (ioctl(ui_fd, UI_END_FF_UPLOAD, &up) < 0)
				perror("hori-ffb: UI_END_FF_UPLOAD");
		} else if (ev[i].type == EV_UINPUT && ev[i].code == UI_FF_ERASE) {
			struct uinput_ff_erase er;

			memset(&er, 0, sizeof(er));
			er.request_id = ev[i].value;
			if (ioctl(ui_fd, UI_BEGIN_FF_ERASE, &er) < 0) {
				perror("hori-ffb: UI_BEGIN_FF_ERASE");
				continue;
			}
			er.retval = ff_erase(er.effect_id);
			if (ioctl(ui_fd, UI_END_FF_ERASE, &er) < 0)
				perror("hori-ffb: UI_END_FF_ERASE");
		} else if (ev[i].type == EV_FF) {
			if (ev[i].code == FF_GAIN)
				ff_set_gain(ev[i].value);
			else if (ev[i].code == FF_AUTOCENTER)
				ff_set_autocentre(ev[i].value);
			else
				ff_play(ev[i].code, ev[i].value);
		}
	}
}

/* ---- hidraw acks -------------------------------------------------------- */

static void handle_hidraw(void)
{
	uint8_t buf[64];
	ssize_t n;

	for (;;) {
		n = read(hid_fd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR)
				return;
			fprintf(stderr, "hori-ffb: hidraw read: %s\n", strerror(errno));
			wheel_gone = true;
			quit = 1;
			return;
		}
		if (n == 0)
			return;
		if (buf[0] == RPT_ID && n >= 2 && (buf[1] & 0x80)) {
			acked++;
			if (verbose) {
				char what[80] = "";

				if (buf[1] == 0x82 && n >= 5)
					snprintf(what, sizeof(what), "ack effect %02x %s slot %d%s",
						 buf[2], op_name(buf[3] & 0xf0), buf[3] & 0x0f,
						 buf[4] ? " (status: no such effect)" : "");
				else if (buf[1] == 0x81)
					snprintf(what, sizeof(what), "ack devctl");
				hexdump("<-", buf, n < 8 ? n : 8, what);
			}
		} else if (buf[0] == RPT_ID && n >= 4 && buf[1] == 0x03) {
			if (verbose) {
				char what[48];

				snprintf(what, sizeof(what), "effect %02x slot %d finished",
					 buf[2], buf[3] & 0x0f);
				hexdump("<-", buf, n < 8 ? n : 8, what);
			}
		} else if (buf[0] == RPT_ID || buf[0] == 0x21) {
			if (verbose)
				hexdump("<-", buf, n < 16 ? n : 16, "(vendor report)");
		}
		/* report 0x01 (the controls) arrives through evdev instead */
	}
}

/* ---- input forwarding --------------------------------------------------- */

static void emit_state(void)
{
	struct input_event out[KEY_CNT / 8 + ABS_CNT + 1];
	unsigned long keybits[KEY_CNT / (8 * sizeof(long)) + 1] = { 0 };
	unsigned long keystate[KEY_CNT / (8 * sizeof(long)) + 1] = { 0 };
	unsigned long absbits[ABS_CNT / (8 * sizeof(long)) + 1] = { 0 };
	int fd = ev_fd[ev_main], code, n = 0;
	const int lb = 8 * sizeof(long);

	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
	ioctl(fd, EVIOCGKEY(sizeof(keystate)), keystate);
	ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);
	memset(out, 0, sizeof(out));
	for (code = 0; code < KEY_CNT && n < (int)(sizeof(out) / sizeof(out[0])) - 1; code++)
		if ((keybits[code / lb] >> (code % lb)) & 1) {
			out[n].type = EV_KEY;
			out[n].code = code;
			out[n++].value = (keystate[code / lb] >> (code % lb)) & 1;
		}
	for (code = 0; code < ABS_CNT && n < (int)(sizeof(out) / sizeof(out[0])) - 1; code++) {
		struct input_absinfo ai;

		if (code == ABS_MISC || !((absbits[code / lb] >> (code % lb)) & 1))
			continue;
		if (ioctl(fd, EVIOCGABS(code), &ai) < 0)
			continue;
		out[n].type = EV_ABS;
		out[n].code = code;
		out[n++].value = ai.value;
	}
	out[n].type = EV_SYN;
	out[n++].code = SYN_REPORT;
	if (write(ui_fd, out, n * sizeof(out[0])) < 0)
		perror("hori-ffb: uinput write");
}

static void handle_evdev(int i)
{
	static bool dropped;
	struct input_event ev[64], out[64];
	ssize_t n;
	int k, m = 0;

	n = read(ev_fd[i], ev, sizeof(ev));
	if (n < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return;
		fprintf(stderr, "hori-ffb: evdev read: %s\n", strerror(errno));
		wheel_gone = true;
		quit = 1;
		return;
	}
	if (i != ev_main)
		return;			/* vendor nodes: ABS_MISC only, dropped */
	for (k = 0; k < n / (ssize_t)sizeof(ev[0]); k++) {
		const struct input_event *e = &ev[k];

		if (e->type == EV_SYN && e->code == SYN_DROPPED) {
			dropped = true;
			continue;
		}
		if (dropped) {
			if (e->type == EV_SYN && e->code == SYN_REPORT) {
				dropped = false;
				emit_state();
			}
			continue;
		}
		if (e->type == EV_KEY || (e->type == EV_ABS && e->code != ABS_MISC) ||
		    (e->type == EV_SYN && e->code == SYN_REPORT))
			out[m++] = *e;
	}
	if (m && write(ui_fd, out, m * sizeof(out[0])) < 0)
		perror("hori-ffb: uinput write");
}

/* ---- device discovery --------------------------------------------------- */

static bool read_line_match(const char *path, const char *needle)
{
	char line[256];
	bool found = false;
	FILE *f = fopen(path, "r");

	if (!f)
		return false;
	while (!found && fgets(line, sizeof(line), f))
		found = !strncasecmp(line, needle, strlen(needle));
	fclose(f);
	return found;
}

/* find the wheel's hidraw node; fills the HID device's sysfs path */
static int find_hidraw(char *dev, size_t devlen, char *hidsys)
{
	DIR *d = opendir("/sys/class/hidraw");
	struct dirent *de;
	char path[PATH_MAX];
	int found = -1;

	if (!d)
		return -1;
	while (found < 0 && (de = readdir(d))) {
		if (strncmp(de->d_name, "hidraw", 6))
			continue;
		snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", de->d_name);
		if (!read_line_match(path, HID_ID_STR))
			continue;
		snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device", de->d_name);
		if (!realpath(path, hidsys))
			continue;
		snprintf(dev, devlen, "/dev/%s", de->d_name);
		found = 0;
	}
	closedir(d);
	return found;
}

/* the HID device behind a hidraw node given on the command line */
static int hidsys_of(const char *dev, char *hidsys)
{
	char path[PATH_MAX];
	const char *base = strrchr(dev, '/');

	snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device", base ? base + 1 : dev);
	return realpath(path, hidsys) ? 0 : -1;
}

static bool has_bit(int fd, int type, int code)
{
	unsigned long bits[KEY_CNT / (8 * sizeof(long)) + 1] = { 0 };
	const int lb = 8 * sizeof(long);

	if (ioctl(fd, EVIOCGBIT(type, sizeof(bits)), bits) < 0)
		return false;
	return (bits[code / lb] >> (code % lb)) & 1;
}

/* open and grab every evdev node of the HID device; pick the controls one */
static int open_evdevs(const char *hidsys)
{
	DIR *d = opendir("/sys/class/input");
	struct dirent *de;
	char path[PATH_MAX], real[PATH_MAX], dev[PATH_MAX];
	size_t hl = strlen(hidsys);

	if (!d) {
		perror("hori-ffb: /sys/class/input");
		return -1;
	}
	while ((de = readdir(d))) {
		int fd;

		if (strncmp(de->d_name, "event", 5))
			continue;
		/* /sys/class/input/eventN/device is .../<hid device>/input/inputM */
		snprintf(path, sizeof(path), "/sys/class/input/%s/device", de->d_name);
		if (!realpath(path, real) || strncmp(real, hidsys, hl) || real[hl] != '/')
			continue;
		if (ev_count == 3) {
			fprintf(stderr, "hori-ffb: more than three input nodes, ignoring %s\n",
				de->d_name);
			continue;
		}
		snprintf(dev, sizeof(dev), "/dev/input/%s", de->d_name);
		fd = open(dev, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) {
			fprintf(stderr, "hori-ffb: %s: %s\n", dev, strerror(errno));
			closedir(d);
			return -1;
		}
		if (ioctl(fd, EVIOCGRAB, 1) < 0) {
			fprintf(stderr, "hori-ffb: grab %s: %s\n", dev, strerror(errno));
			close(fd);
			closedir(d);
			return -1;
		}
		if (has_bit(fd, EV_ABS, ABS_X) && has_bit(fd, EV_KEY, BTN_TRIGGER))
			ev_main = ev_count;
		logv(1, "grabbed %s%s\n", dev, ev_main == ev_count ? " (controls)" : "");
		ev_fd[ev_count++] = fd;
	}
	closedir(d);
	if (ev_main < 0) {
		fprintf(stderr, "hori-ffb: no controls input node found for %s\n", hidsys);
		return -1;
	}
	return 0;
}

/* ---- the virtual wheel -------------------------------------------------- */

static int create_uinput(const char *hidraw)
{
	static const int ff_bits[] = {
		FF_CONSTANT, FF_RAMP, FF_PERIODIC, FF_SQUARE, FF_SINE,
		FF_TRIANGLE, FF_SAW_UP, FF_SAW_DOWN, FF_SPRING, FF_DAMPER,
		FF_INERTIA, FF_FRICTION, FF_GAIN, FF_AUTOCENTER,
	};
	unsigned long keybits[KEY_CNT / (8 * sizeof(long)) + 1] = { 0 };
	unsigned long absbits[ABS_CNT / (8 * sizeof(long)) + 1] = { 0 };
	const int lb = 8 * sizeof(long);
	int src = ev_fd[ev_main], code, nkeys = 0, nabs = 0;
	struct uinput_setup us;
	char phys[64];
	size_t i;

	ui_fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (ui_fd < 0) {
		perror("hori-ffb: /dev/uinput");
		return -1;
	}
	ioctl(src, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
	ioctl(src, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits);

	if (ioctl(ui_fd, UI_SET_EVBIT, EV_SYN) < 0 ||
	    ioctl(ui_fd, UI_SET_EVBIT, EV_KEY) < 0 ||
	    ioctl(ui_fd, UI_SET_EVBIT, EV_ABS) < 0 ||
	    ioctl(ui_fd, UI_SET_EVBIT, EV_FF) < 0)
		goto fail;
	for (code = 0; code < KEY_CNT; code++)
		if ((keybits[code / lb] >> (code % lb)) & 1) {
			if (ioctl(ui_fd, UI_SET_KEYBIT, code) < 0)
				goto fail;
			nkeys++;
		}
	for (code = 0; code < ABS_CNT; code++) {
		struct uinput_abs_setup as;

		if (code == ABS_MISC || !((absbits[code / lb] >> (code % lb)) & 1))
			continue;
		memset(&as, 0, sizeof(as));
		as.code = code;
		if (ioctl(src, EVIOCGABS(code), &as.absinfo) < 0)
			goto fail;
		if (ioctl(ui_fd, UI_SET_ABSBIT, code) < 0 ||
		    ioctl(ui_fd, UI_ABS_SETUP, &as) < 0)
			goto fail;
		nabs++;
	}
	for (i = 0; i < sizeof(ff_bits) / sizeof(ff_bits[0]); i++)
		if (ioctl(ui_fd, UI_SET_FFBIT, ff_bits[i]) < 0)
			goto fail;

	snprintf(phys, sizeof(phys), "hori-ffb/%s",
		 strrchr(hidraw, '/') ? strrchr(hidraw, '/') + 1 : hidraw);
	ioctl(ui_fd, UI_SET_PHYS, phys);	/* informational only */

	memset(&us, 0, sizeof(us));
	us.id.bustype = BUS_USB;
	us.id.vendor = HORI_VID;
	us.id.product = HORI_PID;
	us.id.version = VIRT_VERSION;
	snprintf(us.name, sizeof(us.name), "%s", VIRT_NAME);
	us.ff_effects_max = USER_EFFECTS;
	if (ioctl(ui_fd, UI_DEV_SETUP, &us) < 0 || ioctl(ui_fd, UI_DEV_CREATE) < 0)
		goto fail;
	logv(1, "virtual wheel created: %d buttons, %d axes, %d effects\n",
	     nkeys, nabs, USER_EFFECTS);
	return 0;
fail:
	perror("hori-ffb: uinput setup");
	close(ui_fd);
	ui_fd = -1;
	return -1;
}

/* ---- main --------------------------------------------------------------- */

static void on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

static void usage(FILE *f)
{
	fprintf(f,
"usage: hori-ffb [options]\n"
"Force feedback for the HORI FFB Racing Wheel (PC mode, 0f0d:015d).\n"
"\n"
"  -v, --verbose      log every packet sent and every ack (twice: more)\n"
"  -i, --invert       mirror all forces left/right\n"
"  -H, --hold MS      keep host control this long after the last effect\n"
"                     stops, before handing back to the wheel's own\n"
"                     centring (default 250)\n"
"  -r, --reset        at start, free all slots and hand the motor to the\n"
"                     firmware (recovers from a killed hori-ffb; don't use\n"
"                     while the wheel is doing its power-up sweep)\n"
"  -d, --hidraw PATH  use this hidraw node instead of searching\n"
"  -h, --help         this text\n");
}

int main(int argc, char **argv)
{
	static const struct option lopts[] = {
		{ "verbose", no_argument, NULL, 'v' },
		{ "invert", no_argument, NULL, 'i' },
		{ "hold", required_argument, NULL, 'H' },
		{ "reset", no_argument, NULL, 'r' },
		{ "hidraw", required_argument, NULL, 'd' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	char hidraw[PATH_MAX] = "", hidsys[PATH_MAX];
	struct sigaction sa;
	int opt, i, ret = 1;

	t_start = now_ms();
	while ((opt = getopt_long(argc, argv, "viH:rd:h", lopts, NULL)) != -1) {
		switch (opt) {
		case 'v': verbose++; break;
		case 'i': invert = true; break;
		case 'H':
			hold_ms = atoi(optarg);
			if (hold_ms < 0 || hold_ms > 60000) {
				fprintf(stderr, "hori-ffb: --hold takes 0..60000 ms\n");
				return 2;
			}
			break;
		case 'r': reset_at_start = true; break;
		case 'd': snprintf(hidraw, sizeof(hidraw), "%s", optarg); break;
		case 'h': usage(stdout); return 0;
		default: usage(stderr); return 2;
		}
	}
	setvbuf(stderr, NULL, _IOLBF, 0);

	if (hidraw[0] ? hidsys_of(hidraw, hidsys) : find_hidraw(hidraw, sizeof(hidraw), hidsys)) {
		fprintf(stderr, "hori-ffb: no HORI FFB Racing Wheel (0f0d:015d) found."
			" Is it in PC mode?\n");
		return 1;
	}
	logv(1, "wheel: %s (%s)\n", hidraw, hidsys);
	hid_fd = open(hidraw, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (hid_fd < 0) {
		fprintf(stderr, "hori-ffb: %s: %s\n", hidraw, strerror(errno));
		return 1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;	/* no SA_RESTART: poll returns EINTR */
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	if (open_evdevs(hidsys) || create_uinput(hidraw))
		goto out;
	emit_state();

	/*
	 * Nothing is sent at start: the wheel stays with its firmware (power-up
	 * sweep, centring) until an application plays an effect.
	 */
	if (reset_at_start) {
		ensure_clean();
		motor_on = true;
		motor_release();
	}
	fprintf(stderr, "hori-ffb: running on %s%s\n", hidraw, invert ? " (inverted)" : "");
	ret = 0;

	while (!quit) {
		struct pollfd pfd[5];
		long long next = 0, t;
		int n = 0, timeout = -1;

		pfd[n++] = (struct pollfd){ .fd = ui_fd, .events = POLLIN };
		pfd[n++] = (struct pollfd){ .fd = hid_fd, .events = POLLIN };
		for (i = 0; i < ev_count; i++)
			pfd[n++] = (struct pollfd){ .fd = ev_fd[i], .events = POLLIN };
		for (i = 0; i < SLOTS; i++)
			if (slot[i].deadline && (!next || slot[i].deadline < next))
				next = slot[i].deadline;
		if (release_at && (!next || release_at < next))
			next = release_at;
		if (next) {
			t = next - now_ms();
			timeout = t < 0 ? 0 : t > INT_MAX ? INT_MAX : (int)t;
		}
		if (poll(pfd, n, timeout) < 0) {
			if (errno == EINTR)
				continue;
			perror("hori-ffb: poll");
			ret = 1;
			break;
		}
		t = now_ms();
		for (i = 0; i < SLOTS; i++)
			if (slot[i].deadline && slot[i].deadline <= t)
				slot_timer(i);
		if (release_at && release_at <= t) {
			if (any_playing())
				release_at = 0;
			else
				motor_release();
		}
		if (pfd[0].revents & POLLIN)
			handle_uinput();
		if (pfd[1].revents & (POLLIN | POLLERR | POLLHUP))
			handle_hidraw();
		for (i = 0; i < ev_count; i++)
			if (pfd[2 + i].revents & (POLLIN | POLLERR | POLLHUP))
				handle_evdev(i);
		for (i = 1; i < n; i++)
			if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				fprintf(stderr, "hori-ffb: wheel unplugged\n");
				wheel_gone = true;
				quit = 1;
			}
	}

	/* give the wheel back: nothing loaded, firmware centring */
	if (!wheel_gone && (cleaned || motor_on)) {
		devctl(DEV_FREEALL, 1, "free all slots");
		devctl(DEV_STOPALL, 1, "stop all");
		devctl(DEV_HOST, 0, "host control off (firmware centring)");
		/* collect the last acks for the log */
		if (verbose) {
			struct pollfd p = { .fd = hid_fd, .events = POLLIN };

			while (poll(&p, 1, 50) > 0 && (p.revents & POLLIN))
				handle_hidraw();
		}
	}
	logv(1, "sent %lu reports, %lu acked\n", sent, acked);
	fprintf(stderr, "hori-ffb: stopped\n");
out:
	if (ui_fd >= 0) {
		ioctl(ui_fd, UI_DEV_DESTROY);
		close(ui_fd);
	}
	for (i = 0; i < ev_count; i++) {
		ioctl(ev_fd[i], EVIOCGRAB, 0);
		close(ev_fd[i]);
	}
	close(hid_fd);
	return ret;
}
