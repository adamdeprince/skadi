#!/bin/sh
# Run from a disposable source snapshot; output remains alongside that snapshot.
set -eu
export LC_ALL=C
./configure --prefix=/usr --without-ldap --without-krb5 --without-tcl --disable-nls CC=/usr/bin/gcc CFLAGS='-O2 -g -std=gnu11' LIBS=-lm > ../configure.log 2>&1
make -j1 c-client > ../build.log 2>&1
make -j "$(getconf _NPROCESSORS_ONLN)" >> ../build.log 2>&1
./alpine/alpine -v > ../version.txt 2>&1
regression_dir=$(mktemp -d /tmp/skadi-regression.XXXXXX)
trap 'rm -rf "$regression_dir"' EXIT HUP INT TERM
make -C alpine -f Makefile -f ../tests/Makefile.regression TEST_DIR="$regression_dir" check-local-regressions > ../regressions.log 2>&1
