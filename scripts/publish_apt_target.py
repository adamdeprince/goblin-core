#!/usr/bin/env python3
"""Add one tested architecture/release to the existing signed APT repository."""
import argparse
import datetime
import fcntl
import hashlib
import html
import json
import re
import shutil
import subprocess
from pathlib import Path

from publish_package_repositories import ADMIN, PUBLIC, SUITES, artifact, backup, run, sign


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def immutable_copy(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if digest(source) != digest(destination):
            raise RuntimeError(f'Refusing to replace published artifact: {destination}')
    else:
        shutil.copy2(source, destination)


def proof(stage, name, args):
    data = json.loads((stage / name).read_text())
    expected = {'result': 'PASS', 'architecture': args.architecture,
                'server_version': args.version, 'sdk_version': args.version}
    for field, value in expected.items():
        if data.get(field) != value:
            raise RuntimeError(f'{name}: expected {field}={value!r}')
    return data


def packages(stage, args):
    result = []
    for name, architecture in [('goblin-core', args.architecture), ('goblin-core-cpp', 'all')]:
        path = stage / f'{name}_{args.version}_{architecture}.deb'
        fields = subprocess.check_output(
            ['dpkg-deb', '-f', str(path), 'Package', 'Version', 'Architecture'], text=True)
        actual = dict(line.split(': ', 1) for line in fields.strip().splitlines())
        expected = {'Package': name, 'Version': args.version, 'Architecture': architecture}
        if actual != expected:
            raise RuntimeError(f'Unexpected package fields: {actual}')
        result.append((name, architecture, path))
    return result


def publish_packages(stage, site, work, args, binaries):
    backup(work, site)
    admin_backup = work / 'backups' / 'apt-admin'
    if not admin_backup.exists():
        shutil.copytree(ADMIN / 'reprepro/db', admin_backup / 'db')
        shutil.copytree(ADMIN / 'reprepro/conf', admin_backup / 'conf')
        shutil.copytree(site / 'dists', admin_backup / 'dists')
    import os
    environment = dict(os.environ, GNUPGHOME=str(ADMIN / 'gnupg'))
    for name, architecture, path in binaries:
        destination = site / 'pool/main/g' / name / path.name
        if destination.exists() and digest(path) != digest(destination):
            raise RuntimeError(f'Refusing to replace published package: {destination}')
    for name, architecture, path in binaries:
        run('reprepro', '--keepunreferencedfiles', '-b', ADMIN / 'reprepro',
            '--export=never', 'includedeb', args.suite, path, env=environment)
    run('reprepro', '--keepunreferencedfiles', '-b', ADMIN / 'reprepro',
        'check', args.suite, env=environment)
    run('reprepro', '--keepunreferencedfiles', '-b', ADMIN / 'reprepro',
        'export', args.suite, env=environment)
    run('gpgv', '--keyring', site / 'goblinreactor-archive-keyring.gpg',
        site / 'dists' / args.suite / 'InRelease')
    source = site / 'sources' / args.source.name
    immutable_copy(args.source, source)
    sign(source)


def publish_indexes(stage, site, work, args, binaries):
    public_proof = proof(stage, 'public-check.json', args)
    for name, architecture, path in binaries:
        published = site / 'pool/main/g' / name / path.name
        if digest(path) != digest(published):
            raise RuntimeError(f'Published package differs from tested artifact: {published}')
        if public_proof['package_sha256'][name] != digest(published):
            raise RuntimeError(f'Public installation did not verify {name}')
    backup(work, site)
    original_catalog = (site / 'packages.json').read_text()
    (work / 'catalog-before-indexes.json').write_text(original_catalog)
    catalog = json.loads(original_catalog)
    original_packages = catalog['packages']
    replacing = {('goblin-core', args.suite, args.architecture),
                 ('goblin-core-cpp', args.suite, 'all')}
    key = lambda entry: (entry['name'], entry.get('suite'), entry.get('architecture'))
    retained = [entry for entry in original_packages if key(entry) not in replacing]
    docs = stage / 'docs'
    installation = (docs / 'INSTALL.html').read_text()
    if 'ARM64' not in installation or 'Ubuntu 26.04' not in installation:
        raise RuntimeError('Generated installation guide does not describe the ARM target')
    shutil.copytree(docs, site / 'goblin-core-docs', dirs_exist_ok=True)
    installation = re.sub(r'(href|src)="(?!https?:|#|mailto:|data:)([^"\n]+)"',
                          r'\1="goblin-core-docs/\2"', installation)
    (site / 'goblin-core.html').write_text(installation)
    platforms = {(entry.get('architecture'), entry.get('suite'))
                 for entry in original_packages if entry['name'] == 'goblin-core'
                 and entry.get('suite') in SUITES}
    platforms.add((args.architecture, args.suite))
    targets = '; '.join(architecture + ' on ' + ', '.join(
        SUITES[suite] for suite in SUITES if (architecture, suite) in platforms)
        for architecture in sorted({architecture for architecture, suite in platforms}))
    upstream_version = args.version.split('-', 1)[0]
    summary = (f'Goblin Core {upstream_version} is available through this signed APT repository for {targets}. '
               'The goblin-core package installs the Redis-compatible server; '
               'goblin-core-cpp is a separate C++ client SDK. '
               'The service uses loopback TCP port 6379 and a Unix socket. '
               'Stop an existing Redis service first if it uses port 6379.')
    index_path = site / 'index.html'
    index, count = re.subn(r'(<h3 id="goblin-core">.*?</h3>\s*<p>).*?(</p>)',
                          lambda match: match[1] + html.escape(summary) + match[2],
                          index_path.read_text(), count=1, flags=re.S)
    if count != 1:
        raise RuntimeError('Existing Goblin Core repository index section not found')
    index_path.write_text(index)
    for filename in ['README.md', 'README.txt']:
        path = site / filename
        if not path.exists():
            continue
        text, count = re.subn(r'(### goblin-core and goblin-core-cpp\s*\n).*?(?=\n\n)',
                             lambda match: match[1] + summary,
                             path.read_text(), count=1, flags=re.S)
        if count != 1:
            raise RuntimeError(f'Existing Goblin Core section not found in {filename}')
        path.write_text(text)
    source = site / 'sources' / args.source.name
    files = [source, Path(str(source) + '.asc'), site / 'goblin-core.html']
    for name, architecture, path in binaries:
        published = site / 'pool/main/g' / name / path.name
        files.append(published)
        retained.append({
            'name': name, 'format': 'deb', 'suite': args.suite,
            'distribution': SUITES[args.suite], 'architecture': architecture,
            'version': args.version, 'source': artifact(site, source)['url'],
            'artifacts': [artifact(site, published)],
            'verification': {'public_install': 'pass',
                'default_resp_tcp_and_uds': 'pass' if name == 'goblin-core' else 'not-applicable',
                'cmake_client_compile': 'pass' if name == 'goblin-core-cpp' else 'not-applicable'},
        })
    for filename in ['BUILD-INFO.json', 'installed-check.json', 'public-check.json',
                     'build.log', 'installed-test.log', 'public-test.log',
                     'build-native-arm.sh', 'validate-arm-install.sh']:
        path = stage / filename
        if path.exists():
            files.append(path)
    # Keep every other package's catalog record, including existing amd64 builds.
    assert all(entry in retained for entry in original_packages if key(entry) not in replacing)
    catalog['packages'] = retained
    new_artifacts = [artifact(site, path) for path in files]
    updated_paths = {entry['path'] for entry in new_artifacts}
    catalog['artifacts'] = [entry for entry in catalog.get('artifacts', [])
                            if not isinstance(entry, dict) or entry.get('path') not in updated_paths] + new_artifacts
    catalog['updated'] = datetime.date.today().isoformat()
    catalog['verified'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    temporary = site / 'packages.json.tmp'
    temporary.write_text(json.dumps(catalog, indent=2) + '\n')
    sign(temporary)
    temporary.replace(site / 'packages.json')
    Path(str(temporary) + '.asc').replace(site / 'packages.json.asc')
    (work / 'catalog-after-indexes.json').write_text((site / 'packages.json').read_text())
    print('Published ARM installation guide and catalog; other package records preserved', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', type=Path, required=True)
    parser.add_argument('--suite', choices=sorted(SUITES), required=True)
    parser.add_argument('--architecture', choices=['amd64', 'arm64'], required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--phase', choices=['validate', 'packages', 'indexes'], required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'\d+\.\d+\.\d+-\d+\+' + args.suite + r'\d+', args.version):
        parser.error('Version must contain the target suite and a distinct package revision')
    stage = args.stage.resolve()
    args.source = stage / f'goblin-core-{args.version}-{args.architecture}.tar.gz'
    if not args.source.is_file():
        parser.error('Versioned source archive is missing')
    if not (stage / 'docs/INSTALL.html').is_file():
        parser.error('Generated installation guide is missing')
    proof(stage, 'installed-check.json', args)
    binaries = packages(stage, args)
    if args.phase == 'validate':
        for name, architecture, path in binaries:
            print(json.dumps({'package': name, 'architecture': architecture,
                              'version': args.version, 'sha256': digest(path)}))
        return
    site = PUBLIC / 'apt.goblinreactor.com'
    work = ADMIN / f'goblin-core-{args.version}-{args.architecture}'
    work.mkdir(parents=True, exist_ok=True)
    with (ADMIN / 'apt-publish.lock').open('a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if args.phase == 'packages':
            publish_packages(stage, site, work, args, binaries)
        else:
            publish_indexes(stage, site, work, args, binaries)


if __name__ == '__main__':
    main()
