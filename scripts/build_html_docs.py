#!/usr/bin/env python3
"""Build static HTML documentation from Goblin Core Markdown files."""

from __future__ import annotations

import argparse
import html
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


ROOT = Path(__file__).resolve().parents[1]


STYLE = """\
:root {
  color-scheme: light;
  --bg: #fbfbf8;
  --text: #171717;
  --muted: #60605a;
  --border: #d9d8cf;
  --panel: #ffffff;
  --accent: #0f766e;
  --code-bg: #f0efea;
}

* {
  box-sizing: border-box;
}

body {
  margin: 0;
  background: var(--bg);
  color: var(--text);
  font: 16px/1.58 ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont,
    "Segoe UI", sans-serif;
}

main {
  max-width: 920px;
  margin: 0 auto;
  padding: 32px 22px 56px;
}

nav {
  display: flex;
  flex-wrap: wrap;
  gap: 10px 18px;
  margin: 0 0 28px;
  padding-bottom: 16px;
  border-bottom: 1px solid var(--border);
}

nav a {
  color: var(--accent);
  font-weight: 650;
  text-decoration: none;
}

nav a[aria-current="page"] {
  color: var(--text);
}

h1,
h2,
h3 {
  line-height: 1.16;
  margin: 1.6em 0 0.45em;
}

h1 {
  margin-top: 0;
  font-size: 2.3rem;
}

h2 {
  font-size: 1.55rem;
}

h3 {
  font-size: 1.2rem;
}

p,
ul,
table,
pre {
  margin: 0 0 1.1rem;
}

a {
  color: var(--accent);
}

code {
  padding: 0.12em 0.3em;
  border-radius: 4px;
  background: var(--code-bg);
  font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
  font-size: 0.92em;
}

pre {
  overflow-x: auto;
  padding: 14px 16px;
  border: 1px solid var(--border);
  border-radius: 6px;
  background: #20201d;
  color: #f7f7f1;
}

pre code {
  padding: 0;
  background: transparent;
  color: inherit;
}

table {
  width: 100%;
  border-collapse: collapse;
  display: block;
  overflow-x: auto;
}

th,
td {
  padding: 8px 10px;
  border: 1px solid var(--border);
  text-align: left;
  vertical-align: top;
}

th {
  background: var(--code-bg);
}

blockquote {
  margin: 0 0 1.1rem;
  padding-left: 1rem;
  border-left: 4px solid var(--border);
  color: var(--muted);
}

h1 .hero-link {
  color: inherit;
  text-decoration: none;
}

h1 .hero-link:hover {
  text-decoration: underline;
}

footer {
  margin-top: 3rem;
  padding-top: 1rem;
  border-top: 1px solid var(--border);
  color: var(--muted);
  font-size: 0.92rem;
}
"""


REPO_URL = "https://github.com/adamdeprince/goblin-core"
HERO_IMAGE = "goblin-core.png"


@dataclass(frozen=True)
class Page:
    source: Path
    output: Path
    title: str


def output_name(source: Path) -> str:
    if source.name == "README.md":
        return "index.html"
    return f"{source.stem}.html"


def slugify(text: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", text.lower())
    return slug.strip("-") or "section"


def strip_inline_markdown(text: str) -> str:
    text = re.sub(r"`([^`]+)`", r"\1", text)
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", text)
    text = re.sub(r"[*_]{1,2}([^*_]+)[*_]{1,2}", r"\1", text)
    return text.strip()


def title_for(source: Path) -> str:
    for line in source.read_text().splitlines():
        if line.startswith("# "):
            return strip_inline_markdown(line[2:])
    return source.stem


def normalize_source(path: Path) -> Path:
    if path.is_absolute():
        return path
    return (ROOT / path).resolve()


def normalize_sources(sources: Sequence[Path]) -> list[Path]:
    if sources:
        paths = [normalize_source(path) for path in sources]
    else:
        paths = sorted(ROOT.glob("*.md"))
    return [path for path in paths if path.name != "LICENSE.md"]


def rewrite_md_href(href: str) -> str:
    anchor = ""
    if "#" in href:
        href, anchor = href.split("#", 1)
        anchor = f"#{anchor}"
    if href.endswith(".md"):
        filename = Path(href).name
        prefix = href[: -len(filename)]
        if filename == "README.md":
            return f"{prefix}index.html{anchor}"
        return f"{prefix}{Path(filename).stem}.html{anchor}"
    return f"{href}{anchor}"


def rewrite_link_text(text: str) -> str:
    return re.sub(r"\.md\b", "", text)


def render_inline(text: str) -> str:
    placeholders: list[str] = []

    def store(value: str) -> str:
        placeholders.append(value)
        return f"\x00{len(placeholders) - 1}\x00"

    def render_link(match: re.Match[str]) -> str:
        label = rewrite_link_text(match.group(1))
        href = rewrite_md_href(match.group(2).strip())
        return store(
            f'<a href="{html.escape(href, quote=True)}">{render_inline(label)}</a>'
        )

    def render_code(match: re.Match[str]) -> str:
        return store(f"<code>{html.escape(match.group(1))}</code>")

    text = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", render_link, text)
    text = re.sub(r"`([^`]+)`", render_code, text)
    text = html.escape(text)
    text = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", text)
    text = re.sub(r"\*([^*]+)\*", r"<em>\1</em>", text)

    for index, value in enumerate(placeholders):
        text = text.replace(f"\x00{index}\x00", value)
    return text


def is_table_separator(line: str) -> bool:
    cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
    return bool(cells) and all(re.fullmatch(r":?-{3,}:?", cell) for cell in cells)


def split_table_row(line: str) -> list[str]:
    return [cell.strip() for cell in line.strip().strip("|").split("|")]


def render_table(lines: Sequence[str]) -> str:
    headers = split_table_row(lines[0])
    body = [split_table_row(line) for line in lines[2:]]
    out = ["<table>", "<thead>", "<tr>"]
    out.extend(f"<th>{render_inline(cell)}</th>" for cell in headers)
    out.extend(["</tr>", "</thead>", "<tbody>"])
    for row in body:
        out.append("<tr>")
        out.extend(f"<td>{render_inline(cell)}</td>" for cell in row)
        out.append("</tr>")
    out.extend(["</tbody>", "</table>"])
    return "\n".join(out)


def flush_paragraph(out: list[str], paragraph: list[str]) -> None:
    if paragraph:
        out.append(f"<p>{render_inline(' '.join(paragraph))}</p>")
        paragraph.clear()


def flush_list(out: list[str], list_items: list[str]) -> None:
    if list_items:
        out.append("<ul>")
        out.extend(f"<li>{render_inline(item)}</li>" for item in list_items)
        out.append("</ul>")
        list_items.clear()


def render_markdown(markdown: str, hero_href: str | None = None) -> str:
    lines = markdown.splitlines()
    out: list[str] = []
    paragraph: list[str] = []
    list_items: list[str] = []
    index = 0
    hero_used = False

    while index < len(lines):
        line = lines[index]

        if line.startswith("```"):
            flush_paragraph(out, paragraph)
            flush_list(out, list_items)
            language = line[3:].strip()
            index += 1
            code_lines: list[str] = []
            while index < len(lines) and not lines[index].startswith("```"):
                code_lines.append(lines[index])
                index += 1
            class_attr = (
                f' class="language-{html.escape(language, quote=True)}"'
                if language
                else ""
            )
            out.append(
                f"<pre><code{class_attr}>"
                f"{html.escape(chr(10).join(code_lines))}</code></pre>"
            )
            index += 1
            continue

        if not line.strip():
            flush_paragraph(out, paragraph)
            flush_list(out, list_items)
            index += 1
            continue

        if line.lstrip().startswith("|") and index + 1 < len(lines) and is_table_separator(lines[index + 1]):
            flush_paragraph(out, paragraph)
            flush_list(out, list_items)
            table_lines = [line, lines[index + 1]]
            index += 2
            while index < len(lines) and lines[index].lstrip().startswith("|"):
                table_lines.append(lines[index])
                index += 1
            out.append(render_table(table_lines))
            continue

        heading = re.match(r"^(#{1,6})\s+(.+)$", line)
        if heading:
            flush_paragraph(out, paragraph)
            flush_list(out, list_items)
            level = len(heading.group(1))
            text = heading.group(2).strip()
            anchor = slugify(strip_inline_markdown(text))
            inner = render_inline(text)
            if level == 1 and hero_href and not hero_used:
                # Easter egg: the page's main title links to the mascot image.
                inner = (f'<a class="hero-link" '
                         f'href="{html.escape(hero_href, quote=True)}">{inner}</a>')
                hero_used = True
            out.append(f'<h{level} id="{anchor}">{inner}</h{level}>')
            index += 1
            continue

        item = re.match(r"^\s*-\s+(.+)$", line)
        if item:
            flush_paragraph(out, paragraph)
            list_items.append(item.group(1).strip())
            index += 1
            continue

        quote = re.match(r"^\s*>\s?(.+)$", line)
        if quote:
            flush_paragraph(out, paragraph)
            flush_list(out, list_items)
            out.append(f"<blockquote>{render_inline(quote.group(1).strip())}</blockquote>")
            index += 1
            continue

        flush_list(out, list_items)
        paragraph.append(line.strip())
        index += 1

    flush_paragraph(out, paragraph)
    flush_list(out, list_items)
    return "\n".join(out)


def render_nav(pages: Sequence[Page], current: Page) -> str:
    links = []
    for page in pages:
        label = rewrite_link_text(page.title)
        href = html.escape(page.output.name, quote=True)
        current_attr = ' aria-current="page"' if page == current else ""
        links.append(f'<a href="{href}"{current_attr}>{html.escape(label)}</a>')
    return "<nav>" + "\n".join(links) + "</nav>"


def render_page(page: Page, pages: Sequence[Page]) -> str:
    hero_href = HERO_IMAGE if page.output.name == "index.html" else None
    body = render_markdown(page.source.read_text(), hero_href=hero_href)
    title = html.escape(page.title)
    return "\n".join([
        "<!doctype html>",
        '<html lang="en">',
        "<head>",
        '  <meta charset="utf-8">',
        '  <meta name="viewport" content="width=device-width, initial-scale=1">',
        f"  <title>{title}</title>",
        '  <link rel="stylesheet" href="styles.css">',
        "</head>",
        "<body>",
        "<main>",
        render_nav(pages, page),
        body,
        f'<footer>Source and issues on <a href="{REPO_URL}">GitHub</a>.</footer>',
        "</main>",
        "</body>",
        "</html>",
        "",
    ])


def write_pages(sources: Sequence[Path], output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    pages = [
        Page(source=source, output=output_dir / output_name(source), title=title_for(source))
        for source in sources
    ]
    pages.sort(key=lambda page: (page.output.name != "index.html", page.title.lower()))
    expected = {page.output for page in pages}
    expected.add(output_dir / "styles.css")

    for old_file in output_dir.glob("*.html"):
        if old_file not in expected:
            old_file.unlink()

    for page in pages:
        page.output.write_text(render_page(page, pages))
    (output_dir / "styles.css").write_text(STYLE)


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sources", nargs="*", type=Path,
                        help="Markdown files to convert. Defaults to root *.md files.")
    parser.add_argument("--output", type=Path, default=ROOT / "html",
                        help="Output directory for generated HTML.")
    return parser.parse_args(argv)


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    sources = normalize_sources(args.sources)
    write_pages(sources, args.output)
    for source in sources:
        print(f"{source.relative_to(ROOT)} -> {args.output / output_name(source)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))
