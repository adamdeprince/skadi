"""Real loopback integration tests; no mail account or external network needed."""

import asyncio
import importlib.machinery
import importlib.util
import pathlib
import socket
import ssl
import subprocess
import tempfile
import threading
import unittest

ROOT = pathlib.Path(__file__).resolve().parent
loader = importlib.machinery.SourceFileLoader("njord", str(ROOT / "njord"))
spec = importlib.util.spec_from_loader(loader.name, loader)
njord = importlib.util.module_from_spec(spec)
loader.exec_module(njord)


class RelayTests(unittest.IsolatedAsyncioTestCase):
    @classmethod
    def setUpClass(cls):
        cls.certs = tempfile.TemporaryDirectory(prefix="njord-test-")
        cls.cert = str(pathlib.Path(cls.certs.name) / "cert.pem")
        cls.key = str(pathlib.Path(cls.certs.name) / "key.pem")
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
            "-keyout", cls.key, "-out", cls.cert, "-days", "1",
            "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    @classmethod
    def tearDownClass(cls):
        cls.certs.cleanup()

    async def asyncSetUp(self):
        self.servers = []
        self.proxies = []
        self.writers = []
        self.handlers = set()
        self.failures = []

    async def asyncTearDown(self):
        for writer in self.writers:
            writer.close()
        for proxy in self.proxies:
            await proxy.close()
        for server in self.servers:
            server.close()
            await server.wait_closed()
        for task in tuple(self.handlers):
            task.cancel()
        await asyncio.gather(*self.handlers, return_exceptions=True)
        for writer in self.writers:
            try:
                await writer.wait_closed()
            except (OSError, ssl.SSLError):
                pass
        if self.failures:
            raise self.failures[0]

    async def provider(self, handler, tls=False):
        async def connection(reader, writer):
            task = asyncio.current_task()
            self.handlers.add(task)
            self.writers.append(writer)
            try:
                await handler(reader, writer)
            except (asyncio.CancelledError, ConnectionError):
                pass
            except Exception as exc:
                self.failures.append(exc)
            finally:
                writer.close()
                self.handlers.discard(task)

        ctx = None
        if tls:
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(self.cert, self.key)
        server = await asyncio.start_server(connection, "127.0.0.1", 0, ssl=ctx)
        self.servers.append(server)
        return server.sockets[0].getsockname()[1]

    async def proxy(self, port, tls=False, extra=(), host="localhost"):
        argv = ["--upstream", host, "--upstream-port", str(port), "--port", "0"]
        if not tls:
            argv.append("--upstream-plaintext")
        argv.extend(extra)
        proxy = njord.Proxy(njord.parse_args(argv))
        await proxy.start()
        self.proxies.append(proxy)
        return proxy

    async def client(self, proxy, tls=False):
        kwargs = {}
        if tls:
            kwargs = {"ssl": ssl.create_default_context(cafile=self.cert),
                      "server_hostname": "localhost"}
        reader, writer = await asyncio.open_connection(
            "127.0.0.1", proxy.server.sockets[0].getsockname()[1], **kwargs
        )
        self.writers.append(writer)
        return reader, writer

    async def test_large_binary_full_duplex_with_slow_receiver(self):
        sent = bytes(range(256)) * 32769 + b"client tail\x00\r\n"
        response = b"* 1 FETCH (BODY[] {" + str(len(sent)).encode() + b"}\r\n" + sent[::-1]

        async def handler(reader, writer):
            writer.write(response)
            await writer.drain()
            self.assertEqual(await reader.readexactly(len(sent)), sent)
            writer.write(b"\r\n)\r\nA1 OK\r\n")
            await writer.drain()

        proxy = await self.proxy(await self.provider(handler))
        reader, writer = await self.client(proxy)

        async def send():
            writer.write(sent)
            await writer.drain()

        async def receive():
            # Slow consumption forces sustained backpressure. These test read
            # chunks are not message boundaries or limits in the relay.
            received = bytearray()
            while len(received) < len(response):
                block = await reader.read(min(32768, len(response) - len(received)))
                self.assertTrue(block)
                received.extend(block)
                await asyncio.sleep(0.001)
            self.assertEqual(received, response)
            self.assertEqual(await reader.read(), b"\r\n)\r\nA1 OK\r\n")

        await asyncio.wait_for(asyncio.gather(send(), receive()), 30)

    async def test_long_command_and_tcp_half_close(self):
        command = b"A1 APPEND INBOX {" + b"9" * 300000 + b"}\r\n" + bytes(range(256))

        async def handler(reader, writer):
            self.assertEqual(await reader.read(), command)
            writer.write(b"A1 OK after EOF\r\n")
            await writer.drain()

        proxy = await self.proxy(await self.provider(handler))
        reader, writer = await self.client(proxy)
        writer.write(command)
        await writer.drain()
        writer.write_eof()
        self.assertEqual(await asyncio.wait_for(reader.read(), 10), b"A1 OK after EOF\r\n")

    async def test_repeated_and_concurrent_sessions_remain_independent(self):
        async def handler(reader, writer):
            writer.write(b"* OK provider\r\n")
            await writer.drain()
            payload = await reader.readexactly(128)
            writer.write(payload)
            await writer.drain()

        proxy = await self.proxy(await self.provider(handler))

        async def roundtrip(number):
            reader, writer = await self.client(proxy)
            self.assertEqual(await reader.readline(), b"* OK provider\r\n")
            message = number.to_bytes(4, "big") * 32
            writer.write(message)
            await writer.drain()
            self.assertEqual(await reader.read(), message)
            writer.close()
            await writer.wait_closed()

        for start in range(0, 64, 8):
            await asyncio.wait_for(asyncio.gather(*(roundtrip(i) for i in range(start, start + 8))), 10)
        await proxy.close()
        self.assertFalse(proxy.sessions)

    async def test_tls_listener_and_verified_tls_upstream(self):
        async def handler(reader, writer):
            writer.write(b"* OK TLS\r\n")
            await writer.drain()
            self.assertEqual(await reader.readline(), b"A1 NOOP\r\n")
            writer.write(b"A1 OK\r\n")
            await writer.drain()

        port = await self.provider(handler, tls=True)
        proxy = await self.proxy(port, tls=True, extra=[
            "--upstream-ca", self.cert, "--certfile", self.cert, "--keyfile", self.key,
        ])
        reader, writer = await self.client(proxy, tls=True)
        writer.write(b"A1 NOOP\r\n")
        await writer.drain()
        self.assertEqual(await asyncio.wait_for(reader.readline(), 10), b"* OK TLS\r\n")
        self.assertEqual(await asyncio.wait_for(reader.read(), 10), b"A1 OK\r\n")

    async def test_untrusted_upstream_certificate_is_rejected(self):
        async def handler(reader, writer):
            self.fail("An untrusted TLS session must not reach IMAP")

        port = await self.provider(handler, tls=True)
        proxy = await self.proxy(port, tls=True)
        with self.assertLogs("njord", level="WARNING"):
            reader, _ = await self.client(proxy)
            self.assertEqual(await asyncio.wait_for(reader.read(), 10), b"")

    async def test_wrong_upstream_hostname_is_rejected(self):
        async def handler(reader, writer):
            self.fail("A hostname mismatch must not reach IMAP")

        port = await self.provider(handler, tls=True)
        proxy = await self.proxy(port, tls=True, host="127.0.0.1", extra=["--upstream-ca", self.cert])
        with self.assertLogs("njord", level="WARNING"):
            reader, _ = await self.client(proxy)
            self.assertEqual(await asyncio.wait_for(reader.read(), 10), b"")

    async def test_failed_upstream_disconnects_client(self):
        # A bound but non-listening TCP socket reserves an unavailable endpoint.
        with socket.socket() as unavailable:
            unavailable.bind(("127.0.0.1", 0))
            proxy = await self.proxy(unavailable.getsockname()[1], host="127.0.0.1")
            with self.assertLogs("njord", level="WARNING"):
                reader, _ = await self.client(proxy)
                self.assertEqual(await asyncio.wait_for(reader.read(), 10), b"")

    async def test_shutdown_cancels_stalled_tls_handshake(self):
        async def handler(reader, writer):
            await reader.read()

        port = await self.provider(handler)
        proxy = await self.proxy(port, tls=True)
        reader, _ = await self.client(proxy)
        await asyncio.sleep(0.05)
        await asyncio.wait_for(proxy.close(), 5)
        self.assertEqual(await asyncio.wait_for(reader.read(), 5), b"")
        self.assertFalse(proxy.sessions)

    async def test_connection_timeout_does_not_leave_client_waiting(self):
        async def handler(reader, writer):
            await reader.read()

        port = await self.provider(handler)
        proxy = await self.proxy(port, tls=True, extra=["--connect-timeout", "0.1"])
        with self.assertLogs("njord", level="WARNING"):
            reader, _ = await self.client(proxy)
            self.assertEqual(await asyncio.wait_for(reader.read(), 5), b"")

    async def test_shutdown_cancels_stalled_listener_tls(self):
        proxy = await self.proxy(993, extra=["--certfile", self.cert, "--keyfile", self.key])
        reader, _ = await self.client(proxy)
        await asyncio.sleep(0.05)
        self.assertTrue(proxy.sessions)
        await asyncio.wait_for(proxy.close(), 5)
        self.assertEqual(await asyncio.wait_for(reader.read(), 5), b"")
        self.assertFalse(proxy.sessions)

    async def test_invalid_upstream_hostname_disconnects_client(self):
        proxy = await self.proxy(993, host="a" * 64 + ".example.com")
        with self.assertLogs("njord", level="WARNING"):
            reader, _ = await self.client(proxy)
            self.assertEqual(await asyncio.wait_for(reader.read(), 5), b"")

    async def test_stalled_tls_shutdown_retires_session(self):
        release = threading.Event()
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(self.cert, self.key)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            listener.settimeout(10)

            def stalled_provider():
                conn, _ = listener.accept()
                with ctx.wrap_socket(conn, server_side=True) as secure:
                    secure.sendall(b"* OK provider\r\n")
                    # Deliberately never consume/answer the client's close_notify.
                    release.wait(10)

            worker = asyncio.get_running_loop().run_in_executor(None, stalled_provider)
            try:
                proxy = await self.proxy(listener.getsockname()[1], tls=True,
                                         extra=["--upstream-ca", self.cert, "--shutdown-timeout", "0.1"])
                reader, writer = await self.client(proxy)
                self.assertEqual(await asyncio.wait_for(reader.readline(), 5), b"* OK provider\r\n")
                with self.assertLogs("njord", level="WARNING"):
                    writer.close()
                    await writer.wait_closed()
                    async def retired():
                        while proxy.sessions:
                            await asyncio.sleep(0.01)
                    await asyncio.wait_for(retired(), 5)
            finally:
                release.set()
                await worker


if __name__ == "__main__":
    unittest.main()
