#!/usr/bin/env python3
"""Novus OS unified update table server (reference implementation).

Serves:
  GET /v1/updates   -> JSON table of packages in --dir
  GET /pkg/<name>   -> the package bytes
  GET /healthz      -> "ok"

HTTP and (optionally) HTTPS via Python's stdlib ssl. Packages are expected to
be signed .novu/.npth/.apth/.hotpth files produced by tools/novupkg; this
server only publishes them and their SHA-256.
"""
import argparse
import hashlib
import json
import os
import ssl
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

VERSION = 1
SUFFIXES = {".novu": 0, ".npth": 1, ".apth": 2, ".hotpth": 3}


def ext_type(name):
    for suf, ty in SUFFIXES.items():
        if name.endswith(suf):
            return ty
    return None


def build_table(pkg_dir, base_url):
    entries = []
    for name in sorted(os.listdir(pkg_dir)):
        path = os.path.join(pkg_dir, name)
        if not os.path.isfile(path):
            continue
        ty = ext_type(name)
        if ty is None:
            continue
        with open(path, "rb") as f:
            data = f.read()
        entries.append({
            "uuid": hashlib.sha256(name.encode("utf-8")).hexdigest()[:16],
            "name": name,
            "type": ty,
            "level": 1,
            "size": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "url": base_url.rstrip("/") + "/pkg/" + name,
            "published_at": int(os.path.getmtime(path)),
        })
    return {
        "version": VERSION,
        "generated_at": int(time.time()),
        "updates": entries,
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "NovusUpdateSrv/1.0"

    def _send(self, code, body, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        cfg = self.server.cfg
        if self.path == "/healthz":
            self._send(200, b"ok", "text/plain")
            return
        if self.path.split("?")[0] == "/v1/updates":
            table = build_table(cfg["dir"], cfg["base_url"])
            body = json.dumps(table, indent=2).encode("utf-8")
            self._send(200, body)
            return
        if self.path.startswith("/pkg/"):
            name = os.path.basename(self.path[len("/pkg/"):])
            if name in ("", ".", "..") or ext_type(name) is None:
                self._send(404, b'{"error":"not found"}')
                return
            path = os.path.join(cfg["dir"], name)
            if not os.path.isfile(path):
                self._send(404, b'{"error":"not found"}')
                return
            with open(path, "rb") as f:
                data = f.read()
            self._send(200, data, "application/octet-stream")
            return
        self._send(404, b'{"error":"not found"}')

    def log_message(self, fmt, *args):
        if self.server.cfg.get("quiet"):
            return
        sys.stderr.write("[updatesrv] " + (fmt % args) + "\n")


def serve_http(port, cfg):
    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    srv.cfg = cfg
    return srv


def serve_https(port, cfg, cert, key):
    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    srv.cfg = cfg
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(certfile=cert, keyfile=key)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    return srv


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="/tmp/updsrv/pkgs")
    ap.add_argument("--http", type=int, default=8080)
    ap.add_argument("--https", type=int, default=0)
    ap.add_argument("--cert", default="")
    ap.add_argument("--key", default="")
    ap.add_argument("--base-url", default="")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--table-only", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.dir, exist_ok=True)
    cfg = {"dir": args.dir, "base_url": args.base_url, "quiet": args.quiet}
    if not cfg["base_url"]:
        scheme = "https" if args.https else "http"
        port = args.https if args.https else args.http
        cfg["base_url"] = "%s://127.0.0.1:%d" % (scheme, port)

    if args.table_only:
        print(json.dumps(build_table(args.dir, cfg["base_url"]), indent=2))
        return 0

    servers = []
    if args.http:
        servers.append((serve_http(args.http, cfg), "http", args.http))
    if args.https:
        if not args.cert or not args.key:
            sys.stderr.write("--https requires --cert and --key\n")
            return 1
        servers.append((serve_https(args.https, cfg, args.cert, args.key),
                        "https", args.https))
    if not servers:
        sys.stderr.write("no listener requested\n")
        return 1

    import threading
    for srv, scheme, port in servers:
        t = threading.Thread(target=srv.serve_forever, daemon=True)
        t.start()
        sys.stderr.write("[updatesrv] %s on :%d, dir=%s\n"
                         % (scheme, port, args.dir))
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
