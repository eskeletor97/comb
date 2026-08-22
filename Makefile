CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

PREFIX ?= /usr/local

comb: comb.c
	$(CC) $(CFLAGS) -o $@ $<

install: comb
	install -Dm755 comb $(DESTDIR)$(PREFIX)/bin/comb

clean:
	rm -f comb

.PHONY: clean install
