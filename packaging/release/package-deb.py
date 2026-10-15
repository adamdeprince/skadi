#!/usr/bin/env python3
"""Package native Skadi and Njord builds without root."""
import argparse
import pathlib
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--suite', choices=['resolute', 'kylin'], required=True)
p.add_argument('--output', type=pathlib.Path, required=True)
p.add_argument('--package', choices=['skadi', 'njord'], action='append',
               help='Package only this program (repeatable; default: both)')
a = p.parse_args()
root = pathlib.Path.cwd()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
arch = subprocess.check_output(['dpkg', '--print-architecture'], text=True).strip()


def install(src, dest, executable=False):
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(str(src), str(dest))
    dest.chmod(0o755 if executable else 0o644)

for name in dict.fromkeys(a.package or ('skadi', 'njord')):
    version = '0.1.0-2+' + a.suite + '1'
    # The C++ rewrite resets Njord's upstream version from Python's 0.1.1.
    # An epoch makes this an ordinary upgrade for existing installations.
    deb_version = ('1:' if name == 'njord' else '') + version
    stage = out / (name + '-root')
    if stage.exists():
        raise SystemExit('Refusing to reuse existing package root: ' + str(stage))
    stage.mkdir()
    if name == 'skadi':
        install(root / 'alpine/alpine', stage / 'usr/bin/skadi', True)
        install(root / 'doc/man1/alpine.1', stage / 'usr/share/man/man1/skadi.1')
        description = 'Alpine fork with kitty and Silkie terminal support\n Inline images, rich composition, terminal file transfers and offline mail.\n The executable is skadi; existing Alpine configuration paths are retained.'
    else:
        install(root / 'njord/njord', stage / 'usr/bin/njord', True)
        install(root / 'doc/man1/njord.1', stage / 'usr/share/man/man1/njord.1')
        install(root / 'njord/README.md', stage / 'usr/share/doc/njord/README.md')
        description = 'Native C++ compressed IMAP proxy for Skadi\n Optional zstd level 22 transport, verified provider TLS and WebP previews.\n Uses conventional IMAP with the mail provider.'
    binary = stage / ('usr/bin/' + name)
    subprocess.run(['strip', str(binary)], check=True)
    # Derive dependencies from the actual linked libraries and symbol metadata.
    control = out / 'debian/control'
    control.parent.mkdir(exist_ok=True)
    control.write_text('Source: ' + name + '\nSection: mail\nPriority: optional\nMaintainer: Adam DePrince <adam.deprince@gmail.com>\n\nPackage: ' + name + '\nArchitecture: any\nDescription: ' + description + '\n')
    deps = subprocess.check_output(['dpkg-shlibdeps', '-O', '-e' + str(binary)], cwd=str(out), universal_newlines=True).strip().split('=', 1)[1]
    deps += ', ca-certificates'
    install(root / 'LICENSE', stage / ('usr/share/doc/' + name + '/copyright'))
    install(root / 'NOTICE', stage / ('usr/share/doc/' + name + '/NOTICE'))
    control = stage / 'DEBIAN/control'
    control.parent.mkdir()
    control.write_text('Package: ' + name + '\nVersion: ' + deb_version + '\nArchitecture: ' + arch + '\nSection: mail\nPriority: optional\nMaintainer: Adam DePrince <adam.deprince@gmail.com>\nHomepage: https://xn--skai-dqa.goblinreactor.com/\nDepends: ' + deps + '\nDescription: ' + description + '\n')
    filename = out / (name + '_' + version + '_' + arch + '.deb')
    subprocess.run(['dpkg-deb', '--root-owner-group', '-Zxz', '--build', str(stage), str(filename)], check=True)
    print(filename)
