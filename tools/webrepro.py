#!/usr/bin/env python3
"""Measure Web CLI command->output latency the way the browser does.

Drains any backlog first so the measurement is not inflated by an artificial
backlog, then POSTs a command and polls once per second (browser cadence).
"""
import json, time, urllib.request, sys

BASE = "http://esp-ot-br.local"
CMD = sys.argv[1] if len(sys.argv) > 1 else "state"


def get(after, timeout=30):
    t0 = time.time()
    with urllib.request.urlopen(BASE + "/cli?after=%d" % after, timeout=timeout) as r:
        body = r.read()
    return json.loads(body), time.time() - t0, len(body)


def post(command, timeout=30):
    data = json.dumps({"command": command}).encode()
    req = urllib.request.Request(BASE + "/cli", data=data,
                                 headers={"Content-Type": "application/json"})
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=timeout) as r:
        body = r.read()
    return r.status, body.decode(), time.time() - t0


# --- catch up fully so we start from a clean head ---
d, dt, size = get(0)
cursor = d["cursor"]
guard = 0
while d.get("more") and guard < 50:
    d, dt, size = get(cursor)
    cursor = d["cursor"]
    guard += 1
print("caught up: cursor=%d after %d GETs, session=%d ready=%s"
      % (cursor, guard + 1, d["session"], d["ready"]))

t_post = time.time()
status, body, dt_post = post(CMD)
print("POST %-8r -> HTTP %d %s in %.3fs" % (CMD, status, body, dt_post))

seen_echo = seen_out = None
n = 0
while time.time() < t_post + 30 and seen_out is None:
    data, dt, size = get(cursor)
    n += 1
    now = time.time()
    for rec in data.get("records", []):
        if rec["source"] == "input" and seen_echo is None:
            seen_echo = now - t_post
        elif rec["source"] != "input" and seen_echo is not None and seen_out is None:
            seen_out = now - t_post
            print("  OUTPUT %r after %.3fs (poll#%d, GET %.3fs, %dB, more=%s)"
                  % (rec["text"].strip()[:40], seen_out, n, dt, size, data.get("more")))
    if data.get("records"):
        print("  poll#%d t+%.3fs +%d rec GET %.3fs %dB more=%s dropped=%s"
              % (n, now - t_post, len(data["records"]), dt, size,
                 data.get("more"), data.get("dropped")))
    cursor = data["cursor"]
    if seen_out is None:
        time.sleep(1.0)

print("==> cmd=%r  echo=%.3fs  output=%.3fs  polls=%d  (POST %.3fs)"
      % (CMD, seen_echo or -1, seen_out or -1, n, dt_post))
