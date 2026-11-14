#!/usr/bin/env python3
"""Self-test for updatesrv: HTTP + HTTPS table and package fetch."""
import hashlib
import json
import os
import ssl
import subprocess
import sys
import tempfile
import threading
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import updatesrv  # noqa: E402

fails = 0


def ck(name, ok):
    global fails
    print("%-42s %s" % (name, "OK" if ok else "FAIL"))
    if not ok:
        fails += 1


def fetch(url, insecure=False):
    if insecure:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        return urllib.request.urlopen(url, context=ctx).read()
    return urllib.request.urlopen(url).read()


def main():
    tmp = tempfile.mkdtemp(prefix="updsrv.")
    pkg = os.path.join(tmp, "demo.npth")
    blob = bytes(range(256)) * 16
    with open(pkg, "wb") as f:
        f.write(blob)

    cfg = {"dir": tmp, "base_url": "", "quiet": True}

    # HTTP
    srv = updatesrv.serve_http(0, cfg)
    port = srv.server_address[1]
    cfg["base_url"] = "http://127.0.0.1:%d" % port
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    table = json.loads(fetch(cfg["base_url"] + "/v1/updates"))
    ck("http table lists package", len(table["updates"]) == 1)
    e = table["updates"][0]
    ck("http table sha256", e["sha256"] == hashlib.sha256(blob).hexdigest())
    got = fetch(e["url"])
    ck("http package bytes", got == blob)
    srv.shutdown()

    # HTTPS (self-signed)
    cert = os.path.join(tmp, "cert.pem")
    key = os.path.join(tmp, "key.pem")
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-keyout", key,
         "-out", cert, "-days", "2", "-nodes", "-subj", "/CN=localhost",
         "-addext", "subjectAltName=DNS:localhost"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    cfg2 = {"dir": tmp, "base_url": "", "quiet": True}
    srv2 = updatesrv.serve_https(0, cfg2, cert, key)
    port2 = srv2.server_address[1]
    cfg2["base_url"] = "https://127.0.0.1:%d" % port2
    threading.Thread(target=srv2.serve_forever, daemon=True).start()
    table2 = json.loads(fetch(cfg2["base_url"] + "/v1/updates", insecure=True))
    ck("https table lists package", len(table2["updates"]) == 1)
    got2 = fetch(table2["updates"][0]["url"], insecure=True)
    ck("https package bytes", got2 == blob)
    srv2.shutdown()

    print("\n%s (%d failures)" % ("FAILED" if fails else "ALL PASS", fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
