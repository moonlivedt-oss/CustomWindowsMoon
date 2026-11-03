"""Loopback integration: per-phone grants and expiry terminate an active download.

Usage: python tests/test_phone_access_live.py build-check/RadialMenu.exe
Uses an isolated device database/config and no firewall rules.
"""
import http.client
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import sys
import time
import uuid
from urllib.parse import urlsplit


def main():
    binary = Path(sys.argv[1]).resolve(strict=True)
    root = binary.parent.resolve()
    fixture = root / ("phone-access-smoke-" + uuid.uuid4().hex)
    fixture.mkdir()
    assert fixture.resolve().is_relative_to(root)
    proc = None
    try:
        app = fixture / "RadialMenu.exe"
        shutil.copy2(binary, app)
        payload = fixture / "payload.bin"
        with payload.open("wb") as f:
            f.truncate(32 * 1024 * 1024)
        now = int(time.time())
        expiry = now + 14
        (fixture / "phone-devices.tsv").write_text(
            f"guestkey12345678\tGuest\t{now}\t{now}\t0\t0\t1\t{expiry}\n"
            f"limitedkey123456\tLimited\t{now}\t{now}\t0\t0\t0\t0\n", encoding="utf-8")
        env = os.environ.copy()
        env.update(RADIAL_PHONE_LOCAL="1", RADIAL_PHONE_DIR=str(fixture))
        proc = subprocess.Popen([str(app), "--phone-serve", "40", "-", str(payload)], env=env,
                                creationflags=subprocess.CREATE_NO_WINDOW)
        until = time.monotonic() + 10
        address = None
        while time.monotonic() < until:
            report = fixture / "phone_out.txt"
            if report.exists():
                lines = report.read_text(encoding="utf-8").splitlines()
                if len(lines) > 1 and lines[1].startswith("https://"):
                    address = urlsplit(lines[1])
                    break
            if proc.poll() is not None:
                raise AssertionError("test server exited before startup")
            time.sleep(.1)
        assert address is not None, "TLS server did not start"
        context = ssl._create_unverified_context()  # self-signed fixture on 127.0.0.1 only
        assert address.hostname == "127.0.0.1"

        def request(key, path):
            c = http.client.HTTPSConnection(address.hostname, address.port, context=context, timeout=5)
            c.request("GET", "/d/" + key + path)
            r = c.getresponse()
            status, body = r.status, r.read()
            c.close()
            return status, body

        code, body = request("limitedkey123456", "/poll?seq=-1")
        assert code == 200
        data = json.loads(body)
        assert data["files"] == [] and data["shares"] == []
        assert request("limitedkey123456", "/f/0")[0] == 403
        assert request("limitedkey123456", "/mons")[0] == 403
        assert request("guestkey12345678", "/poll?seq=-1")[0] == 200
        download = http.client.HTTPSConnection(address.hostname, address.port, context=context, timeout=5)
        download.connect()
        import socket
        download.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        download.request("GET", "/d/guestkey12345678/f/0")
        response = download.getresponse()
        assert response.status == 200
        assert response.read(1) == b"\0"
        time.sleep(max(0, expiry - time.time()) + 1)
        terminated = False
        try:
            response.read()
        except (http.client.IncompleteRead, OSError):
            terminated = True
        finally:
            download.close()
        assert terminated, "expired guest download was allowed to finish"
        assert request("guestkey12345678", "/poll?seq=-1")[0] in (403, 404)
        assert request("limitedkey123456", "/poll?seq=-1")[0] == 200, "other phone was revoked too"
        print("PASS: independent grants; hidden file metadata; guest expiry; active stream revoked; other phone retained")
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            proc.wait(timeout=10)
        assert fixture.resolve().is_relative_to(root) and fixture != root
        shutil.rmtree(fixture)


if __name__ == "__main__":
    main()
