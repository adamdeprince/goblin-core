# Indexed Compact Hash Threshold Sweep

Goblin Core's compact hash now stores one pooled blob with a byte fingerprint
and a 16-bit entry offset per field. Lookups scan fingerprints with the build
ISA's widest byte vector, then compare the complete field only for fingerprint
matches. The full hash uses the Swiss field index and arena.

The initial sweep supported increasing the old 32-field default through 512. A
second AVX2 sweep through 2048 supports removing the default count ceiling:
**the 64 KiB compact-blob limit now decides promotion**. Operators can still
force an earlier count boundary with `--hash-listpack-max-entries`; zero forces
every hash to the full form and `blob` restores the size-only default.

## Method

- Host: a quiet dedicated 64-core Threadripper PRO 5995WX system.
- Every cardinality and engine starts a fresh server; servers run one at a time.
- Each row holds approximately 500,000 total fields. Keys and fields are 14
  bytes each and values are 16 bytes.
- A Python RESP pipeline issues one multi-field `HSET` per hash. Point tests use
  one `redis-benchmark` client and pipeline depth 256.
- RSS is the patched, live `/proc` value exposed as `INFO memory
  used_memory_rss`, measured as a delta over the warmed empty server.
- The compact/full decision uses 1,000,000 point operations, median of five
  runs. The incumbent matrix uses 200,000 operations, median of three runs.
- The extended 512-2048 pass uses the compile-time AVX2 fingerprint scanner.
- Redis and Valkey use `benchmarks/redis-parity.conf`. Dragonfly uses one
  proactor for parity. Incumbents are black-box RESP servers; their source was
  not inspected.

The exact driver is
[`benchmarks/hash_threshold_sweep.py`](benchmarks/hash_threshold_sweep.py).

## Compact Versus Full

RSS is bytes per hash. HSET is a same-width update to the middle field.

| fields/hash | compact RSS | full RSS | RSS saved | compact HSET | full HSET |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 32 | 1,654.9 | 2,128.1 | 22.2% | 2.137M | 1.996M |
| 40 | 1,662.6 | 2,625.7 | 36.7% | 2.084M | 1.829M |
| 50 | 2,232.3 | 3,154.3 | 29.2% | 2.146M | 1.946M |
| 64 | 3,329.4 | 3,566.4 | 6.6% | 2.198M | 2.050M |
| 80 | 3,343.0 | 4,635.4 | 27.9% | 2.223M | 1.935M |
| 101 | 4,369.1 | 5,506.8 | 20.7% | 2.088M | 1.942M |
| 128 | 4,990.5 | 6,566.6 | 24.0% | 2.088M | 1.912M |
| 161 | 6,227.8 | 11,108.7 | 43.9% | 2.142M | 2.004M |
| 203 | 7,784.6 | 11,704.3 | 33.5% | 2.128M | 2.088M |
| 256 | 9,796.4 | 12,405.4 | 21.0% | 2.193M | 2.115M |
| 322 | 12,277.4 | 17,379.0 | 29.4% | 2.142M | 2.008M |
| 406 | 15,405.8 | 18,520.2 | 16.8% | 2.321M | 2.075M |
| 512 | 19,447.6 | 24,013.6 | 19.0% | 2.033M | 2.008M |

Compact uses less RSS at all 13 points. The non-monotonic savings are allocator
size classes: 64 fields is an unfavorable edge, while 80 fields consumes almost
no more RSS. Same-width HSET is 1.2-14.9% faster in compact form.

## Lookup Shape

Each cell is compact throughput and its difference from forced-full Goblin.
`load` is end-to-end Python construction throughput, not a server-only rate.

| fields/hash | HGET first | HGET middle | HGET last | HGET miss | load fields/s |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 32 | 2.342M (+7.3%) | 2.370M (+11.4%) | 2.321M (+9.0%) | 2.416M (+3.4%) | 0.783M (+3.5%) |
| 40 | 2.416M (+22.2%) | 2.233M (+10.5%) | 2.193M (+8.1%) | 2.410M (+5.3%) | 0.764M (-1.2%) |
| 50 | 2.445M (+13.2%) | 2.326M (+6.0%) | 2.337M (+7.5%) | 2.393M (-1.4%) | 0.806M (+2.4%) |
| 64 | 2.268M (+1.1%) | 2.321M (+5.3%) | 2.193M (-0.9%) | 2.404M (-0.5%) | 0.794M (+0.2%) |
| 80 | 2.381M (+15.0%) | 2.434M (+17.8%) | 2.233M (+7.6%) | 2.445M (+3.4%) | 0.811M (-2.5%) |
| 101 | 2.381M (+11.0%) | 2.263M (+9.0%) | 2.365M (+13.9%) | 2.482M (-0.2%) | 0.830M (+0.1%) |
| 128 | 2.353M (+14.1%) | 2.284M (+11.2%) | 2.278M (+12.1%) | 2.507M (+5.5%) | 0.825M (+0.5%) |
| 161 | 2.289M (+10.1%) | 2.310M (+1.4%) | 2.299M (+12.0%) | 2.451M (-4.9%) | 0.822M (+0.6%) |
| 203 | 2.365M (+5.2%) | 2.464M (+10.6%) | 2.258M (+3.2%) | 2.439M (-9.3%) | 0.823M (+0.3%) |
| 256 | 2.532M (+11.9%) | 2.321M (+3.0%) | 2.238M (+3.6%) | 2.370M (-7.6%) | 0.814M (-1.9%) |
| 322 | 2.359M (+13.2%) | 2.233M (+7.1%) | 2.146M (-3.6%) | 2.326M (-7.4%) | 0.793M (-3.1%) |
| 406 | 2.387M (+11.5%) | 2.399M (+7.2%) | 2.248M (+11.2%) | 2.299M (+0.9%) | 0.792M (-3.7%) |
| 512 | 2.445M (+13.9%) | 2.278M (+6.6%) | 2.184M (+5.2%) | 2.337M (+0.2%) | 0.775M (-6.1%) |

Hits do not show a reason to promote before 512. Misses are the weaker shape:
they trail full Goblin by as much as 9.3% because every fingerprint group must
be checked. Construction also trails by 6.1% at 512. A miss-heavy workload or
one dominated by different-width updates and deletes can choose an earlier
threshold; those mutations rebuild the compact blob.

## Extended AVX2 Sweep

The second sweep tests `512, 645, 812, 1024, 1290, 1625, 2048`. With these
14-byte fields and 16-byte values, each field costs 37 compact bytes including
its directory entry. Exactly 1771 fields produce a 65,535-byte blob; 1772 must
promote. Thus 1625 remains compact while the threshold-selected 2048 row is
intentionally full.

| fields/hash | actual form | compact/selected RSS | forced-full RSS | RSS saved | HSET middle | HGET middle |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 512 | compact | 19,447.6 | 24,013.6 | 19.0% | 2.101M (+3.4%) | 2.284M (+5.9%) |
| 645 | compact | 24,491.4 | 30,484.8 | 19.7% | 2.248M (+11.7%) | 2.416M (+13.3%) |
| 812 | compact | 30,809.9 | 36,837.4 | 16.4% | 2.062M (-0.6%) | 2.128M (-2.3%) |
| 1024 | compact | 38,870.0 | 47,809.0 | 18.7% | 2.110M (-2.1%) | 2.208M (-3.8%) |
| 1290 | compact | 49,003.8 | 60,720.3 | 19.3% | 2.156M (+1.3%) | 2.238M (+0.0%) |
| 1625 | compact | 62,053.7 | 73,647.9 | 15.7% | 2.137M (-1.7%) | 2.213M (-0.7%) |
| 2048 | full | 92,344.7 | 91,740.3 | -0.7% | 2.179M (-1.3%) | 2.263M (-3.8%) |

The 2048 differences are fresh-server noise between two runs of the same full
representation. Before promotion, compact saves 16-20% RSS throughout. HSET and
middle-position HGET remain close to or faster than full, but position and miss
shape expose the cost of scanning a larger directory:

| fields/hash | HGET first | HGET middle | HGET last | HGET miss | construction |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 512 | +7.9% | +5.9% | +0.4% | +5.3% | -5.7% |
| 645 | +10.5% | +13.3% | -3.7% | -5.6% | -5.3% |
| 812 | +11.1% | -2.3% | -9.4% | -15.2% | -7.9% |
| 1024 | +11.7% | -3.8% | +2.6% | -13.1% | -12.7% |
| 1290 | +10.2% | +0.0% | -11.1% | -19.9% | -13.2% |
| 1625 | +4.8% | -0.7% | -15.9% | -23.1% | -18.2% |

The default follows Goblin's memory-first policy and lets blob size decide. A
miss-heavy workload, one dominated by late-position fields, or one that inserts
mostly single-field commands can set a finite count ceiling. The count option is
a workload control, not a structural requirement.

## Incumbent Memory

RSS bytes per hash from the same six-engine pass:

| fields/hash | Goblin compact | Goblin full | Redis 7.2 | Redis 8.8 | Valkey 9.1 | Dragonfly |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 32 | 1,655 | 2,128 | 1,359 | 1,353 | 1,363 | 1,361 |
| 40 | 1,663 | 2,626 | 1,628 | 1,620 | 1,622 | 3,234 |
| 50 | 2,232 | 3,154 | 1,880 | 1,873 | 1,880 | 3,912 |
| 64 | 3,329 | 3,566 | 2,655 | 2,653 | 2,669 | 4,965 |
| 80 | 3,343 | 4,635 | 3,189 | 3,186 | 3,195 | 6,315 |
| 101 | 4,369 | 5,507 | 3,696 | 3,690 | 3,710 | 7,806 |
| 128 | 4,990 | 6,584 | 5,253 | 5,253 | 5,290 | 9,746 |
| 161 | 6,228 | 11,109 | 12,742 | 9,361 | 9,185 | 12,701 |
| 203 | 7,785 | 11,704 | 15,484 | 11,475 | 11,086 | 15,642 |
| 256 | 9,796 | 12,405 | 18,938 | 14,201 | 15,300 | 19,498 |
| 322 | 12,277 | 17,379 | 25,368 | 18,867 | 17,968 | 25,505 |
| 406 | 15,406 | 18,520 | 30,858 | 23,035 | 22,503 | 31,387 |
| 512 | 19,448 | 24,014 | 37,791 | 28,219 | 29,784 | 39,000 |

Redis and Valkey are smaller below 128 fields. Their configured compact form
ends at 128; Goblin becomes the smallest at 128 and widens the lead after they
promote. At 512 fields Goblin compact uses 31% less RSS than Redis 8.8, 35% less
than Valkey 9.1, and about half the RSS of Redis 7.2 and Dragonfly.

The extended matrix keeps the same direction. At 1625 fields the compact form
uses 62,054 RSS bytes/hash versus 92,353 for Redis 8.8, 91,153 for Valkey,
124,094 for Redis 7.2, and 125,975 for Dragonfly.
