"""Small renderer packages and guarded publication; no dependency rebuilding."""
import argparse
import hashlib
import io
import json
import os
import re
import subprocess
import urllib.request
import zipfile
from pathlib import Path
import dependencies

ROOT = Path(__file__).resolve().parents[2]
REPO = 'marcmy/VideoRenderer'
ASSETS = ['MPCVR-Setup.zip', 'MPCVR-Portable.zip', 'SHA256SUMS.txt']


def gh(*args):
    return subprocess.check_output(['gh', *args], text=True)


def write_immutable(path, data):
    if path.exists() and path.read_bytes() != data:
        raise FileExistsError('Different artifact already exists; choose a new source/output directory: '+str(path))
    if not path.exists(): path.write_bytes(data)


def archive_bytes(files):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, data in sorted(files.items()):
            entry = zipfile.ZipInfo(name, date_time=(1980,1,1,0,0,0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o644 << 16
            archive.writestr(entry, data)
    return buffer.getvalue()


def package(version_path, architecture, output):
    version = json.loads(Path(version_path).read_text(encoding='utf-8-sig'))
    current = dependencies.git('rev-parse','HEAD').decode().strip()
    if version['commit'] != current: raise ValueError('Build metadata is from another source commit')
    if os.name=='nt':
        environment=os.environ.copy()
        environment['PSModulePath']=os.path.join(os.environ['SystemRoot'],'System32/WindowsPowerShell/v1.0/Modules')
        current_version=json.loads(subprocess.check_output(['powershell.exe','-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',str(ROOT/'tools/Dev/Version.ps1'),'-Flavor',version['flavor']],env=environment))
        if current_version['sourceDigest']!=version['sourceDigest']: raise ValueError('Source changed after compilation; rebuild before packaging')
    lock = json.loads((ROOT/'dependencies.lock.json').read_text())
    dependencies.check_lock(lock)
    output = Path(output); output.mkdir(parents=True,exist_ok=True)
    arches = ['x86','x64'] if architecture == 'both' else [architecture]
    files = {}
    for arch in arches:
        name = 'MpcVideoRenderer64.ax' if arch == 'x64' else 'MpcVideoRenderer.ax'
        files[name] = (ROOT/f'_bin/Filter_{arch}'/name).read_bytes()
    for path in (ROOT/'distrib').glob('*.cmd'):
        if architecture != 'both' and path.stem.endswith('_'+('32' if architecture == 'x64' else '64')): continue
        files[path.name] = path.read_bytes()
    for name in ['Readme.md','history.txt','LICENSE.txt','dependencies.lock.json']:
        files[name] = (ROOT/name).read_bytes()
    files['Readme-Upstream.md'] = files['Readme.md']
    files['Readme.md'] = (ROOT/'.github/README.md').read_bytes()
    version['architectures'] = arches
    version['binaryHashes'] = {name:hashlib.sha256(data).hexdigest() for name,data in files.items() if name.endswith('.ax')}
    files['source-version.json'] = (json.dumps(version,indent=2)+'\n').encode()
    portable = archive_bytes(files)
    write_immutable(output/'MPCVR-Portable.zip', portable)
    if architecture != 'both':
        if version['flavor'] == 'release': raise ValueError('Public releases require both architectures')
        write_immutable(output/'SHA256SUMS.txt', (dependencies.digest(output/'MPCVR-Portable.zip')+'  MPCVR-Portable.zip\n').encode())
        verify_package(output, current)
        print(json.dumps({name:(output/name).stat().st_size for name in ['MPCVR-Portable.zip','SHA256SUMS.txt']}))
        return
    # Setup includes only the small, exact renderer. All large dependencies are pinned downloads.
    setup = {name:(ROOT/'tools/MaxineSetup'/name).read_bytes() for name in ['Install-MPCVR-Maxine-RIFE.cmd','Install-MPCVR-Maxine-RIFE.ps1','README.md']}
    payload = {'MpcVideoRenderer-Maxine-RIFE.zip':portable,
               'dependencies.lock.json':(ROOT/'dependencies.lock.json').read_bytes(),
               'source-version.json':files['source-version.json'],
               'DependencyDownloads.psm1':(ROOT/'tools/MaxineSetup/DependencyDownloads.psm1').read_bytes(),
               'THIRD-PARTY-NOTICES.txt':(ROOT/'tools/MaxineSetup/THIRD-PARTY-NOTICES.txt').read_bytes()}
    sources = {
        'MaxineRuntimeInstaller':['Install-MPCVRMaxineRuntime.ps1'],
        'RifeRuntimeInstaller':['Install-MPCVRRifeRuntime.ps1','Test-MPCVRRifePreflight.ps1','RifeRuntimeManifest.psm1'],
        'KLiteMaxineUpdater':['Install-KLiteMPCVRUpdater.ps1','Update-KLiteMPCVR.ps1','KLiteRendererInstall.psm1'],
    }
    for directory,names in sources.items():
        for name in names: payload[name]=(ROOT/'tools'/directory/name).read_bytes()
    payload['PAYLOAD-SHA256SUMS.txt'] = ''.join(hashlib.sha256(data).hexdigest()+'  '+name+'\n' for name,data in sorted(payload.items())).encode()
    setup.update({'payload/'+name:data for name,data in payload.items()})
    write_immutable(output/'MPCVR-Setup.zip',archive_bytes(setup))
    sums=''.join(dependencies.digest(output/name)+'  '+name+'\n' for name in ASSETS[:2])
    write_immutable(output/'SHA256SUMS.txt',sums.encode())
    verify_package(output, current, release=version['flavor']=='release')
    print(json.dumps({name:(output/name).stat().st_size for name in ASSETS}))


def verify_package(directory, expected_commit=None, release=False):
    directory=Path(directory)
    sums={name:sha for sha,name in (line.split(maxsplit=1) for line in (directory/'SHA256SUMS.txt').read_text().splitlines())}
    if set(sums) != set(ASSETS[:2]) and (release or set(sums) != {'MPCVR-Portable.zip'}): raise ValueError('Unexpected renderer release assets')
    for name,sha in sums.items():
        if dependencies.digest(directory/name)!=sha: raise ValueError('Renderer package checksum mismatch: '+name)
    with zipfile.ZipFile(directory/'MPCVR-Portable.zip') as archive:
        version=json.loads(archive.read('source-version.json'))
        if expected_commit and version['commit']!=expected_commit: raise ValueError('Published source commit mismatch')
        if release and (version['flavor']!='release' or version['dirty'] or version['architectures']!=['x86','x64']): raise ValueError('Public release must contain both clean official binaries')
        required={'MpcVideoRenderer64.ax' if a=='x64' else 'MpcVideoRenderer.ax' for a in version['architectures']}
        if set(version['architectures'])-{'x86','x64'} or not required or set(version['binaryHashes'])!=required:
            raise ValueError('Binary set disagrees with the architecture manifest')
        for name,sha in version['binaryHashes'].items():
            if hashlib.sha256(archive.read(name)).hexdigest()!=sha: raise ValueError('Binary provenance mismatch')
        lock=json.loads(archive.read('dependencies.lock.json'))
        dependencies.check_lock(lock,check_recipes=False)
    if 'MPCVR-Setup.zip' not in sums:
        if version['architectures']==['x86','x64']: raise ValueError('Full bundles require setup')
        return version
    with zipfile.ZipFile(directory/'MPCVR-Setup.zip') as archive:
        if archive.read('payload/MpcVideoRenderer-Maxine-RIFE.zip')!=(directory/'MPCVR-Portable.zip').read_bytes(): raise ValueError('Setup renderer does not match portable')
        if json.loads(archive.read('payload/dependencies.lock.json'))!=lock: raise ValueError('Setup pins do not match portable')
        sums={name:sha for sha,name in (line.split(maxsplit=1) for line in archive.read('payload/PAYLOAD-SHA256SUMS.txt').decode().splitlines())}
        for name,sha in sums.items():
            if hashlib.sha256(archive.read('payload/'+name)).hexdigest()!=sha: raise ValueError('Setup payload checksum mismatch')
        forbidden=['payload/MPCVR-Maxine-Runtime.zip','payload/MPCVR-RIFE-Common.zip','payload/MPCVR-RIFE-Models.zip']
        if any(name in archive.namelist() for name in forbidden): raise ValueError('Normal setup must not embed large dependencies')
    return version


def published(tag=None, expected_commit=None, directory=None):
    release=json.loads(gh('api',f'repos/{REPO}/releases/'+('tags/'+tag if tag else 'latest')))
    target=Path(directory) if directory else ROOT/'_artifacts/published'/release['tag_name']
    target.mkdir(parents=True,exist_ok=True)
    assets={a['name']:a for a in release['assets']}
    if set(assets)!=set(ASSETS): raise ValueError('Public release has unexpected or missing assets')
    for name,entry in assets.items():
        path=target/name
        sha=entry.get('digest','').removeprefix('sha256:')
        if not path.exists() or dependencies.digest(path)!=sha: urllib.request.urlretrieve(entry['browser_download_url'],path)
        if dependencies.digest(path)!=sha: raise ValueError('Server asset checksum mismatch')
    version=verify_package(target,expected_commit,release=True)
    if version['releaseTag']!=release['tag_name'] or version['releaseName']!=release['name']: raise ValueError('Release labels disagree with binary metadata')
    resolved=json.loads(gh('api',f'repos/{REPO}/commits/'+release['tag_name']))['sha']
    if resolved!=version['commit']: raise ValueError('Release tag points at different source')
    print(json.dumps({'release':release['html_url'],'commit':version['commit'],'version':version['productVersion'],'assets':{n:a['size'] for n,a in assets.items()}},indent=2))
    return version


def retain_one(version):
    """Explicitly approved policy only; never applies to legacy or dependency tags."""
    if os.environ.get('MPCVR_KEEP_ONE_RELEASE')!='true': return
    latest=json.loads(gh('api',f'repos/{REPO}/releases/latest'))
    if latest['tag_name']!=version['releaseTag']: raise ValueError('Replacement is not the verified latest release')
    pages=json.loads(gh('api',f'repos/{REPO}/releases','--paginate','--slurp'))
    for entry in [r for page in pages for r in page]:
        tag=entry['tag_name']
        if tag==version['releaseTag'] or entry['draft'] or entry['prerelease'] or not re.fullmatch(r'v\d+\.\d+\.\d+\.\d+-maxine-rife\.\d+',tag): continue
        archive=ROOT/'_artifacts/release-retention'/tag
        previous=published(tag,directory=archive)
        if previous['revision']>=version['revision']: raise ValueError('Ambiguous release order; preserve both versions for review')
        # Preserve non-ancestor sources rather than removing experimental history.
        ancestry=subprocess.run(['git','-C',str(ROOT),'merge-base','--is-ancestor',previous['commit'],version['commit']],capture_output=True)
        if ancestry.returncode: continue
        ref=json.loads(gh('api',f'repos/{REPO}/git/ref/tags/'+tag))
        dependencies.write_json(archive/'recovery.json',{'replacement':version,'release':entry,'tagRef':ref,'previousVersion':previous})
        if json.loads(gh('api',f'repos/{REPO}/commits/master'))['sha']!=version['commit']:
            raise ValueError('Master advanced; keep previous releases')
        if json.loads(gh('api',f'repos/{REPO}/releases/latest'))['id']!=latest['id']:
            raise ValueError('Latest changed; keep previous releases')
        subprocess.run(['gh','api','--method','DELETE',f'repos/{REPO}/releases/{entry["id"]}'],check=True)
        subprocess.run(['gh','api','--method','DELETE',f'repos/{REPO}/git/refs/tags/'+tag],check=True)
        print('Approved retention removed the previous verified renderer release: '+tag)


def publish(directory):
    version=verify_package(directory, release=True)
    current=json.loads(gh('api',f'repos/{REPO}/commits/master'))['sha']
    if current!=version['commit']:
        print('Source has advanced; this run will not publish an outdated release.'); return
    tag=version['releaseTag']
    release=dependencies.find_release(REPO,tag)
    if release:
        if not release['draft']:
            published(tag, version['commit'])
            if json.loads(gh('api',f'repos/{REPO}/commits/master'))['sha']==version['commit']:
                subprocess.run(['gh','release','edit',tag,'--repo',REPO,'--latest'],check=True)
                retain_one(version)
            print('Existing verified source release reused; no version change or asset overwrite.'); return
    else:
        notes=(f'**Recommended: MPCVR-Setup.zip** — installs the renderer and restore/update shortcuts, detects the NVIDIA GPU, and fetches only checksum-verified, pinned dependencies.\n\n'
               f'**MPCVR-Portable.zip** — x86/x64 renderer binaries for manual installation. Dependencies remain separate.\n\n'
               f'Build: {version["productVersion"]}; numeric Windows FileVersion: {version["version"]}.\n'
               f'Fork source: `{version["commit"]}`. Merged upstream: `{version["upstreamCommit"]}`.\n\n'
               'Immutable runtime/model packages: https://github.com/marcmy/VideoRenderer-Dependencies/releases. Exact tags, sizes, hashes, licenses and compatibility are embedded in dependencies.lock.json. No moving dependency URLs.\n\n'
               '**Upgrade note:** updater shortcuts installed before this release recognize only the old ZIP names. Run this setup once to update those scripts. Old offline setups remain tied to their historical runtime URLs.\n\n'
               'Verify downloads with SHA256SUMS.txt. NVIDIA runtime components retain their separate license terms. Previous releases are preserved unless explicitly approved retention is enabled; its recovery artifacts remain available for 90 days.\n')
        release=dependencies.create_draft(REPO,tag,version['commit'],version['releaseName'],notes)
    dependencies.upload_missing(REPO, release, {name:Path(directory)/name for name in ASSETS})
    # Verify server digests before publishing, and confirm source is still current.
    release=dependencies.verify_uploaded(REPO,release['id'],{name:Path(directory)/name for name in ASSETS})
    if json.loads(gh('api',f'repos/{REPO}/commits/master'))['sha']!=version['commit']:
        print('Source advanced during upload; verified draft retained, not promoted.'); return
    subprocess.run(['gh','api','--method','PATCH',f'repos/{REPO}/releases/{release["id"]}','-F','draft=false','-f','make_latest=false'],check=True,stdout=subprocess.DEVNULL)
    published(tag,version['commit'])
    if json.loads(gh('api',f'repos/{REPO}/commits/master'))['sha']==version['commit']:
        subprocess.run(['gh','release','edit',tag,'--repo',REPO,'--latest'],check=True)
        retain_one(version)


if __name__=='__main__':
    parser=argparse.ArgumentParser(); commands=parser.add_subparsers(dest='command',required=True)
    p=commands.add_parser('package');p.add_argument('--version',required=True);p.add_argument('--architecture',choices=['x86','x64','both'],default='both');p.add_argument('--output',required=True)
    p=commands.add_parser('publish');p.add_argument('--directory',required=True)
    p=commands.add_parser('verify');p.add_argument('--directory',required=True);p.add_argument('--commit');p.add_argument('--release',action='store_true')
    p=commands.add_parser('verify-published');p.add_argument('--tag');p.add_argument('--commit');p.add_argument('--directory')
    args=parser.parse_args()
    if args.command=='package':package(args.version,args.architecture,args.output)
    elif args.command=='publish':publish(args.directory)
    elif args.command=='verify':print(json.dumps(verify_package(args.directory,args.commit,args.release),indent=2))
    else:published(args.tag,args.commit,args.directory)
