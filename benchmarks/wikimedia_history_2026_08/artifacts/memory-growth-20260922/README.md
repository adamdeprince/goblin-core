# Wikimedia memory growth comparison data

This derived dataset supports the
[memory growth plot](../../rle-full-comparison-20260922.md#memory-as-entries-are-added).
It combines observations from two verified replays on `naamah` of the same
1,483,700,913-command numeric trace. All endpoints contain 80,798,328 members
and the same page-ID-to-score mapping.

- [samples.tsv](samples.tsv): every recorded sample for the seven plotted
  curves, followed by each curve's separately recorded final measurement.
  `live_members` is `ZCARD`; `os_rss_kib` is process RSS in KiB. Chart values
  divide these by 1,000,000 and 1,048,576 respectively. `measurement=sample`
  uses the sample's elapsed seconds; `measurement=final` uses the timed feed
  duration from `summary.tsv`. The plot uses member count as its horizontal axis.
- [endpoints.tsv](endpoints.tsv): final RSS and savings relative to the new
  INT32/FLOAT32 RLE result. This also includes the September 22 RLE-off control,
  which is tabulated in the report but not drawn over the nearly identical
  historical packed curve.
- [sources.json](sources.json): exact source paths and SHA-256 hashes, the
  common numeric input hash, series identities, and axis definitions.

The earlier packed and standard Goblin curves and all four incumbents come
from the [September 8 six-engine replay](../full-20260908T190700Z/README.md).
The RLE curve comes from the
[September 22 twelve-variant replay](../rle-full-20260922T035401Z/README.md).
These are separate concurrent runs, with different Goblin builds and numbers
of clients. The plot compares observed memory growth; it does not compare
throughput between runs. Packed curves use INT32/FLOAT32; standard Goblin and
incumbents retain their general member representations.

No original observation is smoothed, removed, or replaced. Some incumbent
final samples differ by 12 KiB from the separate final reading in `summary.tsv`;
both records are retained. Five-minute samples cannot establish peak RSS.
Labels and report savings use the final summary readings.

Regenerate the data and standalone SVG from the repository root:

```sh
python3 benchmarks/wikimedia_history_2026_08/plot_rle_comparison.py
```

The generator requires Gnuplot and verifies the recorded statuses, final
mapping digests, common input hash, and monotonic member counts before plotting.
The two original evidence bundles remain unchanged.
