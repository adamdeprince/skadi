class Njord < Formula
  desc "Native C++ IMAP forwarding proxy for Skadi"
  homepage "https://xn--skai-dqa.goblinreactor.com/#njord"
  url "https://brew.goblinreactor.com/sources/skadi-0.1.0-20261015.2.tar.gz"
  version "0.1.0"
  revision 1
  version_scheme 1 # C++ rewrite replaces the Python 0.1.1 release.
  sha256 "e9f385255b6c69132e59b6e78d7dad77d3118bfa26908802a8a2bf0c240bb906"
  license "Apache-2.0"

  bottle do
    root_url "https://brew.goblinreactor.com/bottles"
    sha256 cellar: :any, arm64_tahoe: "71f93c56c786a80f53f25b4df1d54a450fe96d857c061190bfabf9ce8caf8aab"
  end

  depends_on "pkgconf" => :build
  depends_on "zstd"
  depends_on "webp"
  depends_on "libevent"
  depends_on "openssl@4"
  depends_on "python@3.14" => :test
  allow_network_access! :test

  def install
    ENV.prepend_path "PKG_CONFIG_PATH", Formula["openssl@4"].opt_lib/"pkgconfig"
    system "make", "-C", "njord", "CXX=#{ENV.cxx}", "CXXFLAGS=#{ENV.cxxflags}"
    bin.install "njord/njord"
    man1.install "doc/man1/njord.1"
    doc.install "njord/README.md", "LICENSE", "NOTICE"
    (pkgshare/"tests").install "njord/test_njord.py"
  end

  def caveats
    <<~EOS
      Start a local relay with:
        njord --upstream imap.example.com
      Use your provider's IMAP hostname. Remote clients need listener TLS or
      an encrypted tunnel. Skadi connects with /njord and uses zstd level 22 compression.
      Image previews use WebP; original bytes are available on request.
    EOS
  end

  test do
    assert_match "njord 0.1.0", shell_output("#{bin}/njord --version")
    ENV["NJORDBIN"] = bin/"njord"
    cp pkgshare/"tests/test_njord.py", testpath/"test_njord.py"
    system Formula["python@3.14"].opt_bin/"python3.14", "-m", "unittest", "-v", "test_njord"
  end
end
