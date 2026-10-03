#!/usr/bin/env python3
"""A fake MeshMonitor for trying the saver without a radio.

    python3 tools/mock_meshmonitor.py [port]   (default 8787, token "mm_v1_test")

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
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8787
    print("mock MeshMonitor on http://127.0.0.1:%d  token %s" % (port, TOKEN))
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
