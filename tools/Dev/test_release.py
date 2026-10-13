"""Offline publication safety and package contracts; no GitHub writes or GPU work."""
import json
import os
import shutil
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch
import dependencies
import release


class PublicationTests(unittest.TestCase):
    def test_retention_is_disabled_without_explicit_policy(self):
        with patch.dict(os.environ,{},clear=True), patch.object(release,'gh') as call:
            release.retain_one({})
        call.assert_not_called()

    def test_retention_preserves_current_legacy_and_unreviewed_sources(self):
        current={'releaseTag':'v0.10.9.2605-maxine-rife.2854','commit':'new','revision':2854}
        older='v0.10.9.2605-maxine-rife.2853'
        entries=[{'id':2,'tag_name':current['releaseTag'],'draft':False,'prerelease':False},
                 {'id':1,'tag_name':older,'draft':False,'prerelease':False},
                 {'id':3,'tag_name':'maxine-rife-1322','draft':False,'prerelease':False},
                 {'id':4,'tag_name':'v0.10.9.2605-maxine-rife.2852','draft':True,'prerelease':False}]
        def response(*args):
            endpoint=args[1]
            if endpoint.endswith('/releases/latest'): return json.dumps({'id':2,'tag_name':current['releaseTag']})
            if endpoint.endswith('/releases'): return json.dumps([entries])
            if '/git/ref/tags/' in endpoint: return json.dumps({'ref':'refs/tags/'+older})
            if endpoint.endswith('/commits/master'): return json.dumps({'sha':'new'})
            raise AssertionError(endpoint)
        with tempfile.TemporaryDirectory(prefix='mpcvr-release-test-') as directory, \
             patch.dict(os.environ,{'MPCVR_KEEP_ONE_RELEASE':'true'}), patch.object(release,'ROOT',Path(directory)), \
             patch.object(release,'gh',side_effect=response), \
             patch.object(release,'published',return_value={'commit':'old','revision':2853}) as verify, \
             patch.object(release.subprocess,'run') as call:
            call.return_value.returncode=0
            release.retain_one(current)
            self.assertEqual(verify.call_count,1)
            deletes=[c.args[0] for c in call.call_args_list if 'DELETE' in c.args[0]]
            self.assertEqual(len(deletes),2)
            self.assertTrue(any(c[-1].endswith('/releases/1') for c in deletes))
            self.assertTrue((Path(directory)/'_artifacts/release-retention'/older/'recovery.json').exists())
        # A source advance during verification must prevent all destructive calls.
        def moved(*args):
            if args[1].endswith('/commits/master'): return '{"sha":"advanced"}'
            return response(*args)
        with tempfile.TemporaryDirectory(prefix='mpcvr-release-test-') as directory, \
             patch.dict(os.environ,{'MPCVR_KEEP_ONE_RELEASE':'true'}), patch.object(release,'ROOT',Path(directory)), \
             patch.object(release,'gh',side_effect=moved), \
             patch.object(release,'published',return_value={'commit':'old','revision':2853}), \
             patch.object(release.subprocess,'run') as call:
            call.return_value.returncode=0
            with self.assertRaisesRegex(ValueError,'Master advanced'): release.retain_one(current)
            self.assertFalse(any('DELETE' in c.args[0] for c in call.call_args_list))

    def test_creation_uses_api_response_even_when_list_is_not_updated(self):
        draft={'id':123,'draft':True,'assets':[]}
        with patch.object(dependencies.subprocess,'check_output',return_value=json.dumps(draft).encode()) as call:
            result=dependencies.create_draft('owner/repo','version','a'*40,'Title','Notes')
        self.assertEqual(result['id'],123)
        self.assertEqual(json.loads(call.call_args.kwargs['input'])['target_commitish'],'a'*40)
        self.assertIn('POST',call.call_args.args[0])

    def test_verification_waits_for_server_digest_without_replacing_assets(self):
        with tempfile.TemporaryDirectory(prefix='mpcvr-release-test-') as directory:
            path=Path(directory)/'a.zip'; path.write_bytes(b'verified')
            ready={'id':1,'assets':[{'name':'a.zip','digest':'sha256:'+dependencies.digest(path)}]}
            pending={'id':1,'assets':[{'name':'a.zip','digest':None}]}
            with patch.object(dependencies.subprocess,'check_output',side_effect=[json.dumps(pending).encode(),json.dumps(ready).encode()]), \
                 patch.object(dependencies.time,'sleep'):
                self.assertEqual(dependencies.verify_uploaded('owner/repo',1,{'a.zip':path}),ready)

    def test_partial_draft_resumes_by_id_without_replacing_verified_asset(self):
        with tempfile.TemporaryDirectory(prefix='mpcvr-release-test-') as directory:
            files={name:Path(directory)/name for name in ['existing.zip','missing.zip']}
            for name,path in files.items(): path.write_bytes(name.encode())
            draft={'id':123,'draft':True,'assets':[{'name':'existing.zip','size':files['existing.zip'].stat().st_size,
                    'digest':'sha256:'+dependencies.digest(files['existing.zip'])}]}
            with patch.object(dependencies.subprocess,'run') as call:
                dependencies.upload_missing('owner/repo',draft,files)
            self.assertEqual(call.call_count,1)
            self.assertIn('/releases/123/assets?name=missing.zip',call.call_args.args[0][4])
            self.assertIn('--input',call.call_args.args[0])

    def test_conflicting_or_published_incomplete_assets_fail_before_upload(self):
        with tempfile.TemporaryDirectory(prefix='mpcvr-release-test-') as directory:
            path=Path(directory)/'a.zip'; path.write_bytes(b'verified')
            for draft in [
                {'id':1,'draft':True,'assets':[{'name':'a.zip','size':8,'digest':'sha256:'+'0'*64}]},
                {'id':1,'draft':True,'assets':[{'name':'unknown.zip','size':8,'digest':'sha256:'+'0'*64}]},
                {'id':1,'draft':False,'assets':[]}]:
                with patch.object(dependencies.subprocess,'run') as call, self.assertRaises(ValueError):
                    dependencies.upload_missing('owner/repo',draft,{'a.zip':path})
                call.assert_not_called()

    def test_draft_lookup_survives_tag_endpoint_omission(self):
        pages=[[{'id':3,'tag_name':'version','draft':True},{'id':2,'tag_name':'version','draft':True}]]
        with patch.object(dependencies.subprocess,'check_output',return_value=json.dumps(pages).encode()):
            self.assertEqual(dependencies.find_release('owner/repo','version')['id'],2)
        pages[0].append({'id':4,'tag_name':'version','draft':False})
        with patch.object(dependencies.subprocess,'check_output',return_value=json.dumps(pages).encode()):
            self.assertEqual(dependencies.find_release('owner/repo','version')['id'],4)

    def test_outdated_source_never_creates_release(self):
        with patch.object(release,'verify_package',return_value={'commit':'old'}), \
             patch.object(release,'gh',return_value='{"sha":"new"}'), \
             patch.object(dependencies,'find_release') as find:
            release.publish('unused')
        find.assert_not_called()


class PackageTests(unittest.TestCase):
    def test_single_architecture_and_full_setup_integrity(self):
        with tempfile.TemporaryDirectory(prefix='mpcvr-release-test-') as directory:
            root=Path(directory)/'source'; root.mkdir()
            for name in ['distrib','tools/MaxineSetup','tools/MaxineRuntimeInstaller','tools/RifeRuntimeInstaller','tools/KLiteMaxineUpdater']:
                shutil.copytree(release.ROOT/name,root/name,ignore=shutil.ignore_patterns('__pycache__','*.exe','*.zip'))
            for name in ['Readme.md','history.txt','LICENSE.txt','dependencies.lock.json']:
                shutil.copyfile(release.ROOT/name,root/name)
            for arch,name in [('x86','MpcVideoRenderer.ax'),('x64','MpcVideoRenderer64.ax')]:
                target=root/f'_bin/Filter_{arch}'; target.mkdir(parents=True)
                (target/name).write_bytes(('fixture '+arch).encode())
            version={'commit':'a'*40,'sourceDigest':'b'*64,'flavor':'local','dirty':False}
            version_path=root/'version.json'; version_path.write_text(json.dumps(version))
            with patch.object(release,'ROOT',root), \
                 patch.object(dependencies,'git',return_value=('a'*40).encode()), \
                 patch.object(dependencies,'check_lock'):
                # Source-version query is isolated; fixture binaries never execute.
                with patch.object(release.subprocess,'check_output',return_value=json.dumps(version).encode()):
                    release.package(version_path,'x64',Path(directory)/'x64')
                    release.package(version_path,'both',Path(directory)/'both')
            single=Path(directory)/'x64'
            self.assertFalse((single/'MPCVR-Setup.zip').exists())
            with zipfile.ZipFile(single/'MPCVR-Portable.zip') as archive:
                self.assertIn('Install_MPCVR_64.cmd',archive.namelist())
                self.assertNotIn('Install_MPCVR_32.cmd',archive.namelist())
            full=Path(directory)/'both'
            self.assertEqual(set(p.name for p in full.iterdir()),set(release.ASSETS))
            release.verify_package(full,'a'*40)
            with self.assertRaises(ValueError): release.verify_package(single,release=True)
            # An altered embedded setup payload must fail even if its outer ZIP is rehashed.
            with zipfile.ZipFile(full/'MPCVR-Setup.zip') as archive:
                entries={name:archive.read(name) for name in archive.namelist()}
            entries['payload/DependencyDownloads.psm1']+=b'changed'
            (full/'MPCVR-Setup.zip').write_bytes(release.archive_bytes(entries))
            (full/'SHA256SUMS.txt').write_text(''.join(dependencies.digest(full/n)+'  '+n+'\n' for n in release.ASSETS[:2]))
            with self.assertRaisesRegex(ValueError,'payload checksum'): release.verify_package(full)

    def test_existing_artifact_is_never_overwritten(self):
        with tempfile.TemporaryDirectory(prefix='mpcvr-release-test-') as directory:
            target=Path(directory)/'artifact.zip'
            release.write_immutable(target,b'first')
            release.write_immutable(target,b'first')
            with self.assertRaises(FileExistsError): release.write_immutable(target,b'second')
            self.assertEqual(target.read_bytes(),b'first')


if __name__=='__main__': unittest.main()
