#!/usr/bin/env python3
"""Render the verified full-run RLE measurements as standalone SVG charts.

Requires gnuplot. Pass --preview-dir to also render PNGs for local inspection.
The archived measurements and frozen benchmark harness are never modified.
"""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
import subprocess


HERE = Path(__file__).resolve().parent
LAYOUTS = [(member, score) for member in ("int32", "int64", "uuid")
           for score in ("float32", "float64")]


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
    if preview is not None:
        commands += f"""
set terminal pngcairo size 1040,580 enhanced font 'Arial,14' background rgb '#fffefa'
set output {quoted(str(preview))}
replot
"""
    subprocess.run(["gnuplot"], input=commands, text=True, check=True)
    # Keep generated SVG whitespace stable for source control.
    output.write_text("\n".join(line.rstrip() for line in output.read_text().splitlines())
                      .rstrip() + "\n")


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


if __name__ == "__main__":
    main()
