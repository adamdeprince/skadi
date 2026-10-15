# Standalone installation; configuring or compiling Alpine is not required.
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
DESTDIR ?=
INSTALL ?= install
PYTHON ?= python3

.PHONY: all install uninstall check
all:

install:
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(MANDIR)/man1"
	$(INSTALL) -m 755 njord "$(DESTDIR)$(BINDIR)/njord"
	$(INSTALL) -m 644 ../doc/man1/njord.1 "$(DESTDIR)$(MANDIR)/man1/njord.1"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/njord" "$(DESTDIR)$(MANDIR)/man1/njord.1"

check:
	$(PYTHON) -m unittest discover -s . -p 'test_njord.py' -v
