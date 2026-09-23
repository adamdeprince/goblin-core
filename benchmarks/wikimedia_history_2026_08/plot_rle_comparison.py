#!/usr/bin/env python3
"""Render the verified full-run RLE measurements as standalone SVG charts.

Requires gnuplot. Pass --preview-dir to also render PNGs for local inspection.
The archived measurements and frozen benchmark harness are never modified.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess


HERE = Path(__file__).resolve().parent
LAYOUTS = [(member, score) for member in ("int32", "int64", "uuid")
           for score in ("float32", "float64")]
EARLIER_RUN = "full-20260908T190700Z"
RLE_RUN = "rle-full-20260922T035401Z"
RLE_ENGINE = "goblin-packed-int32-float32-rle-on"
GROWTH_SERIES = (
    (EARLIER_RUN, "redis-7.2.4", "Redis 7.2.4", "#ae4167", 2, 1, None),
    (EARLIER_RUN, "redis-8.8", "Redis 8.8", "#c76323", 2, 1, 5.62),
    (EARLIER_RUN, "valkey-9.1", "Valkey 9.1", "#9a7300", 2, 1, 5.07),
    (EARLIER_RUN, "dragonfly", "Dragonfly", "#7652a2", 2, 1, None),
    (EARLIER_RUN, "goblin-standard", "Goblin standard", "#3a566d", 2, 2, None),
    (EARLIER_RUN, "goblin-packed-int32-float32", "Goblin packed (earlier)",
     "#008c9b", 2.5, 2, None),
    (RLE_RUN, RLE_ENGINE, "Goblin packed + RLE", "#548a08", 3.5, 1, None),
)


def quoted(value: str) -> str:
    return json.dumps(value)


def render(rows: dict[str, dict[str, str]], *, field: str, divisor: float,
           title: str, ylabel: str, limit: float, output: Path,
           preview: Path | None) -> None:
    data = []
    ticks = []
    for index, (member, score) in enumerate(LAYOUTS):
        ticks.append(f'{quoted(member.upper() + chr(10) + score.upper())} {index}')
        off, on = [float(rows[f"goblin-packed-{member}-{score}-rle-{mode}"][field])
                   / divisor for mode in ("off", "on")]
        data.append(f"{index} {off:.9f} {on:.9f}")
    plot = """
plot $measurements using ($1-0.18):2 with boxes lc rgb '#788681' title 'RLE off', \\
     '' using ($1+0.18):3 with boxes lc rgb '#548a08' title 'RLE on (current default)', \\
     '' using ($1-0.18):2:(sprintf('%.2f', $2)) with labels offset 0,0.7 font ',11' notitle, \\
     '' using ($1+0.18):3:(sprintf('%.2f', $3)) with labels offset 0,0.7 font ',11' notitle
"""
    commands = f"""
set encoding utf8
set terminal svg size 1040,580 dynamic enhanced font 'Arial,14' background rgb '#fffefa'
set output {quoted(str(output))}
set title {quoted(title)} font ',21'
set ylabel {quoted(ylabel)} offset -1,0
set xrange [-0.65:5.65]
set yrange [0:{limit}]
set xtics ({', '.join(ticks)}) nomirror textcolor rgb '#45483f'
set ytics nomirror textcolor rgb '#45483f'
set border 3 lc rgb '#b3b8ae'
set grid ytics lc rgb '#e4e7df' back
set key top left horizontal reverse Left samplen 1.5
set style fill solid 1.0 noborder
set boxwidth 0.32 absolute
set lmargin at screen 0.10
set rmargin at screen 0.97
set tmargin at screen 0.82
set bmargin at screen 0.22
set label 1 '1.483 billion increments per variant / 80.8 million final members / 12 concurrent runs' at screen 0.10,0.025 font ',11' tc rgb '#62645c'
$measurements << EOD
{chr(10).join(data)}
EOD
{plot}
"""
    finish_plot(commands, output, preview, 1040, 580)


def finish_plot(commands: str, output: Path, preview: Path | None,
                width: int, height: int) -> None:
    if preview is not None:
        commands += f"""
set terminal pngcairo size {width},{height} enhanced font 'Arial,14' background rgb '#fffefa'
set output {quoted(str(preview))}
replot
"""
    subprocess.run(["gnuplot"], input=commands, text=True, check=True)
    # Keep generated SVG whitespace stable for source control.
    output.write_text("\n".join(line.rstrip() for line in output.read_text().splitlines())
                      .rstrip() + "\n")


def render_growth(preview_dir: Path | None) -> None:
    sources: set[Path] = set()
    summaries = {}
    input_hashes = set()
    for run in (EARLIER_RUN, RLE_RUN):
        archive = HERE / "artifacts" / run
        source = archive / "run/summary.tsv"
        with source.open() as stream:
            summaries[run] = {row["server"]: row for row in
                              csv.DictReader(stream, delimiter="\t")}
        metadata = archive / "run/metadata.txt"
        fields = dict(line.split("=", 1) for line in metadata.read_text().splitlines()
                      if "=" in line)
        if fields.get("hostname") != "naamah" or fields.get("verification_status") != "0":
            raise ValueError(f"expected a verified naamah replay: {run}")
        hashes = archive / "input-after.sha256"
        input_hashes.add(hashes.read_text().splitlines()[0].split()[0])
        sources.update((source, metadata, hashes))
    if len(input_hashes) != 1:
        raise ValueError("the two runs must use the same numeric command trace")

    reference = HERE / "artifacts" / RLE_RUN / "run/digests" / f"{RLE_ENGINE}.json"
    baseline = json.loads(reference.read_text())
    rle_rss = int(summaries[RLE_RUN][RLE_ENGINE]["os_rss_kb"])
    observations = []
    endpoints = []
    blocks = []
    plots = []
    annotations = []
    for index, (run, engine, label, color, width, dash, label_y) in enumerate(GROWTH_SERIES):
        root = HERE / "artifacts" / run / "run"
        final = summaries[run][engine]
        digest_path = root / "digests" / f"{engine}.json"
        digest = json.loads(digest_path.read_text())
        if (final["status"] != "PASS" or final["command_errors"] != "0"
                or final["zcard"] != "80798328" or digest["order_violations"] != 0
                or not digest["all_scores_integral"]):
            raise ValueError(f"expected a passing full replay: {engine}")
        for field in ("count", "score_sum", "mapping_xor_blake2b128",
                      "mapping_sum_blake2b128", "mapping_square_sum_blake2b128"):
            if digest[field] != baseline[field]:
                raise ValueError(f"{engine}: final mapping differs in {field}")
        sample_path = root / "samples" / f"{engine}.tsv"
        with sample_path.open() as stream:
            samples = list(csv.DictReader(stream, delimiter="\t"))
        points = []
        previous_count = -1
        previous_time = -1
        for row in samples:
            members, rss = int(row["zcard"]), int(row["os_rss_kb"])
            elapsed = int(row["elapsed_seconds"])
            if (not previous_count <= members <= 80798328 or rss <= 0
                    or elapsed < previous_time):
                raise ValueError(f"invalid or regressing sample: {engine}: {row}")
            previous_count, previous_time = members, elapsed
            points.append(f"{members / 1e6:.9f} {rss / 1024**2:.9f}")
            observations.append((run, engine, label, "sample", row["elapsed_seconds"],
                                 members, rss))
        if not samples or int(samples[0]["zcard"]) != 0 or previous_count != 80798328:
            raise ValueError(f"incomplete sample series: {engine}")
        # Preserve every sample. Add the separate endpoint from summary.tsv;
        # some incumbent RSS readings differ by 12 KiB between these two reads.
        final_rss = int(final["os_rss_kb"])
        final_gib = final_rss / 1024**2
        points.append(f"80.798328000 {final_gib:.9f}")
        observations.append((run, engine, label, "final", final["feed_seconds"],
                             80798328, final_rss))
        endpoints.append((run, engine, label, final_rss, final_gib,
                          100 * (1 - rle_rss / final_rss)))
        sources.update((sample_path, digest_path))
        blocks.append(f"$series{index} << EOD\n" + "\n".join(points) + "\nEOD")
        blocks.append(f"$end{index} << EOD\n80.798328 {final_gib:.9f}\nEOD")
        plots.append(f"$series{index} using 1:2 with lines lw {width} dt {dash} lc rgb '{color}' notitle")
        plots.append(f"$end{index} using 1:2 with points pt 7 ps 0.8 lc rgb '{color}' notitle")
        y = final_gib if label_y is None else label_y
        annotations.append(
            f"set arrow {index + 1} from first 80.798328,{final_gib:.9f} "
            f"to graph 1.025,first {y:.9f} nohead lw 1 lc rgb '{color}' front\n"
            f"set label {index + 10} {quoted(f'{label}  {final_gib:.2f} GiB')} "
            f"at graph 1.035,first {y:.9f} left font ',12' tc rgb '{color}' front"
        )

    raw_engine = "goblin-packed-int32-float32-rle-off"
    raw_rss = int(summaries[RLE_RUN][raw_engine]["os_rss_kb"])
    endpoints.append((RLE_RUN, raw_engine, "Goblin packed, RLE off (same run)",
                      raw_rss, raw_rss / 1024**2, 100 * (1 - rle_rss / raw_rss)))
    derived = HERE / "artifacts/memory-growth-20260922"
    derived.mkdir(exist_ok=True)
    with (derived / "samples.tsv").open("w", newline="") as stream:
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        writer.writerow(("source_run", "engine", "label", "measurement", "elapsed_seconds",
                         "live_members", "os_rss_kib"))
        writer.writerows(observations)
    with (derived / "endpoints.tsv").open("w", newline="") as stream:
        writer = csv.writer(stream, delimiter="\t", lineterminator="\n")
        writer.writerow(("source_run", "engine", "label", "os_rss_kib", "os_rss_gib",
                         "rss_saved_with_rle_percent"))
        writer.writerows((run, engine, label, rss, f"{gib:.9f}", f"{saved:.6f}")
                         for run, engine, label, rss, gib, saved in endpoints)
    (derived / "sources.json").write_text(json.dumps({
        "numeric_input_sha256": next(iter(input_hashes)),
        "x_axis": "live unique members (ZCARD); chart units are millions",
        "y_axis": "process RSS; chart units are GiB (2^30 bytes)",
        "samples": "all recorded samples, plus each independently recorded final RSS",
        "series": [{"source_run": run, "engine": engine, "label": label}
                   for run, engine, label, *_ in GROWTH_SERIES],
        "source_sha256": {str(path.relative_to(HERE)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in sorted(sources)},
    }, indent=2) + "\n")

    savings = {engine: saved for _, engine, _, _, _, saved in endpoints}
    incumbent_savings = [savings[engine] for _, engine, *_ in GROWTH_SERIES[:4]]
    headline = f"80.8 million page IDs: {rle_rss / 1024**2:.2f} GiB with score RLE"
    comparison = (
        f"Final RSS: {savings['goblin-packed-int32-float32']:.1f}% below earlier packed Goblin / "
        f"{savings['goblin-standard']:.1f}% below standard Goblin / "
        f"{min(incumbent_savings):.1f}-{max(incumbent_savings):.1f}% below incumbents"
    )
    output = HERE / "rle-memory-growth-20260922.svg"
    commands = f"""
set encoding utf8
set terminal svg size 1180,720 dynamic enhanced font 'Arial,14' background rgb '#fffefa'
set output {quoted(str(output))}
set xlabel 'Live unique page IDs (millions)'
set ylabel 'Process RSS (GiB)' offset -1,0
set xrange [0:82]
set yrange [0:8.2]
set xtics 10 nomirror textcolor rgb '#45483f'
set ytics 1 nomirror textcolor rgb '#45483f'
set border 3 lc rgb '#b3b8ae'
set grid ytics lc rgb '#e4e7df' back
unset key
set lmargin at screen 0.08
set rmargin at screen 0.745
set tmargin at screen 0.79
set bmargin at screen 0.16
set label 1 {quoted(headline)} at screen 0.08,0.95 font ',23' front
set label 2 {quoted(comparison)} at screen 0.08,0.90 font ',12' tc rgb '#45483f' front
set label 3 'RLE: Sep 22 (12 concurrent variants). Other curves: Sep 8 (6 engines). Same host and trace; separate runs.' at screen 0.08,0.055 font ',11' tc rgb '#62645c'
set label 4 'Five-minute RSS samples plus final readings. Lines connect observations; final RSS is not peak RSS.' at screen 0.08,0.022 font ',11' tc rgb '#62645c'
{chr(10).join(annotations)}
{chr(10).join(blocks)}
plot {', '.join(plots)}
"""
    finish_plot(commands, output, preview_dir / "rle-memory-growth-20260922.png"
                if preview_dir else None, 1180, 720)
    print(output)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--preview-dir", type=Path)
    args = parser.parse_args()
    archive = HERE / "artifacts/rle-full-20260922T035401Z"
    if not (archive / "JOB-PASS").exists():
        raise SystemExit("refusing to chart a run without JOB-PASS")
    with (archive / "run/summary.tsv").open() as stream:
        records = list(csv.DictReader(stream, delimiter="\t"))
    rows = {row["server"]: row for row in records}
    expected = {f"goblin-packed-{member}-{score}-rle-{mode}"
                for member, score in LAYOUTS for mode in ("off", "on")}
    if len(records) != 12 or set(rows) != expected or any(
        row["status"] != "PASS" or row["command_errors"] != "0"
        or row["zcard"] != "80798328" for row in records
    ):
        raise SystemExit("expected twelve passing full-state results")
    if args.preview_dir:
        args.preview_dir.mkdir(parents=True, exist_ok=True)
    for metric, field, divisor, title, ylabel, limit in (
        ("memory", "os_rss_kb", 1024**2,
         "Score RLE reduces final RSS in every typed layout",
         "Final process RSS (GiB)", 6.8),
        ("time", "feed_seconds", 3600,
         "Replay-time differences stay below 1% in this trial",
         "Replay duration (hours)", 12),
    ):
        stem = f"rle-{metric}-20260922"
        render(rows, field=field, divisor=divisor, title=title, ylabel=ylabel,
               limit=limit, output=HERE / f"{stem}.svg",
               preview=(args.preview_dir / f"{stem}.png") if args.preview_dir else None)
        print(HERE / f"{stem}.svg")
    render_growth(args.preview_dir)


if __name__ == "__main__":
    main()
