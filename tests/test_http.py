"""Exercise the exact embedded HTTP/API and gzip asset through the host server."""

import gzip
import http.client
import json
import os
import pathlib
import socket
import subprocess
import tempfile
import time
import zipfile

root = pathlib.Path(__file__).resolve().parents[1]


def request(method, path, body=None, headers=None):
    c = http.client.HTTPConnection("127.0.0.1", 8080, timeout=3)
    c.request(method, path, body, headers or {})
    r = c.getresponse()
    data = r.read()
    c.close()
    return r, data


with tempfile.TemporaryDirectory() as d:
    p = subprocess.Popen(
        [str(root / "build/tonie-host"), str(pathlib.Path(d) / "flash.bin")],
        stderr=subprocess.PIPE,
    )
    try:
        r = None
        for _ in range(30):
            if p.poll() is not None:
                raise RuntimeError("Host server exited: " + p.stderr.read().decode())
            try:
                r, b = request("GET", "/")
                break
            except OSError:
                time.sleep(0.05)
        if r is None:
            raise RuntimeError("Host server did not become available on localhost:8080")
        assert r.status == 200 and r.getheader("Content-Encoding") == "gzip"
        assert "font-src data:;" in r.getheader("Content-Security-Policy")
        assert gzip.decompress(b) == (root / "build/index.html").read_bytes()
        assert json.loads(request("GET", "/api/v1/state")[1])["count"] == 0
        assert json.loads(request("GET", "/api/v1/state")[1])["bootloader"] == "dfu"
        assert json.loads(request("GET", "/api/v1/state")[1])[
            "firmware"
        ] == os.environ.get("FIRMWARE_VERSION", "0.1.0-dev")
        # Diagnostic commands must stay absent from normal builds.
        assert (
            request("GET", "/api/v1/test-storage?offset=0&length=1024")[0].status == 404
        )
        assert request("GET", "/api/v1/test-platform")[0].status == 404
        assert request("GET", "/api/v1/test-rf-edges")[0].status == 404
        assert request("POST", "/api/v1/test-idle")[0].status == 400
        assert request("POST", "/api/v1/test-rf-delay?cycles=21112")[0].status == 400
        data = (root / "build/test-slix.nfci").read_bytes()
        h = {"Content-Type": "application/octet-stream"}
        r, scanned = request("POST", "/api/v1/scan", headers=h)
        assert r.status == 200 and scanned[:4] == b"NFCI", scanned
        assert request("POST", "/api/v1/scan", b"bad", h)[0].status == 400
        r, b = request("POST", "/api/v1/import?name=Synthetic", data, h)
        assert r.status == 200, b
        id = json.loads(b)["id"]
        assert request("GET", f"/api/v1/tag?id={id}")[1] == data
        r, emulated = request("GET", f"/api/v1/tag?id={id}&emulation=1")
        assert r.status == 200 and emulated[5] == 1 and emulated[7] == data[7] & ~1
        assert emulated[8:68] == data[8:68] and emulated[88:] == data[88:]
        assert emulated[68:88] == bytes(8) + b"\x0f" * 8 + bytes(4)
        for path in [f"favorite?slot=1&id={id}", f"emulate?id={id}"]:
            assert request("POST", "/api/v1/" + path, headers=h)[0].status == 200
        state = json.loads(request("GET", "/api/v1/state")[1])
        assert state["active"] and state["profile"] == 1 and state["favorites"][0] == id
        assert request("GET", f"/api/v1/tag?id={id}")[1] == data
        unsupported = bytearray(data)
        unsupported[5] = 3
        assert (
            request("POST", "/api/v1/import?name=Unsupported", unsupported, h)[0].status
            == 400
        )
        assert json.loads(request("GET", "/api/v1/state")[1])["count"] == 1
        assert (
            request("POST", "/api/v1/stop", headers={"Origin": "https://evil.example"})[
                0
            ].status
            == 403
        )
        assert request("GET", "/", headers={"Host": "evil.example"})[0].status == 403
        assert (
            request("POST", f"/api/v1/rename?id={id}&name=A%20%26%20B", headers=h)[
                0
            ].status
            == 200
        )
        assert (
            json.loads(request("GET", "/api/v1/tags")[1])["tags"][0]["name"] == "A & B"
        )
        assert not json.loads(request("GET", "/api/v1/state")[1])["active"]
        # Ambiguous framing is rejected before API execution.
        c = socket.create_connection(("127.0.0.1", 8080))
        c.sendall(
            b"POST /api/v1/stop HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nContent-Length: 0\r\nContent-Length: 1\r\n\r\n"
        )
        assert b"400" in c.recv(100)
        c.close()
        for headers in [
            b"Transfer-Encoding: chunked\r\nTransfer-Encoding: identity",
            b"Origin: http://127.0.0.1:8080\r\nOrigin: https://evil.example",
            b"Sec-Fetch-Site: same-origin\r\nSec-Fetch-Site: cross-site",
            b"Content-Length : 0",
            b"X-Test: bad\x00value",
        ]:
            c = socket.create_connection(("127.0.0.1", 8080))
            c.sendall(
                b"POST /api/v1/stop HTTP/1.1\r\nHost: 127.0.0.1:8080\r\n"
                + headers
                + b"\r\n\r\n"
            )
            assert b"400" in c.recv(100)
            c.close()
        p.terminate()
        p.wait()
        p = subprocess.Popen(
            [str(root / "build/tonie-host"), str(pathlib.Path(d) / "flash.bin")],
            stderr=subprocess.PIPE,
        )
        time.sleep(0.1)
        assert json.loads(request("GET", "/api/v1/state")[1])["active"]
        assert request("POST", f"/api/v1/delete?id={id}", headers=h)[0].status == 200
        state = json.loads(request("GET", "/api/v1/state")[1])
        assert state["count"] == 0 and state["favorites"][0] == 0
        request("POST", "/api/v1/import?name=Fresh", data, h)
        assert request("POST", "/api/v1/library-reset", headers=h)[0].status == 200
        state = json.loads(request("GET", "/api/v1/state")[1])
        assert (
            state["ready"]
            and state["count"] == 0
            and state["selected"] == 0
            and not any(state["favorites"])
        )
        with zipfile.ZipFile(root / "build/test-export.zip") as z:
            assert z.testzip() is None and "Synthetic.nfc" in z.namelist()
    finally:
        p.terminate()
        p.wait()
print(
    "HTTP/API: embedded gzip, import/export, favorites, restart, unsupported-family rejection, pause and request guards passed"
)
