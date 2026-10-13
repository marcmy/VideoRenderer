"""Content-addressed dependency recipes, bootstrap and manifest verification.

Standard library only. Renderer changes never change these component keys.
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REPO = 'marcmy/VideoRenderer-Dependencies'
SOURCE_REPO = 'marcmy/VideoRenderer'
BASELINE = 'bea5902b1a98488fce1cfc9df9480d388a6bc2af'
RECIPES = {
    'rife-runtime': {
        'recipeVersion': 1,
        'toolchain': {'cuda': '13.4.1', 'tensorrt': '11.3.0.99', 'architectures': ['sm75','sm86','sm89','sm120']},
        'paths': ['Source/RifeRuntimeApi.h','Source/RifeCudaOutputApi.h',
                  'tools/RifeTensorRTRuntime/CMakeLists.txt'],
        'codeDirectory': 'tools/RifeTensorRTRuntime',
        'extensions': ['.cpp','.h','.cu'],
    },
    'rife-models': {
        'recipeVersion': 1,
        'paths': ['tools/RifeTensorRTRuntime/prepare_model.py',
                  'tools/RifeTensorRTRuntime/prepare_feature_models.py',
                  'tools/RifeRuntimeInstaller/model-manifest.schema.json'],
    },
    'maxine': {'recipeVersion': 1, 'paths': ['tools/MaxineRuntimeInstaller/Build-MPCVRMaxineRuntime.py']},
}

def git(*args):
    return subprocess.check_output(['git','-C',str(ROOT),'-c',f'safe.directory={ROOT.as_posix()}',*args])

def digest(path):
    result = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''): result.update(block)
    return result.hexdigest()

def recipe(component):
    spec = RECIPES[component]
    paths = set(spec['paths'])
    if 'codeDirectory' in spec:
        paths |= {name for name in git('ls-files',spec['codeDirectory']).decode().splitlines()
                  if Path(name).suffix in spec['extensions']}
    inputs = {name: git('hash-object','--path='+name,name).decode().strip() for name in sorted(paths)}
    build_recipe = {}
    if component != 'maxine':
        workflow = ROOT / ('.github/workflows/rife-runtime.yml' if component == 'rife-runtime' else '.github/workflows/rife-model.yml')
        text = workflow.read_text()
        names = {'rife-runtime':['Download pinned CUDA 13.4.1 build components',
                                'Download TensorRT 11.3.0.99 for CUDA 13.4','Configure and build runtime'],
                 'rife-models':['Install model preparation dependencies',
                               'Download and verify pinned RIFE sources and license',
                               'Convert RIFE variants to mixed FP16','Prepare and verify reusable encoder stages']}[component]
        for name in names:
            match = re.search(r'(?m)^      - name: '+re.escape(name)+r'\n(.*?)(?=^      - |\Z)',text,re.S)
            if not match: raise ValueError('Missing pinned build recipe step: '+name)
            build_recipe[name] = match.group(1).strip()
        build_recipe['pins'] = sorted(re.findall(r'(?m)^      (?:RIFE_[A-Z0-9_]+|CUDA_(?:VERSION|COMPILER_VERSION|RUNTIME_VERSION|LICENSE_SHA256)|TENSORRT_(?:VERSION|EXPECTED_SHA256|SLA_SHA256)): .+$',text))
    content = {'component': component,'recipeVersion':spec['recipeVersion'], 'buildRecipe':build_recipe,
               'toolchain':spec.get('toolchain',{}),'inputs':inputs}
    fingerprint = hashlib.sha256(json.dumps(content,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    return dict(content,sourceFingerprint=fingerprint,
                releaseTag=f'mpcvr-deps-{component}-{fingerprint[:16]}')

def asset_url(tag, name):
    return f'https://github.com/{REPO}/releases/download/{tag}/{urllib.parse.quote(name)}'

def validate_manifest(manifest, expected=None):
    if manifest.get('schemaVersion') != 1: raise ValueError('Unsupported dependency manifest schema')
    if not re.fullmatch(r'[a-f0-9]{64}',manifest.get('sourceFingerprint','')): raise ValueError('Invalid source fingerprint')
    if expected and manifest['sourceFingerprint'] != expected: raise ValueError('Dependency source fingerprint mismatch')
    if manifest.get('component') not in RECIPES: raise ValueError('Unknown dependency component')
    for name, entry in manifest.get('files',{}).items():
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*',name): raise ValueError('Unsafe dependency filename')
        if not re.fullmatch(r'[a-f0-9]{64}',entry.get('sha256','')): raise ValueError('Invalid dependency checksum')
        if not isinstance(entry.get('bytes'),int) or entry['bytes'] <= 0: raise ValueError('Invalid dependency asset size')
        url = urllib.parse.urlparse(entry.get('url',''))
        if url.scheme != 'https' or url.netloc != 'github.com' or not url.path.startswith(f'/{REPO}/releases/download/'):
            raise ValueError('Dependency must use a pinned repository release URL')
        suffix = url.path.split('/releases/download/',1)[1].split('/')
        if len(suffix) != 2 or not re.fullmatch(r'mpcvr-deps-(?:rife-runtime|rife-models|maxine)-[a-f0-9]{16}',suffix[0]) or urllib.parse.unquote(suffix[1]) != name:
            raise ValueError('Dependency URL is not pinned to the named asset')
    required = {'rife-runtime':['MPCVR-RIFE-Common.zip','runtime-manifest.json','SHA256SUMS.txt',
                               'MPCVR-RIFE-sm75.zip','MPCVR-RIFE-sm86.zip','MPCVR-RIFE-sm89.zip','MPCVR-RIFE-sm120.zip'],
                'rife-models':['MPCVR-RIFE-Models.zip'], 'maxine':['MPCVR-Maxine-Runtime.zip']}
    if not set(required[manifest['component']]).issubset(manifest.get('files',{})): raise ValueError('Incomplete dependency component')
    return manifest

def file_entry(path, tag):
    path = Path(path)
    return {'sha256':digest(path),'bytes':path.stat().st_size,'url':asset_url(tag,path.name)}

def find_release(repo, tag):
    # GitHub's tag endpoint omits unpublished drafts, even when the list includes them.
    pages=json.loads(subprocess.check_output(['gh','api',f'repos/{repo}/releases','--paginate','--slurp']))
    matches=[r for page in pages for r in page if r['tag_name']==tag]
    published=[r for r in matches if not r['draft']]
    if len(published)>1: raise ValueError('Ambiguous published dependency versions: '+tag)
    if len(matches)>1:
        print('Duplicate unpublished drafts preserved pending deletion approval: '+','.join(str(r['id']) for r in matches if r['draft']))
    # Never overwrite/delete a draft. Validate the original complete asset set below.
    return published[0] if published else (min(matches,key=lambda r:r['id']) if matches else None)

def write_json(path, value):
    Path(path).parent.mkdir(parents=True,exist_ok=True)
    Path(path).write_bytes((json.dumps(value,indent=2)+'\n').encode('utf-8'))


def upload_missing(repo, release, files):
    """Resume a draft by ID. Existing bytes must match; never replace assets."""
    existing={a['name']:a for a in release['assets']}
    if set(existing)-set(files): raise ValueError('Unexpected release assets; preserve the draft for review')
    for name, path in files.items():
        if name in existing and (existing[name].get('digest')!='sha256:'+digest(path) or existing[name]['size']!=Path(path).stat().st_size):
            raise ValueError('Refuse to overwrite different release asset: '+name)
    missing=set(files)-set(existing)
    if missing and not release['draft']: raise ValueError('Published release is incomplete; do not mutate it')
    for name in sorted(missing):
        url=f'https://uploads.github.com/repos/{repo}/releases/{release["id"]}/assets?name='+urllib.parse.quote(name,safe='')
        subprocess.run(['gh','api','--method','POST',url,'-H','Content-Type: application/octet-stream',
                        '--input',str(files[name])],check=True,stdout=subprocess.DEVNULL)

def bootstrap(setup, checksums, output):
    setup, output = Path(setup), Path(output)
    expected = dict(line.split(maxsplit=1)[::-1] for line in Path(checksums).read_text().splitlines() if line.strip())
    if digest(setup) != expected[setup.name]: raise ValueError('Baseline setup checksum mismatch')
    output.mkdir(parents=True,exist_ok=True)
    selected = ['MPCVR-Maxine-Runtime.zip','MPCVR-RIFE-Common.zip','MPCVR-RIFE-Models.zip',
                'RIFE-runtime-manifest.json','RIFE-runtime-SHA256SUMS.txt','Maxine-SDK-License.pdf']
    with zipfile.ZipFile(setup) as archive:
        payload_sums = {name: sha for sha,name in (line.split(maxsplit=1)
            for line in archive.read('payload/PAYLOAD-SHA256SUMS.txt').decode().splitlines() if line.strip())}
        selected += [Path(name).name for name in archive.namelist()
                     if name.startswith('payload/') and Path(name).name.startswith(('CUDA-','TensorRT-'))]
        for name in sorted(set(selected)):
            target = output / name
            if target.exists():
                if digest(target) != payload_sums[name]: raise FileExistsError(f'Refuse overwrite: {target}')
                continue
            with archive.open('payload/'+name) as source, target.open('xb') as dest: shutil.copyfileobj(source,dest)
            if digest(target) != payload_sums[name]: raise ValueError(f'Baseline payload checksum mismatch: {name}')
    (output / 'runtime-manifest.json').write_bytes((output / 'RIFE-runtime-manifest.json').read_bytes())
    (output / 'SHA256SUMS.txt').write_bytes((output / 'RIFE-runtime-SHA256SUMS.txt').read_bytes())
    with zipfile.ZipFile(output / 'MPCVR-RIFE-Models.zip') as archive:
        (output / 'RIFE-model-license.txt').write_bytes(archive.read('LICENSES/RIFE-model-license.txt'))
        (output / 'model-manifest.json').write_bytes(archive.read('model-manifest.json'))
    arch_tag = 'maxine-rife-1322-1-bea5902b-rife-runtime'
    sums = {name: sha for sha,name in (line.split(maxsplit=1) for line in (output / 'SHA256SUMS.txt').read_text().splitlines() if line.strip())}
    components = {}
    for component in RECIPES:
        info = recipe(component)
        names = {'rife-runtime':['MPCVR-RIFE-Common.zip','runtime-manifest.json','SHA256SUMS.txt']
                 + [name for name in selected if name.startswith(('CUDA-','TensorRT-'))],
                 'rife-models':['MPCVR-RIFE-Models.zip','RIFE-model-license.txt','model-manifest.json'],
                 'maxine':['MPCVR-Maxine-Runtime.zip','Maxine-SDK-License.pdf']}[component]
        files = {name:file_entry(output/name,info['releaseTag']) for name in names}
        if component == 'rife-runtime':
            for architecture in info['toolchain']['architectures']:
                name=f'MPCVR-RIFE-{architecture}.zip'
                target=output/name
                if not target.exists():
                    urllib.request.urlretrieve(f'https://github.com/{SOURCE_REPO}/releases/download/{arch_tag}/{name}',target)
                if digest(target) != sums[name]: raise ValueError('Baseline architecture checksum mismatch: '+name)
                files[name]=file_entry(target,info['releaseTag'])
        compatibility={'platform':'windows-x64','gpuVendor':'NVIDIA','runtimeAbi':2,
                       'architectures':['sm75','sm86','sm89','sm120']}
        manifest = dict(info,schemaVersion=1,sourceRepository=SOURCE_REPO,sourceCommit=BASELINE,
                        provenance={'rendererRelease':'maxine-rife-1322-1-bea5902b',
                                    'runtimeRelease':arch_tag,'repackaged':False},
                        compatibility=compatibility,files=files)
        validate_manifest(manifest)
        manifest_path = output / (component+'.json')
        if manifest_path.exists():
            draft = json.loads(manifest_path.read_text())
            if draft != manifest:
                snapshot = output / (component+'.draft-'+draft['sourceFingerprint'][:16]+'.json')
                if not snapshot.exists(): snapshot.write_bytes(manifest_path.read_bytes())
        write_json(manifest_path,manifest)
        components[component] = {'sourceFingerprint':info['sourceFingerprint'],'releaseTag':info['releaseTag'],
            'manifestUrl':asset_url(info['releaseTag'],component+'.json'),
            'manifestSha256':digest(output/(component+'.json'))}
    write_json(output / 'dependency-bootstrap.json',{'schemaVersion':1,'baselineCommit':BASELINE,'components':components})
    print(json.dumps(components,indent=2))

def fetch_manifest(component, directory):
    info = recipe(component)
    response = subprocess.run(['gh','api',f'repos/{REPO}/releases/tags/'+info['releaseTag']],capture_output=True,text=True)
    if response.returncode:
        if '404' in response.stderr: return None
        raise RuntimeError(response.stderr)
    release = json.loads(response.stdout)
    if release['draft'] or not release.get('immutable'): raise ValueError('Dependency release must be published and immutable')
    name = component+'.json'
    asset = next(item for item in release['assets'] if item['name'] == name)
    expected = asset.get('digest','').removeprefix('sha256:')
    if not re.fullmatch(r'[a-f0-9]{64}',expected): raise ValueError('Published manifest lacks SHA-256 digest')
    target = Path(directory) / name
    target.parent.mkdir(parents=True,exist_ok=True)
    urllib.request.urlretrieve(asset['browser_download_url'],target)
    if digest(target) != expected: raise ValueError('Published manifest checksum mismatch')
    manifest = validate_manifest(json.loads(target.read_text()),info['sourceFingerprint'])
    return {'manifest':manifest,'manifestUrl':asset['browser_download_url'],'manifestSha256':expected}

def plan(directory, github_output=None):
    result = {'schemaVersion':1,'components':{}}
    for component in RECIPES:
        existing = fetch_manifest(component,directory)
        result['components'][component] = existing or {'manifest':None,'recipe':recipe(component)}
    write_json(Path(directory)/'plan.json',result)
    if github_output:
        with Path(github_output).open('a') as stream:
            for component, entry in result['components'].items():
                stream.write(component.replace('-','_')+'_build='+str(entry['manifest'] is None).lower()+'\n')
    print(json.dumps({name:'reuse' if value['manifest'] else 'build' for name,value in result['components'].items()}))

def publish_seed(directory, component=None):
    directory=Path(directory)
    for component in ([component] if component else RECIPES):
        manifest=validate_manifest(json.loads((directory/(component+'.json')).read_text()))
        tag=manifest['releaseTag']
        files={component+'.json':directory/(component+'.json')}
        for name, entry in manifest['files'].items():
            if f'/releases/download/{tag}/' in entry['url']:
                if digest(directory/name) != entry['sha256'] or (directory/name).stat().st_size!=entry['bytes']:
                    raise ValueError('Seed asset checksum or size changed: '+name)
                files[name]=directory/name
        release=find_release(REPO,tag)
        if not release:
            subprocess.run(['gh','release','create',tag,'--repo',REPO,'--target','main',
                '--title','Immutable '+component+' dependencies '+manifest['sourceFingerprint'][:16],
                '--notes',f'Immutable application dependencies for MPC Video Renderer. Source: {SOURCE_REPO}@{manifest["sourceCommit"]}. See {component}.json for original provenance, exact sizes, SHA-256 hashes and compatibility. NVIDIA components retain their own license terms and are not licensed under the renderer license. Never replace these assets.','--draft','--latest=false'],check=True)
            release=find_release(REPO,tag)
        upload_missing(REPO,release,files)
        release=find_release(REPO,tag)
        actual={a['name']:a.get('digest','').removeprefix('sha256:') for a in release['assets']}
        if actual!={name:digest(path) for name,path in files.items()}: raise ValueError('Uploaded dependency hashes differ')
        if release['draft']:
            subprocess.run(['gh','api','--method','PATCH',f'repos/{REPO}/releases/{release["id"]}','-F','draft=false','-f','make_latest=false'],check=True,stdout=subprocess.DEVNULL)
            release=find_release(REPO,tag)
        if not release.get('immutable'): raise ValueError('Enable native immutable releases before dependency publishing')
        print('Verified immutable component: '+tag)

def lock(directory, destination):
    components={}
    for component in RECIPES:
        path=Path(directory)/(component+'.json')
        manifest=validate_manifest(json.loads(path.read_text()),recipe(component)['sourceFingerprint'])
        components[component]={'releaseTag':manifest['releaseTag'], 'manifestSha256':digest(path),
                               'manifestUrl':asset_url(manifest['releaseTag'],path.name),
                               'manifest':manifest}
    write_json(destination,{'schemaVersion':1,'repository':REPO,'components':components})

def check_lock(value, check_recipes=True):
    if value.get('schemaVersion')!=1 or value.get('repository')!=REPO: raise ValueError('Invalid dependency lock')
    for component in RECIPES:
        pin=value['components'][component]
        manifest=validate_manifest(pin['manifest'],recipe(component)['sourceFingerprint'] if check_recipes else None)
        if pin['releaseTag']!=manifest['releaseTag'] or pin['manifestUrl']!=asset_url(pin['releaseTag'],component+'.json'):
            raise ValueError('Dependency manifest URL is not pinned')
        encoded=(json.dumps(manifest,indent=2)+'\n').encode()
        if hashlib.sha256(encoded).hexdigest()!=pin['manifestSha256']: raise ValueError('Embedded dependency manifest digest mismatch')
    return value

def check_published_lock(value, directory):
    check_lock(value)
    for component,pin in value['components'].items():
        existing=fetch_manifest(component,directory)
        if not existing or existing['manifestSha256']!=pin['manifestSha256'] or existing['manifest']!=pin['manifest']:
            raise ValueError('Pinned dependency is not the verified published component: '+component)
        release=json.loads(subprocess.check_output(['gh','api',f'repos/{REPO}/releases/tags/'+pin['releaseTag']]))
        if release['draft'] or not release.get('immutable'): raise ValueError('Dependency release is not published and immutable')
        releases={pin['releaseTag']:release}
        for name,entry in pin['manifest']['files'].items():
            asset_tag=entry['url'].split('/releases/download/',1)[1].split('/')[0]
            if asset_tag not in releases:
                releases[asset_tag]=json.loads(subprocess.check_output(['gh','api',f'repos/{REPO}/releases/tags/'+asset_tag]))
            if releases[asset_tag]['draft'] or not releases[asset_tag].get('immutable'): raise ValueError('Referenced dependency is not published and immutable')
            assets={a['name']:a for a in releases[asset_tag]['assets']}
            if name not in assets or assets[name]['size']!=entry['bytes'] or assets[name].get('digest')!='sha256:'+entry['sha256']:
                raise ValueError('Published dependency asset differs: '+name)
    print('All pinned dependency assets match the immutable registry; large downloads were unnecessary.')

def inventory(source, tags, output):
    # gh --paginate writes successive arrays, not one JSON document.
    raw=Path(source).read_text(encoding='utf-8-sig'); decoder=json.JSONDecoder(); releases=[]
    while raw.strip():
        page,end=decoder.raw_decode(raw.lstrip()); releases.extend(page); raw=raw.lstrip()[end:]
    refs=json.loads(Path(tags).read_text(encoding='utf-8-sig'))
    groups={}; result=[]
    for release in releases:
        tag=release['tag_name']; assets=[]
        ref=next((r for r in refs if r['ref']=='refs/tags/'+tag),None)
        for a in release['assets']:
            item={k:a.get(k) for k in ('id','name','size','digest','browser_download_url')}
            assets.append(item)
            if a.get('digest'): groups.setdefault(a['digest'],[]).append({'tag':tag,'name':a['name'],'id':a['id']})
        resolved=subprocess.run(['git','-C',str(ROOT),'rev-parse',tag+'^{commit}'],capture_output=True,text=True)
        result.append({'id':release['id'],'tag':tag,'sourceCommit':resolved.stdout.strip() if resolved.returncode==0 else None,'name':release['name'], 'prerelease':release['prerelease'],
                       'target':release['target_commitish'],'tagObject':ref['object'] if ref else None,'assets':assets})
    write_json(output,{'schemaVersion':1,'releases':result,'contentGroups':groups,
                       'deletionApproved':False,'rawMetadataFiles':[str(source),str(tags)]})
    print(f'Inventoried {len(result)} releases, {sum(len(r["assets"]) for r in result)} assets, {len(groups)} unique server hashes; nothing deleted.')

def prepare_component(component, directory):
    directory=Path(directory)
    if component=='rife-models':
        license_path=directory/'LICENSES/RIFE-model-license.txt'
        if license_path.exists(): shutil.copyfile(license_path,directory/'RIFE-model-license.txt')
    required={'rife-runtime':['MPCVR-RIFE-Common.zip','runtime-manifest.json','SHA256SUMS.txt']+
                             ['MPCVR-RIFE-'+a+'.zip' for a in ['sm75','sm86','sm89','sm120']],
              'rife-models':['MPCVR-RIFE-Models.zip','model-manifest.json','RIFE-model-license.txt'],
              'maxine':['MPCVR-Maxine-Runtime.zip','Maxine-SDK-License.pdf']}[component]
    required += [p.name for p in directory.iterdir() if p.is_file() and p.name.startswith(('CUDA-','TensorRT-'))]
    info=recipe(component)
    pages=json.loads(subprocess.check_output(['gh','api',f'repos/{REPO}/releases','--paginate','--slurp']))
    reusable={(a['name'],a['size'],a.get('digest')):a['browser_download_url']
              for page in pages for r in page if not r['draft'] and r.get('immutable') for a in r['assets']}
    files={name:file_entry(directory/name,info['releaseTag']) for name in required}
    for name,entry in files.items():
        key=(name,entry['bytes'],'sha256:'+entry['sha256'])
        if key in reusable:
            entry['url']=reusable[key]
            print('Reusing unchanged published dependency asset: '+name)
    manifest=dict(info,schemaVersion=1,sourceRepository=SOURCE_REPO,sourceCommit=git('rev-parse','HEAD').decode().strip(),
                  provenance={'operation':'validated dependency build','repackaged':True},
                  compatibility={'platform':'windows-x64','gpuVendor':'NVIDIA','runtimeAbi':2,'architectures':['sm75','sm86','sm89','sm120']},
                  files=files)
    validate_manifest(manifest)
    write_json(directory/(component+'.json'),manifest)

def main():
    parser=argparse.ArgumentParser()
    commands=parser.add_subparsers(dest='command',required=True)
    p=commands.add_parser('bootstrap');p.add_argument('--setup',required=True);p.add_argument('--checksums',required=True);p.add_argument('--output',required=True)
    p=commands.add_parser('publish-seed');p.add_argument('--directory',required=True);p.add_argument('--component',choices=RECIPES)
    p=commands.add_parser('lock');p.add_argument('--directory',required=True);p.add_argument('--output',required=True)
    p=commands.add_parser('inventory');p.add_argument('--releases',required=True);p.add_argument('--tags',required=True);p.add_argument('--output',required=True)
    p=commands.add_parser('check');p.add_argument('--lock',required=True);p.add_argument('--published',action='store_true');p.add_argument('--directory',default='_artifacts/dependency-verification')
    p=commands.add_parser('prepare-component');p.add_argument('--component',choices=RECIPES,required=True);p.add_argument('--directory',required=True)
    p=commands.add_parser('plan');p.add_argument('--directory',required=True);p.add_argument('--github-output')
    commands.add_parser('keys')
    args=parser.parse_args()
    if args.command=='bootstrap':bootstrap(args.setup,args.checksums,args.output)
    elif args.command=='publish-seed':publish_seed(args.directory,args.component)
    elif args.command=='lock':lock(args.directory,args.output)
    elif args.command=='inventory':inventory(args.releases,args.tags,args.output)
    elif args.command=='check':
        value=json.loads(Path(args.lock).read_text())
        if args.published:check_published_lock(value,args.directory)
        else:check_lock(value);print('Pinned dependency recipes and manifest hashes verified.')
    elif args.command=='prepare-component':prepare_component(args.component,args.directory)
    elif args.command=='plan':plan(args.directory,args.github_output)
    else:print(json.dumps({component:recipe(component) for component in RECIPES},indent=2))

if __name__=='__main__':main()
