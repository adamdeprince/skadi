#!/usr/bin/env python3
"""Package a native Skadi build and the portable Njord relay without root."""
import argparse
import pathlib
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--suite', choices=['resolute', 'kylin'], required=True)
p.add_argument('--output', type=pathlib.Path, required=True)
a = p.parse_args()
root = pathlib.Path.cwd()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
arch = subprocess.check_output(['dpkg', '--print-architecture'], text=True).strip()


def install(src, dest, executable=False):
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(str(src), str(dest))
    dest.chmod(0o755 if executable else 0o644)

for name in ('skadi', 'njord'):
    version = ('0.1.0' if name == 'skadi' else '0.1.1') + '-1+' + a.suite + '1'
    package_arch = arch if name == 'skadi' else 'all'
    stage = out / (name + '-root')
    if stage.exists():
        raise SystemExit('Refusing to reuse existing package root: ' + str(stage))
    stage.mkdir()
    if name == 'skadi':
        install(root / 'alpine/alpine', stage / 'usr/bin/skadi', True)
        subprocess.run(['strip', str(stage / 'usr/bin/skadi')], check=True)
        install(root / 'doc/man1/alpine.1', stage / 'usr/share/man/man1/skadi.1')
        # shlibdeps reads the actual linked libraries and their installed symbol
        # metadata; dependency versions are not guessed from the distro name.
        control = out / 'debian/control'
        control.parent.mkdir(exist_ok=True)
        control.write_text('Source: skadi\nSection: mail\nPriority: optional\nMaintainer: Adam DePrince <adam.deprince@gmail.com>\n\nPackage: skadi\nArchitecture: any\nDescription: Terminal mail with kitty and Silkie support\n')
        deps = subprocess.check_output(['dpkg-shlibdeps', '-O', '-e' + str(stage / 'usr/bin/skadi')], cwd=str(out), universal_newlines=True).strip().split('=', 1)[1]
        deps += ', ca-certificates'
        description = 'Alpine fork with kitty and Silkie terminal support\n Inline images, rich composition, terminal file transfers and offline mail.\n The executable is skadi; existing Alpine configuration paths are retained.'
    else:
        install(root / 'njord/njord', stage / 'usr/bin/njord', True)
        script = stage / 'usr/bin/njord'
        script.write_text(script.read_text().replace('#!/usr/bin/env python3', '#!/usr/bin/python3', 1))
        install(root / 'doc/man1/njord.1', stage / 'usr/share/man/man1/njord.1')
        install(root / 'njord/README.md', stage / 'usr/share/doc/njord/README.md')
        deps = 'python3 (>= 3.8), ca-certificates'
        description = 'Transparent IMAP forwarding proxy for Skadi\n Streams IMAP between clients and their provider with TLS and backpressure.\n This version does not implement compression or the planned IMAP extension.'
    install(root / 'LICENSE', stage / ('usr/share/doc/' + name + '/copyright'))
    install(root / 'NOTICE', stage / ('usr/share/doc/' + name + '/NOTICE'))
    control = stage / 'DEBIAN/control'
    control.parent.mkdir()
    control.write_text('Package: ' + name + '\nVersion: ' + version + '\nArchitecture: ' + package_arch + '\nSection: mail\nPriority: optional\nMaintainer: Adam DePrince <adam.deprince@gmail.com>\nHomepage: https://xn--skai-dqa.goblinreactor.com/\nDepends: ' + deps + '\nDescription: ' + description + '\n')
    filename = out / (name + '_' + version + '_' + package_arch + '.deb')
    subprocess.run(['dpkg-deb', '--root-owner-group', '-Zxz', '--build', str(stage), str(filename)], check=True)
    print(filename)
