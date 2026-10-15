# Skadi and Njord releases

Skadi is packaged as `skadi`, keeping Alpine's configuration paths and upstream
version banner. Njord is a separate package and does not enable a service.

## Native targets

| Build host | Package target | Repository |
| --- | --- | --- |
| localhost | macOS 26 Tahoe, arm64 | Homebrew `goblinreactor/tap` |
| naamah | Ubuntu 26.04, amd64 | APT `resolute` |
| graviton5 | Ubuntu 26.04, arm64 | APT `resolute` |
| loongson | Kylin V10 SP1, old-world loongarch64 | APT `kylin` |

The configured SSH alias is `loongson`. Njord uses Python 3.8 or later and its
Debian packages have architecture `all`; its integration suite runs on each host.

## Build

Create a clean working-source snapshot with `snapshot.py DESTINATION`. This
includes current tracked changes and untracked source files, preserves symlinks,
and supplies the ignored Autotools auxiliary files. Review the snapshot before
publishing. Keep the original checkout and its existing changes intact.

Run `build-linux.sh` from the isolated snapshot, then
`python3 packaging/release/package-deb.py --suite resolute --output ../packages`
(or `--suite kylin`). The script builds c-client serially before parallel
compilation, links libm, and runs the existing mail regressions. Run
`make -C njord check` separately. `dpkg-shlibdeps` derives runtime dependencies.

The initial Skadi 0.1.0 source archive predates the explicit libm flag in the
helper; use `LIBS=-lm sh packaging/release/build-linux.sh` with that archive.
The separately versioned Njord 0.1.1 archive includes its Python 3.8 fixes.

Homebrew formulas are in `../homebrew`. Build bottles natively, test their
installed commands, publish the bottle and source hashes, and verify a public
bottle installation. Record actual linked and installed dependency versions.
The initial Mac release used Apple clang 21 from CLT 26.5; Homebrew's build/test
wrapper rejected the separately installed Xcode 16.3. Its payload was built
with the formula's configure options, staged as a keg, and bottled by Homebrew.
The formula's installed checks were run directly; do not report that the
blocked `brew test` wrapper passed. The Skadi bottle links against Homebrew's
OpenSSL 3.5.9 and ncurses 6.6.

## Publish and verify

- Keep artifact filenames immutable. Compare SHA-256 before reusing a name.
- Sources and bottles go under `hail::distribution/brew.goblinreactor.com/`;
  matching sources also go under `hail::distribution/apt.goblinreactor.com/`.
- On hail, use the existing `~/.local/share/goblin-apt` publisher locks and
  signing configuration. Back up repository metadata before publication.
- Import packages with `reprepro --export=never includedeb`, then check and
  export the affected suites. Verify each InRelease with the public archive key.
- Change only the Skadi/Njord formulas in a private clone of the existing tap.
  Fast-forward the public bare repository and run `git update-server-info`.
- Install from the public repositories on all four native targets. Run the
  installed programs and relay tests; preserve evidence in the release stage.
- Merge new records into each site's `packages.json`, retaining other packages,
  and sign the catalog with the existing archive key. Update both repository
  landing pages and their plain-text instructions.

The initial release source is a working-tree snapshot based on commit
`a18ac755bf082b3ade2fcc01f1583a2b50dde134`, including existing uncommitted mail
features. Its immutable archive checksum identifies the exact packaged source;
the commit alone does not.

## Website

The website is buildless HTML/CSS in `html/`. Check desktop and mobile layouts,
then rsync that directory to `hail::distribution/skadi.goblinreactor.com/`.
`../nginx/skadi.goblinreactor.com.conf` supplies the two virtual hosts. Test
nginx configuration before reloading. The canonical IDNA hostname is
`xn--skai-dqa.goblinreactor.com`; the ASCII spelling redirects with status 308.
