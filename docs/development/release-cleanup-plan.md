# Historical release cleanup plan — approval required

No historical main-repository release or tag has been deleted. The initial inventory records 82 releases (63 public, 19 prereleases), 343 assets and 266 distinct server SHA-256 values. All 82 release tags resolve to exact source commits locally. Raw GitHub metadata, tag objects, asset IDs, URLs, sizes, hashes and resolved commits are preserved in `_preserved/workflow-migration-20261013/release-inventory.json`, with the original API responses alongside it. Git history/source references and all pre-existing local artifacts remain preserved.

## Replacement prerequisite

Before approving removal, verify the new current release's x86/x64 binary resources, exact master/source SHA, three small release assets, setup/updater fixtures, and pinned dependency resolution. The current setup and updater resolve required dependencies entirely from immutable VideoRenderer-Dependencies releases. The initial migration preserves existing current dependency bytes; architecture selection and every pinned asset/hash have been verified. No renderer update needs a companion runtime prerelease.

## What can be removed after approval

The following old release records can be removed from the main Releases page after the replacement is fully verified. Delete release-only custom `maxine-*` tags only after their source mappings have been preserved. Retain upstream version/beta milestone tags and meaningful development branches even if their release records are removed. The new verified release and the entire dependency repository are excluded.

This is a compatibility decision as well as cleanup: old restore scripts use moving latest URLs with legacy ZIP names, so they already require a one-time run of the current setup. Old bundled installers pin historical architecture-release URLs; deleting those runtime releases breaks fresh installations from those old setup ZIPs. Moving their binaries to a different repository cannot redirect hardcoded GitHub URLs. Existing installed runtimes/engine caches and installer backups are not removed by release cleanup.

## Unique obsolete assets and rollback

The inventory identifies two distinct historical sets of four architecture ZIP hashes, plus an older standalone Maxine runtime. Only the currently required verified set was migrated; retired dependency bytes are still present in their original releases/local diagnostics. Do not delete the retired unique dependency assets until choosing whether to archive those exact bytes in the dependency repository or explicitly retire their old installer/rollback use. Historical setup ZIPs have distinct container hashes and may contain older runtime/model snapshots; they were not blindly mirrored. Their metadata and source mappings alone do not reproduce every old binary ZIP byte-for-byte.

For the one-visible-release end state, approve removal of all obsolete release records after the required archival/compatibility decisions above. A less destructive first step is to remove duplicate current runtime prereleases while retaining retired unique dependency releases until their archival is checked. No automatic pruning is active. Any future retention policy needs explicit approval and must verify/promote the replacement before removing its predecessor.

## Concrete inventory

| Release ID | Tag | Kind | Exact source commit | Tag decision |
| --- | --- | --- | --- | --- |
| 411569972 | `maxine-rife-1322-1-bea5902b` | fork renderer | `bea5902b1a98488fce1cfc9df9480d388a6bc2af` | release-only tag candidate |
| 411569828 | `maxine-rife-1322-1-bea5902b-rife-runtime` | runtime dependency | `bea5902b1a98488fce1cfc9df9480d388a6bc2af` | release-only tag candidate |
| 410013238 | `maxine-rife-1321-1-185570c8` | fork renderer | `185570c8bfab81e2f6e31177ee12723d33221262` | release-only tag candidate |
| 410013142 | `maxine-rife-1321-1-185570c8-rife-runtime` | runtime dependency | `185570c8bfab81e2f6e31177ee12723d33221262` | release-only tag candidate |
| 408710320 | `maxine-rife-1318-1-de2f6de9` | fork renderer | `de2f6de9bdfaa1cda54c9a3c7fe8f84cfc41216b` | release-only tag candidate |
| 408703328 | `maxine-rife-1317-1-80f4fc95` | fork renderer | `80f4fc95ba288ef9c426e9a2706496ad74c60a32` | release-only tag candidate |
| 408710162 | `maxine-rife-1318-1-de2f6de9-rife-runtime` | runtime dependency | `de2f6de9bdfaa1cda54c9a3c7fe8f84cfc41216b` | release-only tag candidate |
| 408703197 | `maxine-rife-1317-1-80f4fc95-rife-runtime` | runtime dependency | `80f4fc95ba288ef9c426e9a2706496ad74c60a32` | release-only tag candidate |
| 408377538 | `maxine-rife-1315-1-250608d0` | fork renderer | `250608d03e3f0030423ee03924498a3999726231` | release-only tag candidate |
| 408377402 | `maxine-rife-1315-1-250608d0-rife-runtime` | runtime dependency | `250608d03e3f0030423ee03924498a3999726231` | release-only tag candidate |
| 406239597 | `maxine-rife-1312-1-b0d8f455` | fork renderer | `b0d8f455d6b2af5177f8f92bddfc06004e76719e` | release-only tag candidate |
| 405433355 | `maxine-rife-1311-1-edcd9bd9` | fork renderer | `edcd9bd955828d2d4a00ae39979046222871ba4a` | release-only tag candidate |
| 405263057 | `maxine-rife-1310-1-2d134082` | fork renderer | `2d1340821288cf6b2b10ce04bcaee0edc84dee01` | release-only tag candidate |
| 405261939 | `maxine-rife-1309-1-4d5ecadc` | fork renderer | `4d5ecadc49aba7cd410469e4e465a7b5b3f96e00` | release-only tag candidate |
| 406239476 | `maxine-rife-1312-1-b0d8f455-rife-runtime` | runtime dependency | `b0d8f455d6b2af5177f8f92bddfc06004e76719e` | release-only tag candidate |
| 405433228 | `maxine-rife-1311-1-edcd9bd9-rife-runtime` | runtime dependency | `edcd9bd955828d2d4a00ae39979046222871ba4a` | release-only tag candidate |
| 405262993 | `maxine-rife-1310-1-2d134082-rife-runtime` | runtime dependency | `2d1340821288cf6b2b10ce04bcaee0edc84dee01` | release-only tag candidate |
| 405261813 | `maxine-rife-1309-1-4d5ecadc-rife-runtime` | runtime dependency | `4d5ecadc49aba7cd410469e4e465a7b5b3f96e00` | release-only tag candidate |
| 403628105 | `maxine-rife-1308-1-5aae5bf2` | fork renderer | `5aae5bf21f0c6547395b65d8d491ecf6ebbf2476` | release-only tag candidate |
| 403352110 | `maxine-rife-1305-1-f193d6d6` | fork renderer | `f193d6d6d645873d0f8b31502a7ef36392afdc15` | release-only tag candidate |
| 403627953 | `maxine-rife-1308-1-5aae5bf2-rife-runtime` | runtime dependency | `5aae5bf21f0c6547395b65d8d491ecf6ebbf2476` | release-only tag candidate |
| 403352015 | `maxine-rife-1305-1-f193d6d6-rife-runtime` | runtime dependency | `f193d6d6d645873d0f8b31502a7ef36392afdc15` | release-only tag candidate |
| 402709163 | `maxine-rife-1304-1-cd0042e4` | fork renderer | `cd0042e46e28471ac2e38815451e319837d841ca` | release-only tag candidate |
| 402592228 | `maxine-rife-1300-1-cb6e6135` | fork renderer | `cb6e6135e8eeb0f1501e5b9e237843f6567d9aa7` | release-only tag candidate |
| 402709094 | `maxine-rife-1304-1-cd0042e4-rife-runtime` | runtime dependency | `cd0042e46e28471ac2e38815451e319837d841ca` | release-only tag candidate |
| 402592186 | `maxine-rife-1300-1-cb6e6135-rife-runtime` | runtime dependency | `cb6e6135e8eeb0f1501e5b9e237843f6567d9aa7` | release-only tag candidate |
| 399619667 | `maxine-rife-1299-1-c2c2177d` | fork renderer | `c2c2177d49fe44cbf472d00040f00db2e586ddfc` | release-only tag candidate |
| 399619568 | `maxine-rife-1299-1-c2c2177d-rife-runtime` | runtime dependency | `c2c2177d49fe44cbf472d00040f00db2e586ddfc` | release-only tag candidate |
| 393474645 | `maxine-rife-1294-1-4854a7d2` | fork renderer | `4854a7d285847cbbbbf50e7288207d0fafe30c4d` | release-only tag candidate |
| 393474157 | `maxine-rife-1293-1-b299a7e0` | fork renderer | `b299a7e0563be2a8ad760c2f278bda6baad974b7` | release-only tag candidate |
| 393474587 | `maxine-rife-1294-1-4854a7d2-rife-runtime` | runtime dependency | `4854a7d285847cbbbbf50e7288207d0fafe30c4d` | release-only tag candidate |
| 393474108 | `maxine-rife-1293-1-b299a7e0-rife-runtime` | runtime dependency | `b299a7e0563be2a8ad760c2f278bda6baad974b7` | release-only tag candidate |
| 392706034 | `maxine-rife-1288-1-43f34158` | fork renderer | `43f3415894d18518bd69e99e8eaab605ee04b5c1` | release-only tag candidate |
| 392705985 | `maxine-rife-1288-1-43f34158-rife-runtime` | runtime dependency | `43f3415894d18518bd69e99e8eaab605ee04b5c1` | release-only tag candidate |
| 391495390 | `rife-abi2-test-c58bf38` | upstream/source milestone | `c58bf38249b9f66a3920938d87dad158b6514487` | retain meaningful source tag |
| 391675489 | `maxine-rife-primaryctx-test-2278f92` | fork renderer | `2278f92ce788f2b0c6964ab855e1b71f7bf308a0` | release-only tag candidate |
| 386801982 | `maxine-1136-1-0895e03b` | fork renderer | `0895e03b0bd2ea7c225c962d9dc90ce1d2a689b6` | release-only tag candidate |
| 386801075 | `maxine-1135-1-d50741b9` | fork renderer | `d50741b9161a4326edf8ae7e2411815b1b2c74e7` | release-only tag candidate |
| 386800396 | `maxine-1134-1-4cabde54` | fork renderer | `4cabde54fe98159e75c9c11547eea3c5645702e3` | release-only tag candidate |
| 385160195 | `maxine-1133-1-75f29085` | fork renderer | `75f29085995b57aa83066d665a05e134b0766297` | release-only tag candidate |
| 385159133 | `maxine-1131-1-6f08b332` | fork renderer | `6f08b3324a1389f850c79cd176e405def4c0b2e3` | release-only tag candidate |
| 384705194 | `maxine-1129-1-94cec078` | fork renderer | `94cec0788d03524ba8900859e3ccd901b0525e2f` | release-only tag candidate |
| 384703542 | `maxine-1128-1-1e3c7ebd` | fork renderer | `1e3c7ebd5b065ff5c6e807ac38d30fdd2e795683` | release-only tag candidate |
| 384701263 | `maxine-1127-1-2b16bb2d` | fork renderer | `2b16bb2d7acdc065d38ec2b9e87bca35ac1d8feb` | release-only tag candidate |
| 383133446 | `maxine-1122-1-73ef32e7` | fork renderer | `73ef32e79ac09152b8cfa16ddea2b04303958d7f` | release-only tag candidate |
| 381956266 | `maxine-1121-1-24e3c7b2` | fork renderer | `24e3c7b2c8705aacd9e58564be9608b233f2826d` | release-only tag candidate |
| 379991756 | `maxine-1109-1-0d759202` | fork renderer | `0d759202bf101fe2cd23996fa944d1e02ba0b31a` | release-only tag candidate |
| 379199988 | `maxine-1104-1-cf7dbdc4` | fork renderer | `cf7dbdc46d2066e4a92ac6c0fb88338c591a8889` | release-only tag candidate |
| 379199420 | `maxine-1102-1-58671d4d` | fork renderer | `58671d4d589905aa41ada3e0066f00debe7ebbbe` | release-only tag candidate |
| 379199026 | `maxine-1101-1-ff16b45e` | fork renderer | `ff16b45ebaf593f348dcd027b78a41fb7e659248` | release-only tag candidate |
| 379197672 | `maxine-1098-1-641cd02b` | fork renderer | `641cd02bec051a84b6560d092bcc8b9eda79b0f1` | release-only tag candidate |
| 379092114 | `maxine-994-1-b45fe3ee` | fork renderer | `b45fe3eea0d1381f2e46a6c253077d2d67675386` | release-only tag candidate |
| 379049921 | `maxine-979-1-cf7eb036` | fork renderer | `cf7eb03630140743baf64aeed1f159655d91ef7e` | release-only tag candidate |
| 378959598 | `maxine-972-1-9f896d06` | fork renderer | `9f896d06605fb1e906a5843ec42042ed18bad083` | release-only tag candidate |
| 376085774 | `maxine-932-1-a194bb35` | fork renderer | `a194bb35406c16b5ee9469162f67e10d7149b445` | release-only tag candidate |
| 375297087 | `maxine-929-1-4131b15e` | fork renderer | `4131b15eaf21ebcdc76b60e26f28c4ac9681dd04` | release-only tag candidate |
| 373964585 | `maxine-928-1-f229eaa7` | fork renderer | `f229eaa7a28995b67896100adc7056fb831ff486` | release-only tag candidate |
| 373751550 | `maxine-927-1-d649cd76` | fork renderer | `d649cd765ba38419173651ea99c780860932850b` | release-only tag candidate |
| 373749198 | `maxine-925-1-5a1d6ee2` | fork renderer | `5a1d6ee2eb805c5d25e6c890ff5693ba63fb4738` | release-only tag candidate |
| 372151473 | `maxine-900-1-5fff8959` | fork renderer | `5fff89596aad93a81d4827ad8e70c67663e1c2a1` | release-only tag candidate |
| 371327260 | `maxine-899-1-88578dfe` | fork renderer | `88578dfe809f884d0cee64523ee69905d5772ebf` | release-only tag candidate |
| 371130426 | `maxine-898-1-f7026b57` | fork renderer | `f7026b578d61519e888fbfd492259e92e144a429` | release-only tag candidate |
| 370986175 | `maxine-897-1-6f297bc2` | fork renderer | `6f297bc24fd022659850f9faff56f90cd5a81878` | release-only tag candidate |
| 370985642 | `maxine-894-1-798f26a9` | fork renderer | `798f26a9df5be310cbeebdc66772e243ca12dc33` | release-only tag candidate |
| 370985238 | `maxine-892-1-9ba511ea` | fork renderer | `9ba511ea0e598dae74ae04af4a64fbf5b830c3bb` | release-only tag candidate |
| 370775966 | `maxine-891-1-d74abbd0` | fork renderer | `d74abbd011dab6bb5a13ee93bd14bf2b5c374ec6` | release-only tag candidate |
| 369311404 | `maxine-884-1-5bf15284` | fork renderer | `5bf15284486eee04a843d416bc36d20c9cbbaf09` | release-only tag candidate |
| 369309837 | `maxine-883-1-365baeb4` | fork renderer | `365baeb4d293bd66338024baf1ff94a7384df5e5` | release-only tag candidate |
| 369308897 | `maxine-882-1-9ab11eb2` | fork renderer | `9ab11eb2d193bc8890f765bb6efd5c4b92d939ff` | release-only tag candidate |
| 369307508 | `maxine-880-1-47d269ae` | fork renderer | `47d269ae20cda33e175351f320f768b11797b333` | release-only tag candidate |
| 365316575 | `maxine-337-1-7cb6dad2` | fork renderer | `7cb6dad2dc5e8d3886100b9dbf6e8f5cf4dde1fd` | release-only tag candidate |
| 365284645 | `maxine-322-1-2baf6f85` | fork renderer | `2baf6f85e3c655d42f2fda68449b49091f1e0046` | release-only tag candidate |
| 365238736 | `maxine-270-1-e77a3f98` | fork renderer | `e77a3f98660635ea1f99d1210e3b7d79e13b3605` | release-only tag candidate |
| 364964775 | `maxine-255-1-73a630c0` | fork renderer | `73a630c037d4a7e634e5b403d77ebc560a734ae6` | release-only tag candidate |
| 364915885 | `maxine-224-1-d5d27aee` | fork renderer | `d5d27aee0527e0cdc8df421babf025928c5670e5` | release-only tag candidate |
| 364656702 | `maxine-214-1-e8a7ac1f` | fork renderer | `e8a7ac1f5cac0dd635179bc582658011d0d2f9ed` | release-only tag candidate |
| 364643383 | `maxine-207-1-eeaa867a` | fork renderer | `eeaa867a93d44d37ccd905f05063a2adf254284e` | release-only tag candidate |
| 364606356 | `maxine-192-1-f1b7b1ab` | fork renderer | `f1b7b1ab7a69a47210a07253ca393d35599f39ce` | release-only tag candidate |
| 364378341 | `maxine-187-1-baa8fefa` | fork renderer | `baa8fefa018f89db1951b39bfc62bef8b3cb581e` | release-only tag candidate |
| 364246156 | `maxine-172-1-935519ce` | fork renderer | `935519ce30b53d88276cdccd9a3bd87da84eaa1d` | release-only tag candidate |
| 363474068 | `maxine-143-1-7f5730a2` | fork renderer | `7f5730a2b79d284e7da035732f92ebe3bb115c89` | release-only tag candidate |
| 363351078 | `maxine-142-1-1f78e116` | fork renderer | `1f78e116175343baa0fc4e0b1d7d471c9f9f45b8` | release-only tag candidate |
