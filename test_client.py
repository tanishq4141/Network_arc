#!/usr/bin/env python3
"""
Test client that mimics the grader: opens ONE TCP socket and sends all
requests over it, then checks that the socket is still open.
"""

import socket
import sys


def read_http_response(sock: socket.socket) -> tuple[int, dict[str, str], str]:
    """Read one complete HTTP response from a persistent connection."""
    buf = b""

    # 1. Read until we have the full header block.
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("Server closed connection unexpectedly")
        buf += chunk

    header_end = buf.index(b"\r\n\r\n")
    header_block = buf[:header_end].decode()
    rest = buf[header_end + 4 :]

    # 2. Parse status line.
    lines = header_block.split("\r\n")
    status_line = lines[0]  # e.g. "HTTP/1.1 200 OK"
    status_code = int(status_line.split(" ", 2)[1])

    # 3. Parse headers.
    headers: dict[str, str] = {}
    for line in lines[1:]:
        if ":" in line:
            key, val = line.split(":", 1)
            headers[key.strip().lower()] = val.strip()

    # 4. Read body according to Content-Length.
    content_length = int(headers.get("content-length", "0"))
    while len(rest) < content_length:
        chunk = sock.recv(4096)
        if not chunk:
            raise ConnectionError("Server closed connection while reading body")
        rest += chunk

    body = rest[:content_length].decode()
    # Any bytes beyond content_length are part of the next response — but
    # since we send requests sequentially and wait for each response, there
    # shouldn't be any.

    return status_code, headers, body


def main() -> None:
    host = "localhost"
    port = 8080

    tests = [
        # (raw_request, expected_status, expected_body_or_None)
        (
            "GET /add?a=2&b=3 HTTP/1.1\r\nHost: localhost\r\n\r\n",
            200,
            "5",
        ),
        (
            "GET /sub?a=10&b=4 HTTP/1.1\r\nHost: localhost\r\n\r\n",
            200,
            "6",
        ),
        (
            "GET /mul?a=6&b=7 HTTP/1.1\r\nHost: localhost\r\n\r\n",
            200,
            "42",
        ),
        (
            "GET /div?a=1&b=0 HTTP/1.1\r\nHost: localhost\r\n\r\n",
            400,
            None,  # body doesn't matter for errors
        ),
        (
            "GET /pow?a=2&b=8 HTTP/1.1\r\nHost: localhost\r\n\r\n",
            404,
            None,
        ),
        (
            "POST /add HTTP/1.1\r\nHost: localhost\r\n\r\n",
            405,
            None,
        ),
    ]

    # --- Additional edge-case tests ---
    extra_tests = [
        # Missing Host header → 400
        (
            "GET /add?a=1&b=2 HTTP/1.1\r\n\r\n",
            400,
            None,
        ),
        # Non-numeric parameter → 400
        (
            "GET /add?a=x&b=3 HTTP/1.1\r\nHost: localhost\r\n\r\n",
            400,
            None,
        ),
        # Division: normal case
        (
            "GET /div?a=9&b=3 HTTP/1.1\r\nHost: localhost\r\n\r\n",
            200,
            "3",
        ),
    ]

    print(f"Connecting to {host}:{port} ...")
    s = socket.create_connection((host, port))
    print("Connected (1 TCP handshake)\n")

    passed = 0
    failed = 0
    total = len(tests) + len(extra_tests)

    for i, (raw, expected_status, expected_body) in enumerate(
        tests + extra_tests, start=1
    ):
        label = raw.split("\r\n")[0]  # e.g. "GET /add?a=2&b=3 HTTP/1.1"
        s.sendall(raw.encode())

        status, _headers, body = read_http_response(s)

        ok = True
        if status != expected_status:
            ok = False
        if expected_body is not None and body != expected_body:
            ok = False

        tag = "✅ PASS" if ok else "❌ FAIL"
        print(f"  [{i}/{total}] {tag}  {label}")
        if not ok:
            print(f"         expected status={expected_status} body={expected_body!r}")
            print(f"         got      status={status} body={body!r}")
            failed += 1
        else:
            passed += 1

    # Check socket is still open by peeking
    s.setblocking(False)
    still_open = True
    try:
        data = s.recv(1, socket.MSG_PEEK)
        if data == b"":
            still_open = False
    except BlockingIOError:
        still_open = True  # no data available but socket is alive
    except Exception:
        still_open = False

    print()
    print(f"socket still open: {still_open}")
    print(f"1 TCP handshake, {len(tests) + len(extra_tests)} responses")
    print(f"\nResults: {passed}/{total} passed, {failed}/{total} failed")

    s.close()

    if failed > 0 or not still_open:
        print("\n⚠️  Some tests failed or socket was closed prematurely.")
        sys.exit(1)
    else:
        print("\n🎉 All tests passed!")
        sys.exit(0)


if __name__ == "__main__":
    main()
