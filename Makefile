CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
THREADS ?= -pthread

PREFIX ?= /usr/local

comb: comb.c config.h
	$(CC) $(CFLAGS) $(THREADS) -o $@ $<

tests/selftest: tests/selftest.c comb.c config.h
	$(CC) $(CFLAGS) $(THREADS) -o $@ tests/selftest.c

tests/loadbench: tests/loadbench.c comb.c config.h
	$(CC) $(CFLAGS) $(THREADS) -o $@ tests/loadbench.c

check: comb tests/selftest
	./tests/selftest
	python3 tests/harness.py --check ./comb

install: comb comb.1
	install -Dm755 comb $(DESTDIR)$(PREFIX)/bin/comb
	install -Dm644 comb.1 $(DESTDIR)$(PREFIX)/share/man/man1/comb.1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/comb
	rm -f $(DESTDIR)$(PREFIX)/share/man/man1/comb.1

clean:
	rm -f comb tests/selftest tests/loadbench

.PHONY: check clean install uninstall
