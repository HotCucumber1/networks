import concurrent.futures
import os
import socket
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COUNTS = {}
COUNTS_LOCK = threading.Lock()

class OriginHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        with COUNTS_LOCK:
            COUNTS[self.path] = COUNTS.get(self.path, 0) + 1
        if self.path == "/missing":
            body = b"not found"
            self.send_response(404)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self.close_connection = True
            return
        if self.path == "/slow":
            time.sleep(0.4)
            body = b"delayed-response"
            self.send_response(200)
            self.send_header("Cache-Control", "max-age=120")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self.close_connection = True
            return
        if self.path == "/nostore":
            body = b"do-not-cache"
            self.send_response(200)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self.close_connection = True
            return
        if self.path == "/large":
            body = b"Z" * (2 * 1024 * 1024)
            self.send_response(200)
            self.send_header("Cache-Control", "max-age=120")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self.close_connection = True
            return
        if self.path == "/revalidate":
            if self.headers.get("If-None-Match") == '"v1"':
                self.send_response(304)
                self.send_header("Cache-Control", "max-age=120")
                self.send_header("ETag", '"v1"')
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                self.close_connection = True
                return
            body = b"version-one"
            self.send_response(200)
            self.send_header("Cache-Control", "max-age=0")
            self.send_header("ETag", '"v1"')
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            self.close_connection = True
            return
        body = ("origin:" + self.path).encode()
        self.send_response(200)
        self.send_header("Cache-Control", "max-age=120")
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        result = b"received:" + body
        self.send_response(200)
        self.send_header("Content-Length", str(len(result)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(result)
        self.close_connection = True

    def log_message(self, fmt, *args):
        pass


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def proxy_request(proxy_port, origin_port, path, method="GET", body=b""):
    sock = socket.create_connection(("127.0.0.1", proxy_port), timeout=20)
    headers = [f"{method} http://127.0.0.1:{origin_port}{path} HTTP/1.1", f"Host: 127.0.0.1:{origin_port}", "Connection: close"]
    if method == "POST":
        headers.append(f"Content-Length: {len(body)}")
    payload = ("\r\n".join(headers) + "\r\n\r\n").encode() + body
    sock.sendall(payload)
    chunks = []
    while True:
        data = sock.recv(65536)
        if not data:
            break
        chunks.append(data)
    sock.close()
    return b"".join(chunks)


def body_of(response):
    return response.split(b"\r\n\r\n", 1)[1]


def status_of(response):
    return int(response.split(b" ", 2)[1])


def main():
    origin_port = free_port()
    proxy_port = free_port()
    origin = ThreadingHTTPServer(("127.0.0.1", origin_port), OriginHandler)
    origin_thread = threading.Thread(target=origin.serve_forever, daemon=True)
    origin_thread.start()
    with tempfile.TemporaryDirectory() as cache_dir:
        process = subprocess.Popen([str(ROOT / "build" / "proxy_server"), str(proxy_port), "8", cache_dir], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            for _ in range(50):
                try:
                    probe = socket.create_connection(("127.0.0.1", proxy_port), timeout=0.2)
                    probe.close()
                    break
                except OSError:
                    time.sleep(0.1)
            else:
                raise RuntimeError("Proxy did not start")

            first = proxy_request(proxy_port, origin_port, "/cached")
            second = proxy_request(proxy_port, origin_port, "/cached")
            assert status_of(first) == 200 and status_of(second) == 200
            assert body_of(first) == body_of(second)
            assert COUNTS["/cached"] == 1, "GET cache hit did not avoid origin request"

            start = time.perf_counter()
            slow_first = proxy_request(proxy_port, origin_port, "/slow")
            first_duration = time.perf_counter() - start
            start = time.perf_counter()
            slow_second = proxy_request(proxy_port, origin_port, "/slow")
            second_duration = time.perf_counter() - start
            assert body_of(slow_first) == body_of(slow_second)
            assert second_duration < first_duration, "Cache hit did not improve response time"

            missing_one = proxy_request(proxy_port, origin_port, "/missing")
            missing_two = proxy_request(proxy_port, origin_port, "/missing")
            assert status_of(missing_one) == 404 and status_of(missing_two) == 404
            assert COUNTS["/missing"] == 2, "404 response was unexpectedly cached"

            proxy_request(proxy_port, origin_port, "/nostore")
            proxy_request(proxy_port, origin_port, "/nostore")
            assert COUNTS["/nostore"] == 2, "Cache-Control: no-store was ignored"

            posted = proxy_request(proxy_port, origin_port, "/echo", "POST", b"hello=world")
            assert status_of(posted) == 200 and body_of(posted) == b"received:hello=world"

            large = proxy_request(proxy_port, origin_port, "/large")
            assert status_of(large) == 200 and len(body_of(large)) == 2 * 1024 * 1024

            stale_first = proxy_request(proxy_port, origin_port, "/revalidate")
            stale_second = proxy_request(proxy_port, origin_port, "/revalidate")
            stale_third = proxy_request(proxy_port, origin_port, "/revalidate")
            assert body_of(stale_first) == b"version-one"
            assert body_of(stale_second) == b"version-one"
            assert body_of(stale_third) == b"version-one"
            assert COUNTS["/revalidate"] == 2, "ETag revalidation did not work as expected"

            paths = [f"/parallel-{index}" for index in range(24)]
            with concurrent.futures.ThreadPoolExecutor(max_workers=12) as executor:
                results = list(executor.map(lambda path: proxy_request(proxy_port, origin_port, path), paths))
            assert all(status_of(result) == 200 for result in results)

            process.terminate()
            process.wait(timeout=5)
            process = subprocess.Popen([str(ROOT / "build" / "proxy_server"), str(proxy_port), "8", cache_dir], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            for _ in range(50):
                try:
                    probe = socket.create_connection(("127.0.0.1", proxy_port), timeout=0.2)
                    probe.close()
                    break
                except OSError:
                    time.sleep(0.1)
            persisted = proxy_request(proxy_port, origin_port, "/cached")
            assert status_of(persisted) == 200 and COUNTS["/cached"] == 1, "Disk cache did not survive proxy restart"

            print("PASS: GET and disk cache")
            print(f"PASS: cache timing first={first_duration * 1000:.1f} ms, hit={second_duration * 1000:.1f} ms")
            print("PASS: 404 is not cached")
            print("PASS: Cache-Control no-store is respected")
            print("PASS: POST body forwarding")
            print("PASS: 2 MiB response")
            print("PASS: ETag revalidation after stale cache")
            print("PASS: 24 concurrent client requests")
            print("PASS: cache survives proxy restart")
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            output = process.stdout.read() if process.stdout else ""
            if "CACHE HIT" not in output:
                print("Proxy log did not contain CACHE HIT")
                print(output[-4000:])
                raise AssertionError("Expected cache hit log")
    origin.shutdown()
    origin.server_close()

if __name__ == "__main__":
    main()
