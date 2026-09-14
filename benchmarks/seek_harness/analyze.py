#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarize a seek-harness log (see seq_server.py / seek_test.html).

Reports, per run, how many post-seek frames hash to content already seen before
a SEEK marker (stale repeats), the number of seeks/samples, and dropped-frame
counters.  A baseline run without seeks already repeats ~29% on the sample
content, so compare relatively.
"""

import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/opencode/seq.log"

segments = [[]]
seeks = 0
with open(path) as fh:
    for line in fh:
        if line.startswith("SEEK"):
            seeks += 1
            segments.append([])
            continue
        m = re.search(r"ct=([\d.]+) h=([0-9a-f]+) tf=(\d+) df=(\d+)", line)
        if m:
            segments[-1].append((float(m.group(1)), m.group(2),
                                 int(m.group(3)), int(m.group(4))))

seen = set()
stale = total = 0
for si, seg in enumerate(segments):
    if si > 0 and seen:
        for _, h, _, _ in seg[5:]:   # skip settle samples
            total += 1
            if h in seen:
                stale += 1
    for _, h, _, _ in seg:
        seen.add(h)

dfs = [s[3] for seg in segments for s in seg]
print("seeks=%d samples=%d stale_repeats=%d (%.1f%%) df: start=%s end=%s max=%s"
      % (seeks, sum(len(s) for s in segments), stale,
         100.0 * stale / max(1, total),
         dfs[0] if dfs else None, dfs[-1] if dfs else None,
         max(dfs) if dfs else None))
