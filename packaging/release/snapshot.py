#!/usr/bin/env python3
"""Copy the working source into an isolated, buildable release snapshot."""
import pathlib
import shutil
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
destination = pathlib.Path(sys.argv[1]).resolve()
if destination.exists():
    raise SystemExit('Destination already exists: ' + str(destination))
files = set(subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'], cwd=root).decode().split('\0'))
# Autotools auxiliary files are ignored in the checkout but needed by configure.
files.update(['install-sh', 'missing', 'depcomp', 'mkinstalldirs', 'ltmain.sh'])
for name in sorted(files):
    if not name or name.startswith(('autom4te.cache/', '.codex/', '.agents/')):
        continue
    source = root / name
    if source.is_symlink():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.symlink_to(source.readlink())
    elif source.is_file():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
# Use the checked-in generated build system, without requiring the exact
# historical Automake version on every build host.
for pattern in ('aclocal.m4', 'configure', 'include/config.h.in', '**/Makefile.in'):
    for target in destination.glob(pattern):
        target.touch()
print(destination)
