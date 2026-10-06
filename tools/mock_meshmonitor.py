#!/usr/bin/env python3
"""A fake MeshMonitor for trying the saver without a radio.

    python3 tools/mock_meshmonitor.py [port] [--chunked] [--backlog N]   (default 8787, token "mm_v1_test")

--backlog N starts with N old messages (the saver must not replay them).
Answers allow any origin (CORS), like a MeshMonitor with this page in ALLOWED_ORIGINS.

Serves /api/v1/sources/<id>/nodes and /messages in MeshMonitor's v1 shape
and invents a new message every couple of seconds (a third are broadcasts).
"""
import json
import random
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

TOKEN = "mm_v1_test"
CHUNKED = "--chunked" in sys.argv  # answer like a reverse proxy that streams responses
BACKLOG = int(sys.argv[sys.argv.index("--backlog") + 1]) if "--backlog" in sys.argv else 0
NODES = ["!%08x" % random.getrandbits(32) for _ in range(40)]
MESSAGES = []
next_id = 1
last_made = 0.0


def make_messages():
    global next_id, last_made
    now = time.time()
    while now - last_made > 2:
        last_made = now if last_made == 0 else last_made + 2
        sender = random.choice(NODES)
        to = "!ffffffff" if random.random() < 0.33 else random.choice([n for n in NODES if n != sender])
        MESSAGES.insert(0, {
            "id": "%d_%d" % (next_id, int(now)),
            "fromNodeId": sender,
            "toNodeId": to,
            "channel": 0 if to == "!ffffffff" else -1,
            "portnum": 1,
            "text": "test %d" % next_id,
            "timestamp": int(now * 1000),
            "createdAt": int(now * 1000),
        })
        next_id += 1
    del MESSAGES[200:]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_OPTIONS(self):
        self.send_response(204)
        self.cors()
        self.send_header("Access-Control-Allow-Headers", "Authorization, Accept")
        self.send_header("Access-Control-Allow-Methods", "GET")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def cors(self):
        self.send_header("Access-Control-Allow-Origin", self.headers.get("Origin") or "*")
        self.send_header("Vary", "Origin")

    def do_GET(self):
        if self.headers.get("Authorization") != "Bearer " + TOKEN:
            return self.send(401, {"success": False, "error": "Unauthorized"})
        parts = urlparse(self.path).path.strip("/").split("/")
        if parts[:3] != ["api", "v1", "sources"] or len(parts) != 5:
            return self.send(404, {"success": False, "error": "Not found"})
        make_messages()
        if parts[4] == "nodes":
            now = int(time.time())
            data = [{"nodeId": n, "shortName": n[-4:], "lastHeard": now - i * 60} for i, n in enumerate(NODES)]
        elif parts[4] == "messages":
            data = MESSAGES[:50]
        else:
            return self.send(404, {"success": False, "error": "Not found"})
        self.send(200, {"success": True, "count": len(data), "data": data})

    def send(self, status, body):
        raw = json.dumps(body).encode()
        self.send_response(status)
        self.cors()
        self.send_header("Content-Type", "application/json")
        if CHUNKED:
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(raw), 700):
                piece = raw[i:i + 700]
                self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    args = [a for i, a in enumerate(sys.argv[1:], 1) if not a.startswith("--") and sys.argv[i - 1] != "--backlog"]
    for i in range(BACKLOG):
        sender = NODES[i % len(NODES)]
        MESSAGES.insert(0, {"id": "old_%d" % i, "fromNodeId": sender, "toNodeId": "!ffffffff", "channel": 0,
                            "portnum": 1, "text": "old %d" % i, "timestamp": int((time.time() - 600 + i) * 1000),
                            "createdAt": int((time.time() - 600 + i) * 1000)})
    port = int(args[0]) if args else 8787
    print("mock MeshMonitor on http://127.0.0.1:%d  token %s" % (port, TOKEN))
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
