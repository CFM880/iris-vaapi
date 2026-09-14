#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Summarize a seek-harness log (see seq_server.py / seek_test.html).

Reports, per run, how many post-seek frames hash to content already seen before
a SEEK marker (stale repeats, content-dependent), and how many POST-seek frames
carry a driver frame-stamp serial (`sn=`, VPU_FRAME_STAMP=1) that was already
displayed before the SEEK marker.  The serial is unique per decoded frame, so a
repeated serial is an unambiguous stale-frame display.
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
        m = re.search(r"ct=([\d.]+) h=([0-9a-f]+)(?: sn=(-?\d+))? tf=(\d+) df=(\d+)", line)
        if m:
            segments[-1].append((float(m.group(1)), m.group(2),
                                 int(m.group(3)) if m.group(3) is not None else -1,
                                 int(m.group(4)), int(m.group(5))))

seen = set()
stale = total = 0
seen_sn = set()
stale_sn = total_sn = 0
for si, seg in enumerate(segments):
    if si > 0 and seen:
        for _, h, sn, _, _ in seg[5:]:   # skip settle samples
            total += 1
            if h in seen:
                stale += 1
            if sn >= 0:
                total_sn += 1
                if sn in seen_sn:
                    stale_sn += 1
    for _, h, sn, _, _ in seg:
        seen.add(h)
        if sn >= 0:
            seen_sn.add(sn)

dfs = [s[4] for seg in segments for s in seg]
print("seeks=%d samples=%d stale_repeats=%d (%.1f%%) stale_serial=%d/%d (%.1f%%) "
      "df: start=%s end=%s max=%s"
      % (seeks, sum(len(s) for s in segments), stale,
         100.0 * stale / max(1, total),
         stale_sn, total_sn,
         100.0 * stale_sn / max(1, total_sn),
         dfs[0] if dfs else None, dfs[-1] if dfs else None,
         max(dfs) if dfs else None))

