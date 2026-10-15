class Njord < Formula
  desc "Transparent IMAP forwarding proxy for Skadi"
  homepage "https://xn--skai-dqa.goblinreactor.com/#njord"
  url "https://brew.goblinreactor.com/sources/njord-0.1.1.tar.gz"
  version "0.1.1"
  sha256 "743f00cf1ebc029613d87f6021fc195fd4a8c238d21bc65ff30d9b0cae530b1a"
  license "Apache-2.0"

  bottle do
    root_url "https://brew.goblinreactor.com/bottles"
    rebuild 1
    sha256 cellar: :any_skip_relocation, arm64_tahoe: "45031cbe4242474086c550f4c55ffb395ae0d72c70c391e60912acbf81f5272c"
  end

  depends_on "python@3.14"
  allow_network_access! :test

  def install
    bin.install "njord/njord"
    inreplace bin/"njord", "#!/usr/bin/env python3", "#!#{Formula["python@3.14"].opt_bin}/python3.14"
    man1.install "doc/man1/njord.1"
    doc.install "njord/README.md", "LICENSE", "NOTICE"
    (pkgshare/"tests").install "njord/test_njord.py"
  end

  def caveats
    <<~EOS
      Start a local relay with:
        njord --upstream imap.example.com
      Use your provider's IMAP hostname. Remote clients need listener TLS or
      an encrypted tunnel. This release forwards traffic without compression.
    EOS
  end

  test do
    assert_match "njord 0.1.1", shell_output("#{bin}/njord --version")
    cp bin/"njord", testpath/"njord"
    cp pkgshare/"tests/test_njord.py", testpath/"test_njord.py"
    system Formula["python@3.14"].opt_bin/"python3.14", "-m", "unittest", "-v", "test_njord"
  end
end
