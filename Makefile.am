## Process this file with automake to produce Makefile.in
## Use aclocal -I m4; automake

# ========================================================================
# Copyright 2006-2008 University of Washington
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# ========================================================================

EXTRA_DIST = config.rpath VERSION README LICENSE njord/njord.cpp njord/image.cpp njord/image.h njord/Makefile njord/README.md njord/test_njord.py njord/test_skadi.c njord/Makefile.integration imap/src/c-client/njord_codec.h imap/src/c-client/njord_transport.c doc/man1/njord.1

SUBDIRS = m4 po $(REGEX_BUILD) pith pico alpine $(WEB_PUBCOOKIE_BUILD) $(WEB_BUILD)

BUILT_SOURCES = c-client.d c-client $(WEB_PUBCOOKIE_LINK)

CLEANFILES = c-client

.PHONY: c-client.d c-client gssapi_proxy.l

c-client.d:
	if test ! -L c-client ; then $(LN_S) imap/c-client c-client ; fi

c-client:
	$(C_CLIENT_WITH_IPV6)
	cd imap && $(MAKE) $(C_CLIENT_TARGET) $(C_CLIENT_CFLAGS) $(C_CLIENT_LDFLAGS) $(C_CLIENT_GCCOPTLEVEL) EXTRAAUTHENTICATORS="$(C_CLIENT_AUTHS)" EXTRASPECIALS="$(C_CLIENT_SPECIALS)" BUNDLED="$(C_CLIENT_BUNDLED)"

gssapi_proxy.l:
	if test ! -L c-client/auth_gss_proxy.c ; then $(LN_S) @abs_top_srcdir@/web/src/pubcookie/auth_gss_proxy.c c-client/auth_gss_proxy.c ; fi

all-local:
	$(MAKE) -C $(srcdir)/njord

clean-local:
	cd imap && $(MAKE) clean
	$(MAKE) -C $(srcdir)/njord clean

man_MANS = doc/man1/alpine.1 doc/man1/pico.1 doc/man1/pilot.1 doc/man1/rpdump.1 doc/man1/rpload.1

ACLOCAL_AMFLAGS = --install -I m4

install-exec-local:
	cd imap && $(MAKE) install-mailutil BUNDLED="$(C_CLIENT_BUNDLED)" DESTDIR="$(DESTDIR)" bindir="$(bindir)" mandir="$(mandir)"
	$(MAKE) -C $(srcdir)/njord install PREFIX="$(prefix)" BINDIR="$(bindir)" MANDIR="$(mandir)" DESTDIR="$(DESTDIR)"

uninstall-local:
	cd imap && $(MAKE) uninstall-mailutil BUNDLED="$(C_CLIENT_BUNDLED)" DESTDIR="$(DESTDIR)" bindir="$(bindir)" mandir="$(mandir)"
	$(MAKE) -C $(srcdir)/njord uninstall PREFIX="$(prefix)" BINDIR="$(bindir)" MANDIR="$(mandir)" DESTDIR="$(DESTDIR)"
