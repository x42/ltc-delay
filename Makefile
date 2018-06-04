PREFIX ?= /usr/local
bindir = $(PREFIX)/bin
mandir = $(PREFIX)/share/man/man1
CFLAGS ?= -Wall -g -O2

VERSION=0.1.0

ifeq ($(shell pkg-config --atleast-version=1.1.0 ltc || echo no), no)
  $(error "https://github.com/x42/libltc version >= 1.1.0 is required - install libltc-dev")
endif

ifeq ($(shell pkg-config --exists jack || echo no), no)
  $(error "http://jackaudio.org is required - install libjack-dev or libjack-jackd2-dev")
endif

override CFLAGS += `pkg-config --cflags ltc jack` -DVERSION=\"$(VERSION)\"
LOADLIBES = `pkg-config --libs ltc jack`

LOADLIBES+=-lm -lpthread

ifneq ($(shell uname),Darwin)
  LOADLIBES+=-lrt
endif

all: ltc-delay

man: ltc-delay.1

ltc-delay: ltc-delay.c

ltc-delay.1: ltc-delay
	help2man -N -n 'JACK audio client to measure delay using LTC' -o ltc-delay.1 ./ltc-delay

clean:
	rm -f ltc-delay

install: install-bin install-man

uninstall: uninstall-bin uninstall-man

install-bin: ltc-delay
	install -d $(DESTDIR)$(bindir)
	install -m755 ltc-delay $(DESTDIR)$(bindir)

uninstall-bin:
	rm -f $(DESTDIR)$(bindir)/ltc-delay
	-rmdir $(DESTDIR)$(bindir)

install-man:
	install -d $(DESTDIR)$(mandir)
	install -m644 ltc-delay.1 $(DESTDIR)$(mandir)

uninstall-man:
	rm -f $(DESTDIR)$(mandir)/ltc-delay.1
	-rmdir $(DESTDIR)$(mandir)

.PHONY: all clean install uninstall man install-man install-bin uninstall-man uninstall-bin
