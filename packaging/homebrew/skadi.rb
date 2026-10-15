class Skadi < Formula
  desc "Alpine mail fork with kitty and Silkie terminal support"
  homepage "https://xn--skai-dqa.goblinreactor.com/"
  url "https://brew.goblinreactor.com/sources/skadi-0.1.0.tar.gz"
  sha256 "6bf0dc0d757e48133af9b39b2340711f77c4379706e7b6c8a3e9764fe7d26aec"
  license "Apache-2.0"

  bottle do
    root_url "https://brew.goblinreactor.com/bottles"
    rebuild 1
    sha256 arm64_tahoe: "e7d51e32aea69acac1639a381b4ec99bd6d01d0c6956998b26140fd76a83b675"
  end

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
    system "make", "-j1", "c-client"
    system "make"
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
