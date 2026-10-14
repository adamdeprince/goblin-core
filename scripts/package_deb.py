#!/usr/bin/env python3
"""Build separate server and C++ SDK Debian packages from a compiled tree."""
import argparse
import hashlib
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def run(*args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)

def put(path, value, executable=False):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value)
    path.chmod(0o755 if executable else 0o644)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--suite', required=True)
    parser.add_argument('--revision', type=int, default=2)
    args = parser.parse_args()
    version = re.search(r'\bVERSION\s+(\d+\.\d+\.\d+)', (ROOT/'CMakeLists.txt').read_text())[1]
    version += f'-{args.revision}+{args.suite}1'
    architecture = subprocess.check_output(['dpkg','--print-architecture'], text=True).strip()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='goblin-package-') as temporary:
        work = Path(temporary)
        server = work/'server'
        sdk = work/'sdk'
        control = server/'DEBIAN'
        control.mkdir(parents=True)
        for name in ['goblin-core','goblin-core-auth']:
            path=server/'usr/bin'/name
            path.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(args.build/name,path)
        for name in ['postinst','prerm','postrm']:
            put(control/name,(ROOT/'packaging/debian'/name).read_text(),True)
        put(control/'conffiles','/etc/goblin-core/goblin-core.conf\n')
        put(server/'usr/lib/systemd/system/goblin-core.service',
            (ROOT/'packaging/debian/goblin-core.service').read_text())
        replacements={'@VAR@':'/var','@BIN@':'/usr/bin','@ETC@':'/etc/goblin-core'}
        # Linux runtime files belong under /run, not /var/run's compatibility link.
        for source,dest in [('goblin-core-service.in','usr/libexec/goblin-core-service'),
                            ('goblin-core.conf.in','etc/goblin-core/goblin-core.conf')]:
            value=(ROOT/'packaging'/source).read_text()
            for old,new in replacements.items(): value=value.replace(old,new)
            value=value.replace('/var/run/goblin-core','/run/goblin-core')
            put(server/dest,value,source.endswith('service.in'))
        for package,name in [(server,'goblin-core'),(sdk,'goblin-core-cpp')]:
            doc=package/'usr/share/doc'/name
            doc.mkdir(parents=True,exist_ok=True)
            for filename in ['LICENSE','NOTICE','INSTALL.md']:
                shutil.copy2(ROOT/filename,doc/filename)
            if (ROOT/'html/INSTALL.html').exists():
                shutil.copytree(ROOT/'html',doc/'html',ignore=shutil.ignore_patterns('benchmarks'))
        # Determine runtime dependencies against this release's installed libraries.
        debian=work/'debian'
        put(debian/'control','Source: goblin-core\nSection: database\nPriority: optional\nMaintainer: Adam DePrince <adam.deprince@gmail.com>\n\nPackage: goblin-core\nArchitecture: any\nDescription: Redis-compatible server\n')
        dependencies=run('dpkg-shlibdeps','-O',*[f'-e{server}/usr/bin/{n}' for n in ['goblin-core','goblin-core-auth']],cwd=work,capture_output=True,text=True).stdout.strip().split('=',1)[1]
        dependencies += ', adduser, init-system-helpers (>= 1.54)'
        put(control/'control',f'Package: goblin-core\nVersion: {version}\nArchitecture: {architecture}\nSection: database\nPriority: optional\nMaintainer: Adam DePrince <adam.deprince@gmail.com>\nDepends: {dependencies}\nHomepage: https://github.com/adamdeprince/goblin-core\nDescription: Compact Redis-compatible server\n Ordinary loopback RESP on port 6379 and a local Unix socket.\n')
        run('cmake','-S',ROOT/'client','-B',work/'client-build',
            '-DCMAKE_INSTALL_PREFIX=/usr','-DCMAKE_INSTALL_LIBDIR=lib')
        run('cmake','--install',work/'client-build','--prefix',sdk/'usr')
        # Exports use the relocated /usr prefix; install(SDK) contains no binaries.
        put(sdk/'DEBIAN/control',f'Package: goblin-core-cpp\nVersion: {version}\nArchitecture: all\nSection: libdevel\nPriority: optional\nMaintainer: Adam DePrince <adam.deprince@gmail.com>\nHomepage: https://github.com/adamdeprince/goblin-core\nDescription: Goblin Core C++ client SDK\n Header-only C++20 SBE clients and generated codecs; CMake target goblin::client.\n The server and Python packages are installed separately.\n')
        for package,name,arch in [(server,'goblin-core',architecture),(sdk,'goblin-core-cpp','all')]:
            lines=[]
            for path in sorted(package.rglob('*')):
                if path.is_file() and 'DEBIAN' not in path.parts:
                    lines.append(hashlib.md5(path.read_bytes()).hexdigest()+'  '+str(path.relative_to(package)))
            put(package/'DEBIAN/md5sums','\n'.join(lines)+'\n')
            dest=args.output/f'{name}_{version}_{arch}.deb'
            run('dpkg-deb','--root-owner-group','--build',package,dest)
            print(dest,flush=True)

if __name__=='__main__': main()
