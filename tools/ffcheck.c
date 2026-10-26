// SPDX-License-Identifier: GPL-2.0
/*
 * ffcheck: a force feedback test for any wheel Linux offers force feedback
 * for -- the hori-ffb virtual wheel, or a Logitech, Thrustmaster, Fanatec...
 * Plays weak effects (20 % at most) through the standard Linux interface and
 * reports each test as PASS (the wheel's driver accepted and played it),
 * FAIL (it refused) or SKIP (the wheel doesn't offer that effect), with how
 * the steering axis moved where that can be seen. An unheld wheel will still
 * turn to its stop under a 15 % constant force.
 *
 *   sudo tools/ffcheck [-v] [--device /dev/input/eventN] [test...]   (default: all)
 *   -v also prints how the steering axis moved in each phase
 *
 * Without --device it uses the hori-ffb virtual wheel if it's there, or else
 * the first input device with force feedback and a steering axis.
 *
 * tests: spring left right sine rumble damper gain autocentre replay update
 *        repeat
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#define NAME "HORI FFB Racing Wheel (hori-ffb)"
#define PCT(p) (0x7fff * (p) / 100)

static int fd;
static int verbose;
static int has_bit(const unsigned long *b, int n);
static unsigned long ffbits[(FF_CNT + 8 * sizeof(long) - 1) / (8 * sizeof(long))];
static int w_start, w_end, w_range = 65536;
static int n_pass, n_fail, n_skip;
static int t_fail;		/* this test failed */

static void result(const char *what, int dir)
{
	const char *moved = "";

	if (dir == 2)
		moved = "feel it";
	else if (dir == 3)
		moved = "holds centre";
	else if (dir && w_range > 0) {
		int d = w_end - w_start, th = w_range / 16;

		if (dir < 0 ? d < -th : d > th)
			moved = dir < 0 ? "moved left" : "moved right";
		else if (dir < 0 ? d > th : d < -th)
			moved = dir < 0 ? "WENT RIGHT: mirrored?" : "WENT LEFT: mirrored?";
		else
			moved = "check by feel";
	}
	if (t_fail) {
		n_fail++;
		printf("FAIL  %s\n", what);
	} else {
		n_pass++;
		printf("PASS  %-22s %s\n", what, moved);
	}
	fflush(stdout);
}

static int need(int code, const char *label)
{
	if (code < 0 || has_bit(ffbits, code))
		return 1;
	n_skip++;
	printf("SKIP  %-22s not supported\n", label);
	fflush(stdout);
	return 0;
}

static int has_bit(const unsigned long *b, int n)
{
	return (b[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1;
}

/* the hori-ffb virtual wheel if present, else the first device with EV_FF and ABS_X */
static int find_wheel(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *de;
	char path[PATH_MAX], name[256];
	int f, any = -1;

	while (d && (de = readdir(d))) {
		unsigned long evb[1] = { 0 }, absb[1] = { 0 };

		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		f = open(path, O_RDWR | O_NONBLOCK);
		if (f < 0)
			continue;
		name[0] = 0;
		ioctl(f, EVIOCGNAME(sizeof(name)), name);
		if (!strcmp(name, NAME)) {
			if (any >= 0)
				close(any);
			closedir(d);
			return f;
		}
		ioctl(f, EVIOCGBIT(0, sizeof(evb)), evb);
		ioctl(f, EVIOCGBIT(EV_ABS, sizeof(absb)), absb);
		if (any < 0 && has_bit(evb, EV_FF) && has_bit(absb, ABS_X)) {
			any = f;
			continue;
		}
		close(f);
	}
	if (d)
		closedir(d);
	return any;
}

static long long ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

/* run for `dur` ms, tracking ABS_X; prints start/min/max/end */
static void watch(int dur, const char *label)
{
	struct input_absinfo ai = { 0 };
	int lo, hi, last;
	long long end = ms() + dur;

	ioctl(fd, EVIOCGABS(ABS_X), &ai);
	lo = hi = last = ai.value;
	while (ms() < end) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		struct input_event ev[64];
		ssize_t n;
		int i;

		if (poll(&p, 1, 20) <= 0)
			continue;
		n = read(fd, ev, sizeof(ev));
		for (i = 0; i < n / (ssize_t)sizeof(ev[0]); i++)
			if (ev[i].type == EV_ABS && ev[i].code == ABS_X) {
				last = ev[i].value;
				if (last < lo) lo = last;
				if (last > hi) hi = last;
			}
	}
	{
		struct input_absinfo now = { 0 };

		/* events may not reach us (another program can hold the device): the
		 * axis's current value is always readable */
		if (ioctl(fd, EVIOCGABS(ABS_X), &now) == 0 && now.value != last) {
			last = now.value;
			if (last < lo) lo = last;
			if (last > hi) hi = last;
		}
	}
	if (verbose)
		printf("      %-26s X %5d..%5d end %5d\n", label, lo, hi, last);
	w_start = ai.value;
	w_end = last;
}

static void play(int id, int value)
{
	struct input_event ev = { .type = EV_FF, .code = id, .value = value };

	if (write(fd, &ev, sizeof(ev)) != sizeof(ev)) {
		perror("      play");
		t_fail = 1;
	}
}

static void set(int code, int value)
{
	struct input_event ev = { .type = EV_FF, .code = code, .value = value };

	if (write(fd, &ev, sizeof(ev)) != sizeof(ev)) {
		perror("      set");
		t_fail = 1;
	}
}

static int upload(struct ff_effect *e)
{
	e->id = -1;
	if (ioctl(fd, EVIOCSFF, e) < 0) {
		printf("      upload: %s\n", strerror(errno));
		t_fail = 1;
		return -1;
	}
	return e->id;
}

/* play n effects together for `dur` ms, then stop, erase and watch centring */
static void run(const char *label, struct ff_effect *e, int n, int dur, int count, int dir)
{
	int i;

	if (verbose)
		printf("      %s\n", label);
	t_fail = 0;
	for (i = 0; i < n; i++)
		if (upload(&e[i]) < 0) {
			result(label, 0);
			return;
		}
	for (i = 0; i < n; i++)
		play(e[i].id, count);
	watch(dur, "playing");
	for (i = 0; i < n; i++) {
		play(e[i].id, 0);
		ioctl(fd, EVIOCRMFF, e[i].id);
	}
	{
		int s0 = w_start, e0 = w_end;

		watch(1500, "after stop");
		w_start = s0; w_end = e0;
	}
	result(label, dir);
}

static struct ff_effect constant(int dir, int pct, int len)
{
	struct ff_effect e = { .type = FF_CONSTANT, .direction = dir };

	e.u.constant.level = PCT(pct);
	e.replay.length = len;
	return e;
}

static struct ff_effect condition(int type, int pct)
{
	struct ff_effect e = { .type = type };

	e.u.condition[0].left_coeff = e.u.condition[0].right_coeff = PCT(pct);
	e.u.condition[0].left_saturation = e.u.condition[0].right_saturation =
		0xffff * pct / 100;
	return e;
}

static int want(int argc, char **argv, const char *t)
{
	int i;

	if (argc < 2)
		return 1;
	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], t))
			return 1;
	return 0;
}

int main(int argc, char **argv)
{
	int n = 0, a = 1;
	struct ff_effect e[2];
	struct input_id id = { 0 };
	struct input_absinfo ax = { 0 };
	char name[256] = "";

	if (argc > 1 && !strcmp(argv[1], "-v")) {
		verbose = 1;
		argc--;
		argv++;
	}
	if (argc > 2 && (!strcmp(argv[1], "--device") || !strcmp(argv[1], "-d"))) {
		fd = open(argv[2], O_RDWR | O_NONBLOCK);
		if (fd < 0) {
			perror(argv[2]);
			return 1;
		}
		a = 3;
	} else
		fd = find_wheel();
	argc -= a - 1;
	argv += a - 1;
	if (fd < 0) {
		printf("No force feedback wheel found.\n");
		return 1;
	}
	ioctl(fd, EVIOCGNAME(sizeof(name)), name);
	ioctl(fd, EVIOCGID, &id);
	ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffbits)), ffbits);
	ioctl(fd, EVIOCGEFFECTS, &n);
	if (ioctl(fd, EVIOCGABS(ABS_X), &ax) == 0)
		w_range = ax.maximum - ax.minimum;
	printf("Wheel: %s\nUSB %04x:%04x\n", name, id.vendor, id.product);
	if (n <= 0) {
		printf("This device offers no force feedback.\n");
		return 1;
	}
	{
		static const struct { int bit; const char *name; } fx[] = {
			{ FF_CONSTANT, "constant" }, { FF_RAMP, "ramp" }, { FF_PERIODIC, "periodic" },
			{ FF_RUMBLE, "rumble" }, { FF_SPRING, "spring" }, { FF_DAMPER, "damper" },
			{ FF_INERTIA, "inertia" }, { FF_FRICTION, "friction" }, { FF_GAIN, "gain" },
			{ FF_AUTOCENTER, "autocentre" },
		};
		int k, col;

		/* short lines: MiSTer's script screen is narrow */
		printf("%d effects at once. Supports:\n ", n);
		col = 1;
		for (k = 0; k < (int)(sizeof(fx) / sizeof(fx[0])); k++) {
			if (!has_bit(ffbits, fx[k].bit))
				continue;
			if (col + 1 + (int)strlen(fx[k].name) > 40) {
				printf("\n ");
				col = 1;
			}
			col += printf(" %s", fx[k].name);
		}
		printf("\n\n");
	}

	if (want(argc, argv, "spring") && need(FF_SPRING, "spring")) {
		e[0] = condition(FF_SPRING, 20);
		e[0].replay.length = 1000;
		run("spring", e, 1, 1200, 1, 3);
	}
	if (want(argc, argv, "left") && need(FF_CONSTANT, "push left")) {
		e[0] = constant(0x4000, 15, 1000);
		run("push left", e, 1, 1200, 1, -1);
	}
	if (want(argc, argv, "right") && need(FF_CONSTANT, "push right")) {
		e[0] = constant(0xc000, 15, 1000);
		run("push right", e, 1, 1200, 1, 1);
	}
	if (want(argc, argv, "sine") && need(FF_SINE, "vibration (sine)")) {
		e[0] = (struct ff_effect){ .type = FF_PERIODIC, .direction = 0x4000 };
		e[0].u.periodic.waveform = FF_SINE;
		e[0].u.periodic.period = 250;
		e[0].u.periodic.magnitude = PCT(15);
		e[0].replay.length = 1500;
		run("vibration (sine)", e, 1, 1700, 1, 2);
	}
	if (want(argc, argv, "rumble") && need(FF_RUMBLE, "rumble")) {
		/* as MiSTer's Main sends it */
		e[0] = (struct ff_effect){ .type = FF_RUMBLE, .direction = 0x4000 };
		e[0].u.rumble.strong_magnitude = 0xffff * 20 / 100;
		e[0].u.rumble.weak_magnitude = 0xffff * 20 / 100;
		e[0].replay.length = 1000;
		run("rumble", e, 1, 1200, 1, 2);
	}
	if (want(argc, argv, "damper") && need(FF_DAMPER, "damper") && need(FF_CONSTANT, "damper")) {
		e[0] = constant(0x4000, 15, 1000);
		e[1] = condition(FF_DAMPER, 20);
		run("damper", e, 2, 1200, 1, -1);
	}
	if (want(argc, argv, "gain") && need(FF_GAIN, "gain") && need(FF_CONSTANT, "gain")) {
		set(FF_GAIN, 0x4000);	/* 25 % */
		e[0] = constant(0xc000, 15, 1000);
		run("gain", e, 1, 1200, 1, 1);
		set(FF_GAIN, 0xffff);
	}
	if (want(argc, argv, "autocentre") && need(FF_AUTOCENTER, "autocentre") && need(FF_CONSTANT, "autocentre")) {
		set(FF_AUTOCENTER, 0xffff * 20 / 100);
		e[0] = constant(0x4000, 15, 1000);
		run("autocentre", e, 1, 1200, 1, -1);
		set(FF_AUTOCENTER, 0);
	}
	if (want(argc, argv, "replay") && need(FF_CONSTANT, "replay")) {
		/* uploaded once, played twice with a release in between */
		if (verbose)
			printf("      played, released, played again\n");
		t_fail = 0;
		e[0] = constant(0x4000, 15, 500);
		if (upload(&e[0]) >= 0) {
			play(e[0].id, 1);
			watch(700, "first play");
			watch(1500, "released");
			play(e[0].id, 1);
			watch(700, "second play, same upload");
			{ int s0 = w_start, e0 = w_end;
			  ioctl(fd, EVIOCRMFF, e[0].id);
			  watch(1500, "after erase");
			  w_start = s0; w_end = e0; }
		}
		result("replay", -1);
	}
	if (want(argc, argv, "update") && need(FF_CONSTANT, "update")) {
		/* how games drive a constant force: one effect, re-uploaded while it plays */
		if (verbose)
			printf("      left, updated to right while playing\n");
		t_fail = 0;
		e[0] = constant(0x4000, 15, 0);
		if (upload(&e[0]) >= 0) {
			play(e[0].id, 1);
			watch(600, "left");
			e[0].direction = 0xc000;
			if (ioctl(fd, EVIOCSFF, &e[0]) < 0) {
				printf("      update: %s\n", strerror(errno));
				t_fail = 1;
			}
			watch(900, "updated to right");
			{ int s0 = w_start, e0 = w_end;
			  play(e[0].id, 0);
			  ioctl(fd, EVIOCRMFF, e[0].id);
			  watch(1500, "after stop");
			  w_start = s0; w_end = e0; }
		}
		result("update while playing", 1);
	}
	if (want(argc, argv, "repeat") && need(FF_CONSTANT, "repeat")) {
		e[0] = constant(0xc000, 15, 300);
		e[0].replay.delay = 300;
		run("delay + repeat x3", e, 1, 1500, 3, 1);
	}
	printf("\nResult: %d PASS, %d FAIL, %d SKIP\n", n_pass, n_fail, n_skip);
	close(fd);
	return n_fail ? 2 : 0;
}
