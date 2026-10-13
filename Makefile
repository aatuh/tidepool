CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?= -lm

.PHONY: all tidepool run test clean
all: builds/tidepool

tidepool: builds/tidepool

builds/tidepool: tidepool.c | builds
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

builds/unit: tests/unit.c tidepool.c | builds
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

builds:
	mkdir -p $@

run: builds/tidepool
	./builds/tidepool

test: builds/tidepool builds/unit
	./builds/unit
	python3 tests/smoke.py ./builds/tidepool

clean:
	rm -rf builds
