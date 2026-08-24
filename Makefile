CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

PREFIX ?= /usr/local

comb: comb.c config.h
	$(CC) $(CFLAGS) -o $@ $<

tests/selftest: tests/selftest.c comb.c config.h
	$(CC) $(CFLAGS) -o $@ tests/selftest.c

check: comb tests/selftest
	./tests/selftest
	python3 tests/harness.py --check ./comb

install: comb
	install -Dm755 comb $(DESTDIR)$(PREFIX)/bin/comb

clean:
	rm -f comb tests/selftest

.PHONY: check clean install
