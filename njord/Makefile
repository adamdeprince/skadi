# Standalone C++17 build. Python is used only by the integration test harness.
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man
DESTDIR ?=
INSTALL ?= install
PYTHON ?= python3
PKG_CONFIG ?= pkg-config
CXXFLAGS ?= -O2 -g
NJORDBIN ?= njord
DEPENDENCIES = libevent_openssl libevent_extra openssl libzstd libcurl libwebp
EVENT_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(DEPENDENCIES))
EVENT_LIBS := $(shell $(PKG_CONFIG) --libs $(DEPENDENCIES))

.PHONY: all install uninstall check clean
all: $(NJORDBIN)

$(NJORDBIN): njord.cpp image.cpp image.h ../pith/stb_image.h ../pith/stb_image_resize2.h ../imap/src/c-client/njord_codec.h Makefile
	$(PKG_CONFIG) --exists $(DEPENDENCIES)
	$(CXX) $(CPPFLAGS) $(EVENT_CFLAGS) $(CXXFLAGS) -std=c++17 -Wall -Wextra -Wpedantic njord.cpp image.cpp $(LDFLAGS) $(EVENT_LIBS) $(LDLIBS) -o "$@"

install: all
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(MANDIR)/man1"
	$(INSTALL) -m 755 "$(NJORDBIN)" "$(DESTDIR)$(BINDIR)/njord"
	$(INSTALL) -m 644 ../doc/man1/njord.1 "$(DESTDIR)$(MANDIR)/man1/njord.1"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/njord" "$(DESTDIR)$(MANDIR)/man1/njord.1"

check: all
	NJORDBIN="$(abspath $(NJORDBIN))" $(PYTHON) -m unittest discover -s . -p 'test_njord.py' -v

clean:
	rm -f "$(NJORDBIN)"
