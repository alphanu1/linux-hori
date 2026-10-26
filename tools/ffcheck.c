// SPDX-License-Identifier: GPL-2.0
/*
 * ffcheck: play weak effects on the hori-ffb virtual wheel through the
 * standard Linux force feedback interface, and report how the steering
 * axis moved while each played. Nothing above 20 %. An unheld wheel will
 * still turn to its stop under a 15 % constant force.
 *
 *   sudo tools/ffcheck [test...]       (default: all)
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

static int find_virtual(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *de;
	char path[PATH_MAX], name[256];
	int f = -1;

	while (d && (de = readdir(d))) {
		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(path, sizeof(path), "/dev/input/%s", de->d_name);
		f = open(path, O_RDWR | O_NONBLOCK);
		if (f < 0)
			continue;
		if (ioctl(f, EVIOCGNAME(sizeof(name)), name) > 0 && !strcmp(name, NAME)) {
			printf("%s\n", path);
			break;
		}
		close(f);
		f = -1;
	}
	if (d)
		closedir(d);
	return f;
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
	printf("   %-30s ABS_X start %5d min %5d max %5d end %5d\n",
	       label, ai.value, lo, hi, last);
}

static void play(int id, int value)
{
	struct input_event ev = { .type = EV_FF, .code = id, .value = value };

	if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
		perror("play");
}

static void set(int code, int value)
{
	struct input_event ev = { .type = EV_FF, .code = code, .value = value };

	if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
		perror("set");
}

static int upload(struct ff_effect *e)
{
	e->id = -1;
	if (ioctl(fd, EVIOCSFF, e) < 0) {
		fprintf(stderr, "   upload: %s\n", strerror(errno));
		return -1;
	}
	return e->id;
}

/* play n effects together for `dur` ms, then stop, erase and watch centring */
static void run(const char *label, struct ff_effect *e, int n, int dur, int count)
{
	int i;

	printf("%s\n", label);
	for (i = 0; i < n; i++)
		if (upload(&e[i]) < 0)
			return;
	for (i = 0; i < n; i++)
		play(e[i].id, count);
	watch(dur, "playing");
	for (i = 0; i < n; i++) {
		play(e[i].id, 0);
		ioctl(fd, EVIOCRMFF, e[i].id);
	}
	watch(1500, "after stop (firmware centring)");
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
	unsigned long ff[4] = { 0 };
	int n = 0;
	struct ff_effect e[2];

	fd = find_virtual();
	if (fd < 0) {
		fprintf(stderr, "no hori-ffb virtual wheel found\n");
		return 1;
	}
	ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff)), ff);
	ioctl(fd, EVIOCGEFFECTS, &n);
	printf("ff bits (0x50..0x61): %lx, %d effects\n",
	       (ff[0x50 / (8 * sizeof(long))] >> (0x50 % (8 * sizeof(long)))) & 0x3ffff, n);

	if (want(argc, argv, "spring")) {
		e[0] = condition(FF_SPRING, 20);
		e[0].replay.length = 1000;
		run("spring 20% (wheel at centre: no movement expected)", e, 1, 1200, 1);
	}
	if (want(argc, argv, "left")) {
		e[0] = constant(0x4000, 15, 1000);
		run("constant 15%, direction 0x4000", e, 1, 1200, 1);
	}
	if (want(argc, argv, "right")) {
		e[0] = constant(0xc000, 15, 1000);
		run("constant 15%, direction 0xc000", e, 1, 1200, 1);
	}
	if (want(argc, argv, "sine")) {
		e[0] = (struct ff_effect){ .type = FF_PERIODIC, .direction = 0x4000 };
		e[0].u.periodic.waveform = FF_SINE;
		e[0].u.periodic.period = 250;
		e[0].u.periodic.magnitude = PCT(15);
		e[0].replay.length = 1500;
		run("sine 15%, period 250 ms", e, 1, 1700, 1);
	}
	if (want(argc, argv, "rumble")) {
		/* as MiSTer's Main sends it; ff-core turns it into a 50 ms sine */
		e[0] = (struct ff_effect){ .type = FF_RUMBLE, .direction = 0x4000 };
		e[0].u.rumble.strong_magnitude = 0xffff * 20 / 100;
		e[0].u.rumble.weak_magnitude = 0xffff * 20 / 100;
		e[0].replay.length = 1000;
		run("rumble strong 20% weak 20%", e, 1, 1200, 1);
	}
	if (want(argc, argv, "damper")) {
		e[0] = constant(0x4000, 15, 1000);
		e[1] = condition(FF_DAMPER, 20);
		run("constant 15% + damper 20%", e, 2, 1200, 1);
	}
	if (want(argc, argv, "gain")) {
		set(FF_GAIN, 0x4000);	/* 25 % */
		e[0] = constant(0xc000, 15, 1000);
		run("constant 15% at gain 25%", e, 1, 1200, 1);
		set(FF_GAIN, 0xffff);
	}
	if (want(argc, argv, "autocentre")) {
		set(FF_AUTOCENTER, 0xffff * 20 / 100);
		e[0] = constant(0x4000, 15, 1000);
		run("constant 15% + autocentre 20%", e, 1, 1200, 1);
		set(FF_AUTOCENTER, 0);
	}
	if (want(argc, argv, "replay")) {
		/* uploaded once, played twice with a hand-back to the firmware between */
		printf("constant 15%%, 500 ms, played, released, played again\n");
		e[0] = constant(0x4000, 15, 500);
		if (upload(&e[0]) >= 0) {
			play(e[0].id, 1);
			watch(700, "first play");
			watch(1500, "released (firmware centring)");
			play(e[0].id, 1);
			watch(700, "second play, same upload");
			ioctl(fd, EVIOCRMFF, e[0].id);
			watch(1500, "after erase (firmware centring)");
		}
	}
	if (want(argc, argv, "update")) {
		/* how games drive a constant force: one effect, re-uploaded while it plays */
		printf("constant 15%% left, updated to right while playing\n");
		e[0] = constant(0x4000, 15, 0);
		if (upload(&e[0]) >= 0) {
			play(e[0].id, 1);
			watch(600, "left");
			e[0].direction = 0xc000;
			if (ioctl(fd, EVIOCSFF, &e[0]) < 0)
				perror("   update");
			watch(900, "updated to right");
			play(e[0].id, 0);
			ioctl(fd, EVIOCRMFF, e[0].id);
			watch(1500, "after stop (firmware centring)");
		}
	}
	if (want(argc, argv, "repeat")) {
		e[0] = constant(0xc000, 15, 300);
		e[0].replay.delay = 300;
		run("constant 15%, 300 ms, delay 300 ms, played x3", e, 1, 1500, 3);
	}
	close(fd);
	return 0;
}
