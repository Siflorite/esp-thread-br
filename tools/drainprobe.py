#!/usr/bin/env python3
"""Measure the BR Web CLI ring-buffer production rate vs the 16-records-per-poll drain rate."""
import json, time, urllib.request

BASE = "http://esp-ot-br.local"


def get(after, timeout=30):
    with urllib.request.urlopen(BASE + "/cli?after=%d" % after, timeout=timeout) as r:
        return json.loads(r.read())


d = get(0)
cursor = d["cursor"]
print("start cursor=%d" % cursor)

t0 = time.time()
total = 0
calls = 0
drops = 0
overwrites = 0
while time.time() - t0 < 12:
    d = get(cursor)
    cur_new = d["cursor"]
    n = len(d["records"])
    total += n
    calls += 1
    if d.get("more"):
        overwrites += 1
    if d.get("dropped"):
        drops += 1
    cursor = cur_new
    if n == 0:
        time.sleep(0.05)

el = time.time() - t0
print("drained %d records in %.2fs via %d GETs  => %.1f rec/s drained, %.1f GET/s"
      % (total, el, calls, total / el, calls / el))
print("polls that still had more=True: %d/%d   dropped-flag polls: %d"
      % (overwrites, calls, drops))
print("=> device produces MORE than %.1f records/s" % (total / el))
