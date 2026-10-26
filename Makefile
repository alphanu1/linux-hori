# SPDX-License-Identifier: GPL-2.0
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -std=gnu11
LDLIBS  += -lm
VERSION := $(shell cat VERSION 2>/dev/null || echo dev)
CFLAGS  += -DHORI_FFB_VERSION='"$(VERSION)"'

# MiSTer (armhf). Put the toolchain's bin directory on PATH, or pass
# CROSS_COMPILE=/path/to/bin/arm-none-linux-gnueabihf-
CROSS_COMPILE ?= arm-none-linux-gnueabihf-
ARM_CFLAGS    ?= -O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard

all: hori-ffb

hori-ffb: hori-ffb.c VERSION
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# static, so it doesn't depend on the board's libc
arm: hori-ffb-arm
hori-ffb-arm: hori-ffb.c VERSION
	$(CROSS_COMPILE)gcc $(ARM_CFLAGS) -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
		-DHORI_FFB_VERSION='"$(VERSION)"' -static -s -o $@ $< -lm

# test tool: uploads and plays weak effects on the virtual wheel
tools/ffcheck: tools/ffcheck.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f hori-ffb hori-ffb-arm tools/ffcheck

.PHONY: all arm clean
