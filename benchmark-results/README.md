# Decode benchmark results

Result filenames describe the comparison and intentionally do not contain a
date. Future runs should update an existing comparison file or add a new,
descriptive CSV file. Run timestamps remain inside the CSV/report metadata.

Tracked files:

- `h264-hevc8-v4l2-vaapi.csv`: H.264 High and HEVC Main 8-bit raw runs.
- `vp9-profile0-v4l2-vaapi.csv`: VP9 Profile 0 raw runs.
- `p010-v4l2-vaapi.csv`: HEVC Main10 and VP9 Profile 2 P010 raw runs.
- `v4l2-vaapi-summary.sql`: reproducible aggregation across the raw runs.
- `v4l2-vaapi-report.json`: canonical report input.
- `v4l2-vaapi-report.html`: generated portable report.

Per-run logs and generated elementary streams live in the ignored `logs/` and
`streams/` directories so repeated comparisons do not pollute Git history.

Example output routing:

```sh
python3 benchmarks/compare_decode_paths.py \
  --h264 /path/to/h264.mp4 \
  --hevc /path/to/hevc.mp4 \
  --build-dir build \
  --output-dir benchmark-results/logs/h264-hevc8 \
  --results-file benchmark-results/h264-hevc8-v4l2-vaapi.csv

python3 benchmarks/compare_decode_paths.py \
  --vp9 /path/to/vp9-profile0.webm \
  --build-dir build \
  --output-dir benchmark-results/logs/vp9-profile0 \
  --results-file benchmark-results/vp9-profile0-v4l2-vaapi.csv \
  --repetitions 5

python3 benchmarks/compare_10bit_paths.py \
  --hevc benchmark-results/streams/hevc-main10-4k60.h265 \
  --vp9 benchmark-results/streams/vp9-profile2-4k60-1200.ivf \
  --build-dir build \
  --output-dir benchmark-results/logs/p010 \
  --results-file benchmark-results/p010-v4l2-vaapi.csv
```
