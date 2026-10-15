"""Real loopback integration tests; no mail account or external network needed."""

import asyncio
import ctypes
import ctypes.util
import os
import re
import pathlib
import socket
import ssl
import subprocess
import tempfile
import threading
import unittest
import struct
import zlib

ROOT = pathlib.Path(__file__).resolve().parent
BINARY = pathlib.Path(os.environ.get("NJORDBIN", ROOT / "njord")).resolve()


class Zstd:
    """Exercise the published wire format independently through libzstd's C API."""
    def __init__(self):
        library = os.environ.get("ZSTDLIB") or ctypes.util.find_library("zstd")
        if not library:
            directory = subprocess.check_output(["pkg-config", "--variable=libdir", "libzstd"], text=True).strip()
            library = str(pathlib.Path(directory) / "libzstd.dylib")
        self.lib = ctypes.CDLL(library)
        size = ctypes.c_size_t
        pointer = ctypes.c_void_p
        signatures = {
            "ZSTD_compressBound": (size, [size]),
            "ZSTD_compress": (size, [pointer, size, pointer, size, ctypes.c_int]),
            "ZSTD_getFrameContentSize": (ctypes.c_ulonglong, [pointer, size]),
            "ZSTD_decompress": (size, [pointer, size, pointer, size]),
            "ZSTD_isError": (ctypes.c_uint, [size]),
        }
        for name, (result, args) in signatures.items():
            function = getattr(self.lib, name)
            function.restype, function.argtypes = result, args

    def compress(self, data):
        output = ctypes.create_string_buffer(self.lib.ZSTD_compressBound(len(data)))
        size = self.lib.ZSTD_compress(output, len(output), data, len(data), 22)
        assert not self.lib.ZSTD_isError(size)
        return output.raw[:size]

    def decompress(self, frame):
        size = self.lib.ZSTD_getFrameContentSize(frame, len(frame))
        assert size < 2**64 - 2
        output = ctypes.create_string_buffer(size)
        result = self.lib.ZSTD_decompress(output, size, frame, len(frame))
        assert not self.lib.ZSTD_isError(result) and result == size
        return output.raw[:size]


class ProxyProcess:
    def __init__(self, process, port, logs):
        self.process = process
        self.port = port
        self.logs = logs
        self.reader = asyncio.create_task(self.read_logs())

    async def read_logs(self):
        while True:
            line = await self.process.stderr.readline()
            if not line:
                break
            self.logs.append(line.decode(errors="replace"))

    async def close(self):
        if self.process.returncode is None:
            self.process.terminate()
            try:
                await asyncio.wait_for(self.process.wait(), 5)
            except asyncio.TimeoutError:
                self.process.kill()
                await self.process.wait()
                raise
        await self.reader
        if self.process.returncode != 0:
            raise AssertionError("Njord exited %s: %s" % (self.process.returncode, "".join(self.logs)))

    async def wait_log(self, text):
        async def find():
            while not any(text in line for line in self.logs):
                if self.process.returncode is not None:
                    raise AssertionError("Njord stopped: " + "".join(self.logs))
                await asyncio.sleep(0.01)
        await asyncio.wait_for(find(), 5)


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

    async def provider(self, handler, tls=False, host="127.0.0.1"):
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
        server = await asyncio.start_server(connection, host, 0, ssl=ctx)
        self.servers.append(server)
        return server.sockets[0].getsockname()[1]

    async def proxy(self, port, tls=False, extra=(), host="localhost", compressed=False):
        argv = ["--upstream", host, "--upstream-port", str(port), "--port", "0"]
        if not compressed:
            argv.append("--raw")
        if not tls:
            argv.append("--upstream-plaintext")
        argv.extend(extra)
        process = await asyncio.create_subprocess_exec(str(BINARY), *argv, "--verbose",
                                                       stdout=asyncio.subprocess.DEVNULL,
                                                       stderr=asyncio.subprocess.PIPE)
        logs = []
        try:
            while True:
                line = await asyncio.wait_for(process.stderr.readline(), 5)
                if not line:
                    raise AssertionError("Njord failed to start: " + "".join(logs))
                logs.append(line.decode(errors="replace"))
                match = re.search(r"listening on \[.*\]:(\d+);", logs[-1])
                if match:
                    proxy = ProxyProcess(process, int(match.group(1)), logs)
                    self.proxies.append(proxy)
                    return proxy
        except BaseException:
            if process.returncode is None:
                process.kill()
            await process.wait()
            raise

    async def client(self, proxy, tls=False, host="127.0.0.1"):
        kwargs = {}
        if tls:
            kwargs = {"ssl": ssl.create_default_context(cafile=self.cert),
                      "server_hostname": "localhost"}
        reader, writer = await asyncio.open_connection(
            host, proxy.port, **kwargs
        )
        self.writers.append(writer)
        return reader, writer

    async def large_duplex(self, tls=False):
        sent = bytes(range(256)) * 32769 + b"client tail\x00\r\n"
        response = b"* 1 FETCH (BODY[] {" + str(len(sent)).encode() + b"}\r\n" + sent[::-1]
        consumed = asyncio.Event()

        async def handler(reader, writer):
            writer.write(response)
            await writer.drain()
            self.assertEqual(await reader.readexactly(len(sent)), sent)
            # Keep TLS alive while the slow reader drains. Python 3.8's SSL
            # transport cannot resume a buffered reader after close_notify.
            await consumed.wait()
            writer.write(b"\r\n)\r\nA1 OK\r\n")
            await writer.drain()

        extra = ["--upstream-ca", self.cert, "--certfile", self.cert, "--keyfile", self.key] if tls else []
        proxy = await self.proxy(await self.provider(handler, tls=tls), tls=tls, extra=extra)
        reader, writer = await self.client(proxy, tls=tls)

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
            consumed.set()
            self.assertEqual(await reader.read(), b"\r\n)\r\nA1 OK\r\n")

        await asyncio.wait_for(asyncio.gather(send(), receive()), 30)

    async def test_large_binary_full_duplex_with_slow_receiver(self):
        await self.large_duplex()

    async def test_large_tls_full_duplex_with_slow_receiver(self):
        await self.large_duplex(tls=True)

    async def compressed_client(self, proxy, tls=False):
        reader, writer = await self.client(proxy, tls=tls)
        writer.write(b"NJORD1\r\n")
        await writer.drain()
        self.assertEqual(await reader.readexactly(8), b"NJORD1\r\n")
        return reader, writer

    async def compressed_transfer(self, tls=False):
        # Multiple codec frames and literal-looking binary data in each direction.
        data = (b"Subject: a repeated subject\r\n" + bytes(range(256))) * 8193
        response = data[::-1] + b"\r\nA1 OK\r\n"
        async def handler(reader, writer):
            async def send():
                writer.write(response)
                await writer.drain()
            sending = asyncio.create_task(send())
            self.assertEqual(await reader.readexactly(len(data)), data)
            await sending
        extra = ["--upstream-ca", self.cert, "--certfile", self.cert, "--keyfile", self.key] if tls else []
        proxy = await self.proxy(await self.provider(handler, tls=tls), tls=tls,
                                 extra=extra, compressed=True)
        reader, writer = await self.compressed_client(proxy, tls=tls)
        codec = Zstd()
        async def send():
            for start in range(0, len(data), 131072):
                frame = codec.compress(data[start:start + 131072])
                writer.write(len(frame).to_bytes(4, "big") + frame)
                await writer.drain()
        async def receive():
            received = bytearray()
            wire_bytes = 0
            while len(received) < len(response):
                size = int.from_bytes(await reader.readexactly(4), "big")
                frame = await reader.readexactly(size)
                wire_bytes += size + 4
                received.extend(codec.decompress(frame))
                await asyncio.sleep(0.001)
            self.assertEqual(received, response)
            self.assertLess(wire_bytes, len(response) // 2)
        await asyncio.wait_for(asyncio.gather(send(), receive()), 30)

    async def test_compressed_duplex(self):
        await self.compressed_transfer()

    async def test_compressed_tls_duplex(self):
        await self.compressed_transfer(tls=True)

    async def test_compressed_bad_frame_retires_only_its_session(self):
        async def handler(reader, writer):
            writer.write(b"* OK hello\r\n")
            await writer.drain()
            await reader.read()
        proxy = await self.proxy(await self.provider(handler), compressed=True)
        reader, writer = await self.compressed_client(proxy)
        writer.write(b"\0\0\0\x04junk")
        await writer.drain()
        await proxy.wait_log("invalid Njord zstd frame boundary")
        await proxy.wait_log("session closed")
        reader, writer = await self.compressed_client(proxy)
        size = int.from_bytes(await reader.readexactly(4), "big")
        self.assertEqual(Zstd().decompress(await reader.readexactly(size)), b"* OK hello\r\n")

    async def test_compressed_fragmented_frame_and_half_close(self):
        data = b"A1 APPEND {300000}\r\n" + bytes(range(256)) * 1172
        async def handler(reader, writer):
            self.assertEqual(await reader.read(), data)
            writer.write(b"A1 OK after half close\r\n")
            await writer.drain()
        proxy = await self.proxy(await self.provider(handler), compressed=True)
        reader, writer = await self.compressed_client(proxy)
        frame = Zstd().compress(data)
        wire = len(frame).to_bytes(4, "big") + frame
        for start in range(0, len(wire), 3):
            writer.write(wire[start:start + 3])
            await writer.drain()
        writer.write_eof()
        size = int.from_bytes(await reader.readexactly(4), "big")
        self.assertEqual(Zstd().decompress(await reader.readexactly(size)), b"A1 OK after half close\r\n")

    @unittest.skipUnless(os.environ.get("SKADITEST"), "set SKADITEST to the c-client integration executable")
    async def test_skadi_direct_and_compressed_batches(self):
        for compressed in (False, True):
            fetches = []
            structure = (b'(("TEXT" "PLAIN" ("CHARSET" "UTF-8") NIL NIL "7BIT" 10 1 NIL NIL NIL NIL)'
                         b'("APPLICATION" "OCTET-STREAM" ("NAME" "payload.bin") NIL NIL "BASE64" 123456 NIL '
                         b'("ATTACHMENT" ("FILENAME" "payload.bin")) NIL NIL) "MIXED" '
                         b'("BOUNDARY" "boundary") NIL NIL NIL)')
            async def handler(reader, writer):
                writer.write(b"* PREAUTH [CAPABILITY IMAP4rev1] test server\r\n")
                await writer.drain()
                while True:
                    line = await reader.readline()
                    if not line:
                        return
                    tag, command = line.rstrip(b"\r\n").split(b" ", 1)
                    if command.startswith(b"CAPABILITY"):
                        writer.write(b"* CAPABILITY IMAP4rev1\r\n" + tag + b" OK capabilities\r\n")
                    elif command.startswith((b"SELECT ", b"EXAMINE ")):
                        writer.write(b"* FLAGS (\\Seen)\r\n* 2 EXISTS\r\n* 0 RECENT\r\n"
                                     b"* OK [UIDVALIDITY 1234] identity\r\n* OK [UIDNEXT 103] next\r\n" +
                                     tag + b" OK [READ-ONLY] selected\r\n")
                    elif command.startswith(b"UID FETCH "):
                        fetches.append(command)
                        bodies = b"BODY.PEEK[]" in command
                        headers = b"BODY.PEEK[HEADER]" in command
                        selection = command.split()[2].decode()
                        def selected(uid):
                            for component in selection.split(","):
                                ends = component.split(":")
                                low = int(ends[0]) if ends[0] != "*" else 102
                                high = int(ends[-1]) if ends[-1] != "*" else 102
                                if min(low,high) <= uid <= max(low,high): return True
                            return False
                        for number in (1, 2):
                            if not selected(100+number): continue
                            head = (b"Subject: cached subject\r\nFrom: sender@example.invalid\r\n"
                                    b"MIME-Version: 1.0\r\nContent-Type: multipart/mixed; boundary=boundary\r\n\r\n")
                            payload = head + b"batch-body\x00binary\r\n" * 10000 if bodies else head
                            prefix = b"* %d FETCH (UID %d " % (number, 100 + number)
                            if not bodies:
                                prefix += b"FLAGS () RFC822.SIZE 200000 BODYSTRUCTURE " + structure + b" "
                            if bodies or headers:
                                prefix += (b"BODY[]" if bodies else b"BODY[HEADER]") + b" {%d}\r\n" % len(payload)
                                writer.write(prefix + payload + b")\r\n")
                            else:
                                writer.write(prefix + b")\r\n")
                            await writer.drain()
                        writer.write(tag + b" OK fetched\r\n")
                    elif command == b"LOGOUT":
                        writer.write(b"* BYE closing\r\n" + tag + b" OK logout\r\n")
                        await writer.drain()
                        return
                    else:
                        self.fail("Unexpected IMAP command: " + repr(command))
                    await writer.drain()
            port = await self.provider(handler)
            if compressed:
                proxy = await self.proxy(port, compressed=True)
                port = proxy.port
            mailbox = "{127.0.0.1:%d/imap/notls%s}INBOX" % (port, "/njord" if compressed else "")
            with tempfile.TemporaryDirectory(prefix="njord-skadi-") as home:
                process = await asyncio.create_subprocess_exec(os.environ["SKADITEST"], home, mailbox,
                                                               stdout=asyncio.subprocess.PIPE,
                                                               stderr=asyncio.subprocess.PIPE)
                stdout, stderr = await asyncio.wait_for(process.communicate(), 30)
                self.assertEqual(process.returncode, 0, (stdout, stderr))
            self.assertEqual(len(fetches), 8)
            self.assertGreater(len(fetches[1]), 20000)
            self.assertIn(b"1:100,103:4294967295",fetches[6])

    async def image_request(self, proxy, command):
        reader, writer = await self.client(proxy)
        writer.write(b"NJIMG1\r\n")
        await writer.drain()
        self.assertEqual(await reader.readexactly(8), b"NJIMG1\r\n")
        codec = Zstd()
        frame = codec.compress(command + b"\r\n")
        writer.write(len(frame).to_bytes(4,"big") + frame)
        await writer.drain()
        data = bytearray()
        while True:
            try:
                size = int.from_bytes(await reader.readexactly(4),"big")
            except asyncio.IncompleteReadError as e:
                self.assertEqual(e.partial,b"")
                break
            data.extend(codec.decompress(await reader.readexactly(size)))
        children = pathlib.Path("/proc/%d/task/%d/children" % (proxy.process.pid,proxy.process.pid))
        if children.exists():
            async def reaped():
                while children.read_text().strip(): await asyncio.sleep(0.01)
            await asyncio.wait_for(reaped(),5)
        return bytes(data)

    async def test_image_preview_original_redirects_and_repeated_use(self):
        def chunk(kind, data):
            return struct.pack("!I",len(data)) + kind + data + struct.pack("!I",zlib.crc32(kind+data))
        # Larger than the former 3 MiB image limit; no optional imaging library.
        w, h = 1100, 1100
        png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR",struct.pack("!IIBBBBB",w,h,8,2,0,0,0)) +
               chunk(b"IDAT",zlib.compress(b"".join(b"\0"+os.urandom(w*3) for _ in range(h)))) + chunk(b"IEND",b""))
        requests = []
        async def handler(reader, writer):
            line = await reader.readline()
            if not line: return
            requests.append(line)
            while True:
                header = await reader.readline()
                if not header: return
                if header == b"\r\n": break
            if b"/downgrade " in line:
                writer.write(b"HTTP/1.1 302 Found\r\nLocation: http://localhost/image.png\r\nContent-Length: 0\r\n\r\n")
            elif b"/loop " in line:
                writer.write(b"HTTP/1.1 302 Found\r\nLocation: /loop\r\nContent-Length: 0\r\n\r\n")
            else:
                writer.write(b"HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % len(png) + png)
            await writer.drain()
        port = await self.provider(handler,tls=True,host="127.0.0.1")
        proxy = await self.proxy(1,compressed=True,extra=["--image-ca",self.cert,"--image-allow-private"])
        url = "https://localhost:%d/image.png?%s" % (port,"q"*10000)
        for operation in (b"PREVIEW ",b"ORIGINAL ",b"PREVIEW "):
            response = await asyncio.wait_for(self.image_request(proxy,operation+url.encode()),30)
            head, payload = response.split(b"\r\n",1)
            status, original, length, mime = head.split()
            self.assertEqual(status,b"OK"); self.assertEqual(int(original),len(png)); self.assertEqual(int(length),len(payload))
            if operation == b"ORIGINAL ":
                self.assertEqual(payload,png)
            else:
                self.assertEqual(mime,b"image/webp"); self.assertEqual(payload[8:12],b"WEBP")
                self.assertLess(len(payload),len(png))
        for path in ("downgrade","loop"):
            response = await self.image_request(proxy,("PREVIEW https://localhost:%d/%s" % (port,path)).encode())
            self.assertTrue(response.startswith(b"ERR "),response)
        self.assertEqual(len(requests),5)
        if os.environ.get("SKADITEST"):
            mailbox = "{127.0.0.1:%d/njord}INBOX" % proxy.port
            with tempfile.TemporaryDirectory(prefix="njord-image-skadi-") as home:
                process = await asyncio.create_subprocess_exec(os.environ["SKADITEST"],home,mailbox,url,
                    stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.PIPE)
                stdout, stderr = await asyncio.wait_for(process.communicate(),30)
                self.assertEqual(process.returncode,0,(stdout,stderr))
            self.assertEqual(len(requests),7) # Cache lookups never fetch again.
        for extra in (["--image-ca",self.cert],["--image-allow-private"]):
            protected = await self.proxy(1,compressed=True,extra=extra)
            self.assertTrue((await self.image_request(protected,b"PREVIEW "+url.encode())).startswith(b"ERR "))

    async def test_ipv6_listener_and_provider(self):
        async def handler(reader, writer):
            writer.write(b"* OK IPv6\r\n")
            await writer.drain()
        port = await self.provider(handler, host="::1")
        proxy = await self.proxy(port, host="::1", extra=["--listen", "::1"])
        reader, _ = await self.client(proxy, host="::1")
        self.assertEqual(await asyncio.wait_for(reader.read(), 5), b"* OK IPv6\r\n")

    async def test_listener_handshake_timeout_leaves_proxy_usable(self):
        async def handler(reader, writer):
            writer.write(b"* OK still here\r\n")
            await writer.drain()
        proxy = await self.proxy(await self.provider(handler), extra=[
            "--certfile", self.cert, "--keyfile", self.key, "--connect-timeout", "0.25"])
        reader, _ = await self.client(proxy)
        self.assertEqual(await asyncio.wait_for(reader.read(), 5), b"")
        await proxy.wait_log("connection setup timed out")
        reader, _ = await self.client(proxy, tls=True)
        self.assertEqual(await asyncio.wait_for(reader.read(), 5), b"* OK still here\r\n")

    async def test_cli_validation_and_version(self):
        result = subprocess.run([str(BINARY), "--version"], capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout, "njord 0.1.0\n")
        for extra in (["--port", "65536"], ["--port", "-1"], ["--upstream-port", "0"],
                      ["--connect-timeout", "nan"], ["--connect-timeout", "inf"],
                      ["--shutdown-timeout", "0"], ["--shutdown-timeout", "1e100"],
                      ["--certfile", self.cert], ["--upstream-plaintext", "--upstream-ca", self.cert]):
            with self.subTest(arguments=extra):
                result = subprocess.run([str(BINARY), "--upstream", "localhost", *extra],
                                        capture_output=True, text=True, timeout=5)
                self.assertNotEqual(result.returncode, 0)

    @unittest.skipUnless(pathlib.Path("/proc/self/fd").exists(), "Linux descriptor accounting")
    async def test_repeated_clients_do_not_accumulate_descriptors(self):
        async def handler(reader, writer):
            writer.write(b"* OK\r\n")
            await writer.drain()
        proxy = await self.proxy(await self.provider(handler), host="127.0.0.1")
        async def exchange():
            reader, writer = await self.client(proxy)
            self.assertEqual(await asyncio.wait_for(reader.read(), 5), b"* OK\r\n")
            writer.close()
            await writer.wait_closed()
        await exchange()
        await proxy.wait_log("session closed")
        descriptors = pathlib.Path("/proc/%d/fd" % proxy.process.pid)
        baseline = len(list(descriptors.iterdir()))
        for _ in range(128):
            await exchange()
        async def retired():
            while sum("session closed" in line for line in proxy.logs) != 129:
                await asyncio.sleep(0.01)
        await asyncio.wait_for(retired(), 5)
        self.assertLessEqual(len(list(descriptors.iterdir())), baseline)

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
        self.assertEqual(proxy.process.returncode, 0)

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
        reader, _ = await self.client(proxy)
        self.assertEqual(await asyncio.wait_for(reader.read(), 10), b"")

    async def test_wrong_upstream_hostname_is_rejected(self):
        async def handler(reader, writer):
            self.fail("A hostname mismatch must not reach IMAP")

        port = await self.provider(handler, tls=True)
        proxy = await self.proxy(port, tls=True, host="127.0.0.1", extra=["--upstream-ca", self.cert])
        reader, _ = await self.client(proxy)
        self.assertEqual(await asyncio.wait_for(reader.read(), 10), b"")

    async def test_failed_upstream_disconnects_client(self):
        # A bound but non-listening TCP socket reserves an unavailable endpoint.
        with socket.socket() as unavailable:
            unavailable.bind(("127.0.0.1", 0))
            proxy = await self.proxy(unavailable.getsockname()[1], host="127.0.0.1")
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
        self.assertEqual(proxy.process.returncode, 0)

    async def test_connection_timeout_does_not_leave_client_waiting(self):
        async def handler(reader, writer):
            await reader.read()

        port = await self.provider(handler)
        proxy = await self.proxy(port, tls=True, extra=["--connect-timeout", "0.1"])
        reader, _ = await self.client(proxy)
        self.assertEqual(await asyncio.wait_for(reader.read(), 5), b"")

    async def test_shutdown_cancels_stalled_listener_tls(self):
        proxy = await self.proxy(993, extra=["--certfile", self.cert, "--keyfile", self.key])
        reader, _ = await self.client(proxy)
        await asyncio.sleep(0.05)
        await asyncio.wait_for(proxy.close(), 5)
        self.assertEqual(await asyncio.wait_for(reader.read(), 5), b"")
        self.assertEqual(proxy.process.returncode, 0)

    async def test_invalid_upstream_hostname_disconnects_client(self):
        proxy = await self.proxy(993, host="a" * 64 + ".example.com")
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
                writer.close()
                await writer.wait_closed()
                await proxy.wait_log("session shutdown timed out")
                await proxy.wait_log("session closed")
            finally:
                release.set()
                await worker


if __name__ == "__main__":
    unittest.main()
