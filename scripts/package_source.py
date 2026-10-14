#!/usr/bin/env python3
"""Create a reproducible source archive, including uncommitted source edits."""
import argparse
import gzip
import io
import re
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {'include', 'src', 'third_party', 'cmake', 'sbe', 'scripts', 'tests',
           'docs', 'packaging', 'client', 'bluefield', 'python', 'html'}
EXCLUDE = {'__pycache__', '_vendor', 'build', 'dist', 'benchmarks', 'benchmark-results', '.venv'}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    version = re.search(r'\bVERSION\s+(\d+\.\d+\.\d+)', (ROOT/'CMakeLists.txt').read_text())[1]
    files = []
    for path in sorted(ROOT.rglob('*')):
        rel = path.relative_to(ROOT)
        if not path.is_file() or any(p.startswith('.') or p.startswith('build') or p in EXCLUDE for p in rel.parts):
            continue
        if rel.parts[0] not in SOURCES and not (len(rel.parts)==1 and (path.suffix=='.md' or path.name in {'CMakeLists.txt','LICENSE','NOTICE','Containerfile'})):
            continue
        if path.suffix in {'.so','.dylib','.pyc'}: continue
        # Repository packages disable Kafka and XLIO. Omit their unused vendor
        # trees, including upstream test-key fixtures, from public build inputs.
        if len(rel.parts)>1 and rel.parts[0]=='third_party' and rel.parts[1] in {'librdkafka','xlio'}:
            continue
        if path.name in {'RING-LATENCY-HANDOFF.md','TODO.md'}: continue
        files.append(path)
    args.output.mkdir(parents=True, exist_ok=True)
    archive = args.output/f'goblin-core-{version}.tar.gz'
    with archive.open('wb') as raw, gzip.GzipFile(fileobj=raw,mode='wb',mtime=0,filename='') as gz, tarfile.open(fileobj=gz,mode='w') as tar:
        for path in files:
            content=path.read_bytes()
            info=tarfile.TarInfo(f'goblin-core-{version}/{path.relative_to(ROOT)}')
            info.size=len(content)
            info.mode=0o755 if path.stat().st_mode & 0o111 else 0o644
            tar.addfile(info,io.BytesIO(content))
        content=(b'This is the build source for the Goblin Reactor repository packages.\n'
                 b'Kafka and XLIO vendor trees are omitted because these integrations\n'
                 b'are disabled in the packages. Configure with\n'
                 b'-DGOBLIN_CORE_ENABLE_KAFKA=OFF -DGOBLIN_CORE_ENABLE_XLIO=OFF.\n'
                 b'For the complete development tree, use the upstream Git repository.\n')
        info=tarfile.TarInfo(f'goblin-core-{version}/PACKAGE-SOURCE.txt')
        info.size=len(content)
        info.mode=0o644
        tar.addfile(info,io.BytesIO(content))
    print(archive)

if __name__=='__main__': main()
