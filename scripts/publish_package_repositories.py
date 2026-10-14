#!/usr/bin/env python3
"""Publish validated packages on hail; PyPI is deliberately not part of this tool."""
import argparse
import datetime
import fcntl
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path

KEY='B7B61E8CB85A9270530364F23273CB62402FC793'
ADMIN=Path('/home/adam/.local/share/goblin-apt')
PUBLIC=Path('/mnt/distribution')
SUITES={'jammy':'Ubuntu 22.04 LTS','noble':'Ubuntu 24.04 LTS',
        'resolute':'Ubuntu 26.04 LTS','bookworm':'Debian 12','trixie':'Debian 13'}
NAMES=('goblin-core','goblin-core-cpp')

def run(*args,**kwargs):
    return subprocess.run([str(a) for a in args],check=True,**kwargs)

def tap_git(tap,*args):
    # This known public repository is on the distribution mount, which reports
    # a different owner. Scope Git's trust exception to this command and path.
    return run('git','-c',f'safe.directory={tap}','-C',tap,*args,
               capture_output=True,text=True).stdout.strip()

def sign(path):
    run('gpg','--homedir',ADMIN/'gnupg','--batch','--yes','--local-user',KEY,
        '--armor','--detach-sign','--output',str(path)+'.asc',path)
    run('gpgv','--keyring',PUBLIC/'apt.goblinreactor.com/goblinreactor-archive-keyring.gpg',str(path)+'.asc',path)

def artifact(site,path):
    rel=path.relative_to(site).as_posix()
    return {'path':rel,'filename':path.name,'url':f'https://{site.name}/{rel}',
            'size':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}

def backup(stage,site):
    target=stage/'backups'/site.name
    target.mkdir(parents=True,exist_ok=True)
    for filename in ['index.html','README.md','README.txt','packages.json','packages.json.asc']:
        source=site/filename
        if source.exists() and not (target/filename).exists(): shutil.copy2(source,target/filename)

def source(stage,site):
    target=site/'sources/goblin-core-0.10.6.tar.gz'
    target.parent.mkdir(exist_ok=True)
    shutil.copy2(stage/target.name,target)
    sign(target)

def publish_apt(stage,site):
    for suite in SUITES:
        assert (stage/suite/'installed.pass').exists(), f'{suite}: installed-package test required'
    backup(stage,site)
    admin_backup=stage/'backups/apt-admin'
    if not admin_backup.exists():
        shutil.copytree(ADMIN/'reprepro/db',admin_backup/'db')
        shutil.copytree(ADMIN/'reprepro/conf',admin_backup/'conf')
        shutil.copytree(site/'dists',admin_backup/'dists')
    env=dict(os.environ,GNUPGHOME=str(ADMIN/'gnupg'))
    for suite in SUITES:
        for deb in sorted((stage/suite).glob('*_0.10.6-2+*.deb')):
            run('reprepro','--keepunreferencedfiles','-b',ADMIN/'reprepro',
                '--export=never','includedeb',suite,deb,env=env)
    run('reprepro','--keepunreferencedfiles','-b',ADMIN/'reprepro','check',*SUITES,env=env)
    run('reprepro','--keepunreferencedfiles','-b',ADMIN/'reprepro','export',*SUITES,env=env)
    for suite in SUITES:
        run('gpgv','--keyring',site/'goblinreactor-archive-keyring.gpg',site/'dists'/suite/'InRelease')
    source(stage,site)

def publish_brew(stage,site):
    assert (stage/'brew/installed.pass').exists(), 'Homebrew installed-package tests required'
    backup(stage,site)
    for bottle in (stage/'brew').glob('*.bottle.tar.gz'):
        shutil.copy2(bottle,site/'bottles'/bottle.name.replace('--','-',1))
    source(stage,site)
    tap=site/'homebrew-tap.git'
    previous=tap_git(tap,'rev-parse','HEAD')
    head_backup=stage/'backups/brew-tap-head.txt'
    if not head_backup.exists(): head_backup.write_text(previous+'\n')
    tap_git(tap,'fetch',stage/'brew/homebrew-tap.bundle','HEAD')
    incoming=tap_git(tap,'rev-parse','FETCH_HEAD')
    tap_git(tap,'merge-base','--is-ancestor',previous,incoming)
    branch=tap_git(tap,'symbolic-ref','HEAD')
    tap_git(tap,'update-ref',branch,incoming,previous)
    tap_git(tap,'update-server-info')

def indexes(stage,site):
    is_apt=site.name.startswith('apt.')
    if is_apt:
        assert all((stage/s/'public.pass').exists() for s in SUITES), 'Public signed APT installs required'
    else:
        assert (stage/'brew/public.pass').exists(), 'Public Homebrew bottle install required'
    backup(stage,site)
    docs=site/'goblin-core-docs'
    shutil.copytree(stage/'docs',docs,dirs_exist_ok=True)
    page=(docs/'INSTALL.html').read_text()
    import re
    page=re.sub(r'(href|src)="(?!https?:|#|mailto:|data:)([^"\n]+)"',
                r'\1="goblin-core-docs/\2"',page)
    (site/'goblin-core.html').write_text(page)
    label='signed APT' if is_apt else 'Homebrew'
    commands=('sudo apt-get update\nsudo apt-get install goblin-core\nsudo systemctl enable --now goblin-core\n'
              '# Optional C++ client SDK:\nsudo apt-get install goblin-core-cpp') if is_apt else (
              'brew tap goblinreactor/tap https://brew.goblinreactor.com/homebrew-tap.git\n'
              'brew install goblinreactor/tap/goblin-core\nbrew services start goblinreactor/tap/goblin-core\n'
              '# Optional C++ client SDK:\nbrew install goblinreactor/tap/goblin-core-cpp')
    targets='amd64 on Ubuntu 22.04/24.04/26.04 and Debian 12/13' if is_apt else 'Apple Silicon on macOS 26 Tahoe'
    summary=(f'Goblin Core 0.10.6 is available through this {label} repository for {targets}. '
             'The goblin-core package installs the Redis-compatible server; goblin-core-cpp is a separate C++ client SDK. '
             'The service uses loopback TCP port 6379 and a Unix socket. SBE, rings and other high-performance transports are not enabled. '
             'Stop an existing Redis service first if it uses port 6379. Python clients are released separately; the matching PyPI release is pending approval.')
    block=f'\n<h3 id="goblin-core">goblin-core and goblin-core-cpp</h3>\n<p>{summary}</p>\n<p><a href="goblin-core.html">Install and configure Goblin Core</a> (includes repository setup and socket paths).</p>\n<pre><code>{commands}</code></pre>\n'
    index=(site/'index.html').read_text()
    if 'id="goblin-core"' not in index:
        assert '<h2 id="available-packages">Available packages</h2>' in index
        index=index.replace('<h2 id="available-packages">Available packages</h2>',
                            '<h2 id="available-packages">Available packages</h2>'+block,1)
        (site/'index.html').write_text(index)
    for filename in ['README.md','README.txt']:
        path=site/filename
        if not path.exists(): continue
        text=path.read_text()
        if '### goblin-core and goblin-core-cpp' not in text:
            text=text.replace('## Available packages','## Available packages\n\n### goblin-core and goblin-core-cpp\n\n'+summary+'\n\n[Installation instructions](https://'+site.name+'/goblin-core.html).\n\n```sh\n'+commands+'\n```\n',1)
            if is_apt: text=text.replace('Published Debian/Ubuntu binary packages: ', 'Published Debian/Ubuntu binary packages: **goblin-core**, **goblin-core-cpp**, ',1)
            path.write_text(text)
    catalog=json.loads((site/'packages.json').read_text())
    catalog['packages']=[p for p in catalog['packages'] if p['name'] not in NAMES]
    files=[site/'sources/goblin-core-0.10.6.tar.gz',site/'goblin-core.html']
    release=site/'releases/goblin-core-0.10.6'
    release.mkdir(parents=True,exist_ok=True)
    if is_apt:
        for filename in ['package_deb.py','build-linux.sh','test-public-apt.sh']:
            shutil.copy2(stage/filename,release/filename)
        for suite,distribution in SUITES.items():
            shutil.copy2(stage/f'{suite}-build.log',release/f'{suite}-build.log')
            shutil.copy2(stage/f'{suite}-public.log',release/f'{suite}-public.log')
            for name in NAMES:
                arch='amd64' if name=='goblin-core' else 'all'
                version=f'0.10.6-2+{suite}1'
                path=site/f'pool/main/g/{name}/{name}_{version}_{arch}.deb'
                files.append(path)
                catalog['packages'].append({'name':name,'format':'deb','suite':suite,
                    'distribution':distribution,'architecture':arch,'version':version,
                    'source':f'https://{site.name}/sources/goblin-core-0.10.6.tar.gz',
                    'artifacts':[artifact(site,path)],'verification':{'public_install':'pass',
                    'default_resp_tcp_and_uds':'pass' if name=='goblin-core' else 'not-applicable',
                    'cmake_client_compile':'pass' if name=='goblin-core-cpp' else 'not-applicable'}})
    else:
        tap=site/'homebrew-tap.git'
        catalog['tap_commit']=tap_git(tap,'rev-parse','HEAD')
        for name in NAMES:
            path=site/f'bottles/{name}-0.10.6.arm64_tahoe.bottle.tar.gz'
            files.append(path)
            data=json.loads((stage/'brew'/f'{name}--0.10.6.arm64_tahoe.bottle.json').read_text())
            deps=data[f'goblinreactor/tap/{name}']['bottle']['tags']['arm64_tahoe']['tab']['runtime_dependencies']
            catalog['packages'].append({'name':name,'formula':f'goblinreactor/tap/{name}',
                'format':'homebrew-bottle','version':'0.10.6','distribution':'macOS 26 Tahoe',
                'architecture':'arm64','platform_tag':'arm64_tahoe','bottle':artifact(site,path)['url'],
                'source':f'https://{site.name}/sources/goblin-core-0.10.6.tar.gz',
                'runtime_dependencies':deps,'artifacts':[artifact(site,path)],
                'verification':{'public_install':'pass','installed_formula_test':'pass'}})
        for path in (stage/'brew').glob('*.log'): shutil.copy2(path,release/path.name)
    files.extend(p for p in release.rglob('*') if p.is_file())
    new=[artifact(site,p) for p in files]
    replaced={a['path'] for a in new}
    catalog['artifacts']=[a for a in catalog.get('artifacts',[])
                          if not isinstance(a,dict) or a.get('path') not in replaced]+new
    catalog['updated']=datetime.date.today().isoformat()
    catalog['verified']=datetime.datetime.now(datetime.timezone.utc).isoformat()
    tmp=site/'packages.json.tmp'
    tmp.write_text(json.dumps(catalog,indent=2)+'\n')
    tmp.replace(site/'packages.json')
    sign(site/'packages.json')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage',type=Path,required=True)
    parser.add_argument('--phase',choices=['apt-packages','brew-packages','indexes'],required=True)
    args=parser.parse_args()
    sites=['apt.goblinreactor.com','brew.goblinreactor.com'] if args.phase=='indexes' else [args.phase.split('-')[0]+'.goblinreactor.com']
    for domain in sites:
        site=PUBLIC/domain
        with (ADMIN/(domain.split('.')[0]+'-publish.lock')).open('a') as lock:
            fcntl.flock(lock,fcntl.LOCK_EX)
            if args.phase=='apt-packages': publish_apt(args.stage,site)
            elif args.phase=='brew-packages': publish_brew(args.stage,site)
            else: indexes(args.stage,site)

if __name__=='__main__': main()
