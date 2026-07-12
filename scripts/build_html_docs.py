#!/usr/bin/env python3
"""Build static HTML documentation from Goblin Core Markdown files."""

from __future__ import annotations

import argparse
import html
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


ROOT = Path(__file__).resolve().parents[1]


STYLE = """\
:root {
  color-scheme: light;
  --ink: #12140f;
  --paper: #f7f6f1;
  --white: #fffefa;
  --muted: #62645c;
  --line: #d8d8cf;
  --acid: #a6e22e;
  --acid-dark: #548a08;
  --cyan: #0b8897;
  --orange: #ed6b2f;
  --dark: #11130f;
  --mono: "SFMono-Regular", Consolas, "Liberation Mono", monospace;
  --sans: Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont,
    "Segoe UI", sans-serif;
  --serif: Georgia, "Times New Roman", serif;
}

* {
  box-sizing: border-box;
}

html {
  scroll-behavior: smooth;
}

body {
  margin: 0;
  background: var(--paper);
  color: var(--ink);
  font: 16px/1.62 var(--sans);
  letter-spacing: 0;
}

a {
  color: var(--cyan);
  text-underline-offset: 3px;
}

.skip-link {
  position: fixed;
  top: 8px;
  left: 8px;
  z-index: 100;
  transform: translateY(-150%);
  padding: 8px 12px;
  border: 1px solid var(--ink);
  background: var(--white);
  color: var(--ink);
}

.skip-link:focus {
  transform: none;
}

.site-header {
  border-bottom: 1px solid var(--line);
  background: rgba(255, 254, 250, 0.96);
}

.site-nav {
  width: min(calc(100% - 40px), 1180px);
  min-height: 76px;
  margin: 0 auto;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 28px;
}

.brand {
  display: inline-flex;
  flex: 0 0 auto;
  align-items: center;
  gap: 10px;
  color: var(--ink);
  font: 850 0.98rem/1 var(--mono);
  text-decoration: none;
}

.brand-mark {
  width: 38px;
  height: 38px;
  overflow: hidden;
  border: 1px solid var(--ink);
  border-radius: 50%;
  background: var(--white);
}

.brand-mark img {
  width: 100%;
  height: 100%;
  object-fit: cover;
  transform: scale(1.45) translateY(4%);
}

.nav-links {
  display: flex;
  align-items: center;
  gap: 25px;
}

.nav-links a {
  color: var(--ink);
  font-size: 0.82rem;
  font-weight: 760;
  text-decoration: none;
}

.nav-links a:hover,
.nav-links a[aria-current="page"] {
  color: var(--acid-dark);
}

.nav-links .nav-cta {
  padding: 9px 14px;
  border: 1px solid var(--ink);
  background: var(--ink);
  color: white;
}

.nav-links .nav-cta:hover {
  border-color: var(--orange);
  background: var(--orange);
  color: white;
}

.doc-shell {
  width: min(calc(100% - 40px), 980px);
  margin: 0 auto;
  padding: 80px 0 72px;
}

.doc {
  min-width: 0;
}

h1,
h2,
h3,
h4 {
  font-family: var(--sans);
  letter-spacing: 0;
}

h1 {
  max-width: 900px;
  margin: 0 0 34px;
  font-size: clamp(3rem, 7vw, 6.3rem);
  font-weight: 880;
  line-height: 0.94;
}

h2 {
  margin: 2.3em 0 0.65em;
  padding-top: 0.75em;
  border-top: 1px solid var(--ink);
  font-size: clamp(1.65rem, 3vw, 2.4rem);
  font-weight: 820;
  line-height: 1.08;
}

h3 {
  margin: 1.8em 0 0.55em;
  font-size: 1.25rem;
  line-height: 1.18;
}

h4 {
  margin: 1.5em 0 0.5em;
  font-size: 1rem;
}

.doc > p:first-of-type {
  max-width: 850px;
  margin: -8px 0 32px;
  color: #30322d;
  font: 1.18rem/1.6 var(--serif);
}

p,
ul,
table,
pre {
  margin: 0 0 1.2rem;
}

ul {
  padding-left: 1.25rem;
}

li {
  margin: 0.32rem 0;
  padding-left: 0.18rem;
}

li::marker {
  color: var(--acid-dark);
}

code {
  padding: 0.13em 0.34em;
  border-radius: 4px;
  background: #e9e8e1;
  font-family: var(--mono);
  font-size: 0.9em;
}

pre {
  overflow-x: auto;
  padding: 22px 24px;
  border: 1px solid var(--ink);
  border-radius: 0;
  background: var(--dark);
  color: #f5f6f1;
  font: 0.82rem/1.7 var(--mono);
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
  border-block: 1px solid var(--ink);
}

th,
td {
  padding: 12px 14px;
  border: 0;
  border-bottom: 1px solid var(--line);
  text-align: left;
  vertical-align: top;
  font-size: 0.84rem;
}

th {
  color: var(--muted);
  font: 720 0.7rem/1.35 var(--mono);
  text-transform: uppercase;
}

tbody tr:last-child td {
  border-bottom: 0;
}

blockquote {
  margin: 1.6rem 0;
  padding: 18px 22px;
  border-left: 4px solid var(--orange);
  background: var(--white);
  color: var(--muted);
  font-family: var(--serif);
}

img {
  display: block;
  max-width: 100%;
  height: auto;
}

.doc img {
  margin: 2rem auto;
  border: 1px solid var(--line);
}

.doc-footer {
  display: flex;
  justify-content: space-between;
  gap: 30px;
  margin-top: 4.5rem;
  padding-top: 1.25rem;
  border-top: 1px solid var(--ink);
  color: var(--muted);
  font: 0.76rem/1.5 var(--mono);
}

.doc-footer a {
  font-weight: 760;
}

@media (max-width: 800px) {
  .nav-links a:nth-child(3),
  .nav-links a:nth-child(4) {
    display: none;
  }
}

@media (max-width: 580px) {
  .site-nav,
  .doc-shell {
    width: min(calc(100% - 30px), 980px);
  }

  .site-nav {
    min-height: 66px;
  }

  .brand-mark {
    width: 34px;
    height: 34px;
  }

  .nav-links {
    gap: 12px;
  }

  .nav-links a:not(.nav-cta) {
    display: none;
  }

  .nav-links .nav-cta {
    padding: 8px 11px;
    font-size: 0.75rem;
  }

  .doc-shell {
    padding: 52px 0 55px;
  }

  h1 {
    font-size: clamp(2.75rem, 15vw, 4.5rem);
  }

  .doc > p:first-of-type {
    font-size: 1.06rem;
  }

  th,
  td {
    padding: 10px;
  }

  .doc-footer {
    flex-direction: column;
    gap: 8px;
  }
}

@media (prefers-reduced-motion: reduce) {
  html {
    scroll-behavior: auto;
  }
}
"""


REPO_URL = "https://github.com/adamdeprince/goblin-core"
BRAND_IMAGE = "goblin.png"


@dataclass(frozen=True)
class Page:
    source: Path
    output: Path
    title: str


def output_name(source: Path) -> str:
    # README.md -> README.html (not index.html): a hand-authored index.html is the site
    # landing page and must not be overwritten by the generated README.
    return f"{source.stem}.html"


EXCLUDE_DIRS = {".git", "html", "node_modules", "__pycache__", ".venv", "venv",
                "third_party", "memory", "benchmark-results"}

EXCLUDE_FILES = {"RELEASE.md", "RING-LATENCY-HANDOFF.md"}

IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".gif", ".svg", ".webp"}


def is_excluded(path: Path) -> bool:
    # Skip vendored code, build/output trees, virtualenvs, and agent memory when walking
    # the tree -- only the project's own docs should become pages.
    try:
        relative = path.relative_to(ROOT)
    except ValueError:
        return False
    if relative.name in EXCLUDE_FILES:
        return True
    for part in relative.parts[:-1]:
        if part in EXCLUDE_DIRS or part.startswith("build"):
            return True
    return False


def output_path(source: Path, output_dir: Path) -> Path:
    # Mirror the source's directory under the output root (blogs/x.md -> blogs/x.html) so
    # relative links between docs survive and same-named files (many README.md) don't
    # collide.
    try:
        rel_dir = source.relative_to(ROOT).parent
    except ValueError:
        rel_dir = Path(".")
    return output_dir / rel_dir / output_name(source)


def rel_prefix(output: Path, output_dir: Path) -> str:
    # "../" per directory the page sits below the output root, for depth-correct links to
    # shared assets (styles.css, the hero image) and nav targets.
    depth = len(output.relative_to(output_dir).parts) - 1
    return "../" * depth


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
        paths = sorted(default_doc_sources())
    return [path for path in paths if path.name != "LICENSE.md" and not is_excluded(path)]


def default_doc_sources() -> list[Path]:
    # The project's own docs: tracked files plus untracked-but-not-gitignored ones (so a
    # freshly written, uncommitted doc still builds), minus gitignored local output
    # (benchmark-results/, build trees). is_excluded then drops tracked-but-vendored trees
    # (third_party/) and agent memory. Falls back to a filesystem walk without git.
    def ls(*args: str) -> list[str]:
        out = subprocess.run(["git", "-C", str(ROOT), "ls-files", "-z", *args],
                             capture_output=True, check=True).stdout.decode()
        return [name for name in out.split("\0") if name]

    try:
        names = set(ls()) | set(ls("--others", "--exclude-standard"))
    except (OSError, subprocess.CalledProcessError):
        return [p for p in ROOT.rglob("*.md") if not is_excluded(p)]
    md = (ROOT / name for name in names if name.endswith(".md"))
    return [path for path in md if not is_excluded(path)]


def rewrite_md_href(href: str) -> str:
    anchor = ""
    if "#" in href:
        href, anchor = href.split("#", 1)
        anchor = f"#{anchor}"
    if href.endswith(".md"):
        filename = Path(href).name
        if filename in EXCLUDE_FILES:
            repo_path = href.lstrip("./")
            while repo_path.startswith("../"):
                repo_path = repo_path[3:]
            return f"{REPO_URL}/blob/main/{repo_path}{anchor}"
        prefix = href[: -len(filename)]
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

    def render_image(match: re.Match[str]) -> str:
        alt = strip_inline_markdown(match.group(1))
        src = match.group(2).strip()
        return store(
            f'<img src="{html.escape(src, quote=True)}" '
            f'alt="{html.escape(alt, quote=True)}" loading="lazy">'
        )

    def render_code(match: re.Match[str]) -> str:
        return store(f"<code>{html.escape(match.group(1))}</code>")

    text = re.sub(r"!\[([^\]]*)\]\(([^)]+)\)", render_image, text)
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

        if list_items and line[:1].isspace():
            list_items[-1] += " " + line.strip()
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


def render_nav(nav_pages: Sequence[Page], current: Page, prefix: str) -> str:
    by_source_name = {page.source.name: page for page in nav_pages}
    primary = (
        ("README.md", "Docs"),
        ("BENCHMARKS.md", "Benchmarks"),
        ("PERFORMANCE_BRIEF.md", "Architecture"),
        ("LATENCY-SHOOTOUT.md", "Ring latency"),
    )
    links = []
    for source_name, label in primary:
        page = by_source_name.get(source_name)
        if page is None:
            continue
        href = html.escape(prefix + page.output.name, quote=True)
        current_attr = ' aria-current="page"' if page == current else ""
        links.append(f'<a href="{href}"{current_attr}>{label}</a>')
    links.append(f'<a class="nav-cta" href="{REPO_URL}">GitHub</a>')
    brand_src = html.escape(prefix + BRAND_IMAGE, quote=True)
    home_href = html.escape(prefix + "index.html", quote=True)
    return "\n".join([
        '<header class="site-header">',
        '<nav class="site-nav" aria-label="Primary navigation">',
        f'<a class="brand" href="{home_href}" aria-label="Goblin Core home">',
        f'<span class="brand-mark"><img src="{brand_src}" alt=""></span>',
        '<span>goblin_core</span>',
        '</a>',
        f'<div class="nav-links">{"".join(links)}</div>',
        '</nav>',
        '</header>',
    ])


def render_page(page: Page, nav_pages: Sequence[Page], output_dir: Path) -> str:
    prefix = rel_prefix(page.output, output_dir)
    body = render_markdown(page.source.read_text())
    title = html.escape(page.title)
    document_title = title if page.title == "Goblin Core" else f"{title} — Goblin Core"
    styles_href = html.escape(prefix + "styles.css", quote=True)
    favicon_href = html.escape(prefix + BRAND_IMAGE, quote=True)
    return "\n".join([
        "<!doctype html>",
        '<html lang="en">',
        "<head>",
        '  <meta charset="utf-8">',
        '  <meta name="viewport" content="width=device-width, initial-scale=1">',
        f"  <title>{document_title}</title>",
        f'  <link rel="icon" href="{favicon_href}" type="image/png" sizes="1254x1254">',
        f'  <link rel="apple-touch-icon" href="{favicon_href}">',
        f'  <link rel="stylesheet" href="{styles_href}">',
        "</head>",
        "<body>",
        '<a class="skip-link" href="#content">Skip to content</a>',
        render_nav(nav_pages, page, prefix),
        '<main class="doc-shell" id="content">',
        '<article class="doc">',
        body,
        '</article>',
        '<footer class="doc-footer">',
        '<span>GOBLIN CORE / APACHE-2.0</span>',
        f'<span>Source and issues on <a href="{REPO_URL}">GitHub</a>.</span>',
        '</footer>',
        "</main>",
        "</body>",
        "</html>",
        "",
    ])


def write_pages(sources: Sequence[Path], output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    pages = [
        Page(source=source, output=output_path(source, output_dir), title=title_for(source))
        for source in sources
    ]
    # Nav lists only the top-level pages: a flat nav of every nested doc (dozens of command
    # pages) would be unusable. README first, then alphabetical by title.
    root_readme = output_dir / "README.html"
    nav_pages = sorted(
        (page for page in pages if page.output.parent == output_dir),
        key=lambda page: (page.output != root_readme, page.title.lower()),
    )
    expected = {page.output for page in pages}
    expected.add(output_dir / "styles.css")

    for old_file in output_dir.rglob("*.html"):
        if old_file.name == "index.html":
            continue  # hand-authored landing page: never generated or pruned here
        if old_file not in expected:
            old_file.unlink()

    for page in pages:
        page.output.parent.mkdir(parents=True, exist_ok=True)
        page.output.write_text(render_page(page, nav_pages, output_dir))
    (output_dir / "styles.css").write_text(STYLE)

    brand_source = ROOT / "html" / BRAND_IMAGE
    brand_dest = output_dir / BRAND_IMAGE
    if brand_source.exists() and brand_source.resolve() != brand_dest.resolve():
        brand_dest.write_bytes(brand_source.read_bytes())

    # Copy image assets that sit alongside a converted doc (blog charts, the hero image)
    # into the mirrored tree so the pages can reference them.
    for source_dir in {page.source.parent for page in pages}:
        try:
            source_dir.relative_to(ROOT)
        except ValueError:
            continue
        for asset in source_dir.iterdir():
            if asset.is_file() and asset.suffix.lower() in IMAGE_EXTS:
                dest = output_dir / asset.relative_to(ROOT)
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_bytes(asset.read_bytes())

    # Copy the LLM-oriented docs (the llms.txt convention) verbatim to the site
    # root so they are served at /llms.txt and /llms-full.txt. They are rebuilt
    # here on each run, not committed under html/.
    for name in ("llms.txt", "llms-full.txt"):
        source_file = ROOT / name
        if source_file.exists():
            (output_dir / name).write_text(source_file.read_text())


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
        rel = source.relative_to(ROOT) if source.is_relative_to(ROOT) else source
        print(f"{rel} -> {output_path(source, args.output)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))
