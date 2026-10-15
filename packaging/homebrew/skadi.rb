class Skadi < Formula
  desc "Alpine mail fork with kitty and Silkie terminal support"
  homepage "https://xn--skai-dqa.goblinreactor.com/"
  url "https://brew.goblinreactor.com/sources/skadi-0.1.0-20261015.2.tar.gz"
  sha256 "e9f385255b6c69132e59b6e78d7dad77d3118bfa26908802a8a2bf0c240bb906"
  version "0.1.0"
  revision 1
  license "Apache-2.0"

  bottle do
    root_url "https://brew.goblinreactor.com/bottles"
    sha256 arm64_tahoe: "7bae5c731598834d4b3c04a4e128bba9b2f004208f4c422eae0bc9e4873d9710"
  end

  depends_on "pkgconf" => :build
  depends_on "zstd"
  depends_on "webp"
  depends_on "ncurses"
  depends_on "openssl@3"

  def install
    ENV.append "CPPFLAGS", "-I#{Formula["ncurses"].opt_include}"
    ENV.append "LDFLAGS", "-L#{Formula["ncurses"].opt_lib}"
    system "./configure", *std_configure_args,
           "--without-ldap", "--without-krb5", "--without-tcl", "--disable-nls",
           "--with-ssl-dir=#{Formula["openssl@3"].opt_prefix}",
           "--with-ssl-certs-dir=#{etc}/openssl@3/certs"
    # c-client's historical generated-file setup is not parallel-safe.
    # Build the client targets; Njord is an optional separate formula.
    %w[pith pico alpine].each { |directory| system "make", "-C", directory }, "-j1", "c-client.d", "c-client"
    # Build the client targets; Njord is an optional separate formula.
    %w[pith pico alpine].each { |directory| system "make", "-C", directory }
    bin.install "alpine/alpine" => "skadi"
    man1.install "doc/man1/alpine.1" => "skadi.1"
    doc.install "LICENSE", "NOTICE", "README"
  end

  def caveats
    <<~EOS
      Run skadi. This fork retains Alpine's configuration paths and upstream
      version banner (Alpine 2.29.99). Njord is a separate formula:
        brew install goblinreactor/tap/njord
    EOS
  end

  test do
    assert_match "Alpine 2.29.99", shell_output("#{bin}/skadi -v 2>&1")
    assert_path_exists man1/"skadi.1"
  end
end
