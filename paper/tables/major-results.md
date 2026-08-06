# Major results (representative point: 8GiB host / 2TiB device / all-Q8)

Regenerated from `benchmarks/cxl-sim/unified-sweep.csv` by
`paper/scripts/generate-tables.py` -- do not hand-edit.

| Model | Comparison | Mean bytes/token | Reduction vs. full-scan-CXL | Hit rate | Precision/Recall |
|---|---|---|---|---|---|
| SmolLM2-135M | exact-no-prefetch | 4,722,553.4 | 321.1x | 0.0738 | 0.000 / 0.000 |
| SmolLM2-135M | exact-predictor | 4,722,553.4 | 321.1x | 0.0738 | 0.553 / 0.829 |
| SmolLM2-135M | exact-predictor-prefetch | 8,100,156.8 | 187.2x | 0.8310 | 0.553 / 0.829 |
| SmolLM2-135M | exact-predictor-coalescing | 8,100,156.8 | 187.2x | 0.8310 | 0.553 / 0.829 |
| SmolLM2-135M | oracle | 4,722,848.7 | 321.1x | 1.0000 | 1.000 / 1.000 |
| SmolLM2-360M | exact-no-prefetch | 6,660,908.4 | 404.7x | 0.0899 | 0.000 / 0.000 |
| SmolLM2-360M | exact-predictor | 6,660,908.4 | 404.7x | 0.0899 | 0.601 / 0.903 |
| SmolLM2-360M | exact-predictor-prefetch | 11,008,425.8 | 244.9x | 0.9035 | 0.601 / 0.903 |
| SmolLM2-360M | exact-predictor-coalescing | 11,008,425.8 | 244.9x | 0.9035 | 0.601 / 0.903 |
| SmolLM2-360M | oracle | 6,661,084.7 | 404.7x | 1.0000 | 1.000 / 1.000 |
