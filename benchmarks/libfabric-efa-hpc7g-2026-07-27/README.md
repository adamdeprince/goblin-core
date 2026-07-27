# Native AWS EFA benchmark artifacts

This directory preserves Goblin Core's first native AWS EFA provider matrix.
Two `hpc7g.4xlarge` instances ran in one `us-east-1d` cluster placement group.
Each host had 16 physical Arm cores, one NUMA node, 32 MiB of shared L3, and an
active EFA2 device. The server and client were pinned to CPU 8 and memory node
0.

The Release build is commit `96bcececd06a0983bb94ee12ec010dec9a9beb34`
with AWS libfabric `2.4.0amzn5.0`. `metadata.txt` records the executable hashes.

## Method

- Every connection was established once and reused. Connection setup is not
  timed.
- Every command used pipeline depth 1, 20,000 warmups, and 200,000 measured
  round trips.
- Each libfabric provider/wire/send combination ran against a fresh server.
- Native `efa` and software `tcp` used `FI_EP_RDM`, `FI_MSG | FI_SOURCE`,
  `fi_recv`, receive completion queues, and either automatic injection or
  forced `fi_send` with TX completion reclamation.
- Kernel TCP retained the hosts' default ENA adaptive RX moderation.
- `verbs;ofi_rxm` was built but returned `fi_getinfo: -61 (No data available)`
  on EFA hardware, so no row is fabricated for it.

## Artifact map

| File | Contents |
|---|---|
| `comparison-latency.csv` | Canonical 80-distribution comparison used by the document |
| `aggregate.csv` | Eight-operation averages ranked by median |
| `latency.csv` | Original one-pass matrix, including its first kernel controls |
| `pre-placement-smoke-latency.csv` | 1,000-sample EFA matrix before cluster placement |
| `config.csv` | Transport, wire, endpoint, buffer, and sample metadata |
| `efa-*.out`, `tcp-*.out` | Per-provider raw distributions |
| `kernel-tcp-*.out` | Kernel controls from the first one-pass matrix |
| `goblin-kernel-tcp-*-rerun.out` | Dedicated 200,000-sample kernel validation repeats |
| `*.server.log` | Goblin startup and provider-selection evidence |
| `fi-pingpong.out` | Native EFA 64-byte fabric sanity check |
| `metadata.txt` | Build identity and sanitized host metadata |
| `SHA256SUMS` | SHA-256 digest for every other artifact |

The initial matrix showed state-dependent latency steps in the interrupt-driven
kernel controls. A dedicated repeat moved the affected operations, while the
native EFA and libfabric `tcp` results remained stable. The canonical
`comparison-latency.csv` therefore uses the dedicated kernel controls and
retains the initial controls separately rather than deleting them.

The benchmark automation is
[`benchmarks/libfabric_provider_matrix.sh`](https://github.com/adamdeprince/goblin-core/blob/main/benchmarks/libfabric_provider_matrix.sh).
