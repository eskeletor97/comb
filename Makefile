CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
# config.h's static palettes/keymap are unused in some modules; that is fine.
CFLAGS += -Wno-unused-const-variable
THREADS ?= -pthread

PREFIX ?= /usr/local

MODS := base state load match jobs render clip input

comb: $(addsuffix .o,$(MODS)) main.o config.h
	$(CC) $(CFLAGS) $(THREADS) -o $@ $(addsuffix .o,$(MODS)) main.o

%.o: %.c comb.h config.h
	$(CC) $(CFLAGS) $(THREADS) -c $< -o $@

TEST_SRCS := base.c state.c load.c match.c jobs.c render.c clip.c input.c

tests/selftest: tests/selftest.c $(TEST_SRCS) comb.h config.h
	$(CC) $(CFLAGS) $(THREADS) -o $@ tests/selftest.c

tests/loadbench: tests/loadbench.c $(TEST_SRCS) comb.h config.h
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
	rm -f comb *.o tests/selftest tests/loadbench

.PHONY: check clean install uninstall
