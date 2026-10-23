# Development and releases

Root `Readme.md` and the allocation counters in `Source/resource.h` follow
upstream. The fork's GitHub landing guide is `.github/README.md`; packages
include it as `Readme.md` alongside `Readme-Upstream.md`. Private Maxine/RIFE
resource IDs live in `Source/ForkResource.h` in the 20000-32767 range, independent
of upstream's resource editor. Shared dialog layout changes still require
semantic review when both sides edit the same controls; sync must preserve the
conflict artifact and stop instead of guessing.

## One local checkout

Use `C:\Users\marcm\src\VideoRenderer`. The Codex project marks it Primary. A chat created by the old handoff can still have a different default cwd: specify this working directory explicitly. The older Documents checkout and synchronization worktrees retain their histories, dirty snapshots, private diagnostics and ignored artifacts. They are preserved historical locations, not routine development checkouts. See the [inventory](workspace-inventory-2026-10-13.md).

Start a task with `./dev.ps1 status`. It fetches origin and upstream, shows unpublished/divergent history and local edits, and identifies the build. `-Offline` inspects cached remote state. Origin must be `marcmy/VideoRenderer`; upstream must be `Aleksoid1978/VideoRenderer`. Never merge blindly into a dirty checkout. Use a temporary integration worktree only for actual divergence/conflicts, preserve its results, and bring the verified commit back to the primary checkout using ordinary Git.

## Small command set

Run from the primary directory in Windows PowerShell 5.1 or PowerShell 7:

| Command | Purpose |
| --- | --- |
| `./dev.ps1 status` | Fetch and inspect the three source states before edits. |
| `./dev.ps1 sync` | Fast-forward clean master from the fork, integrate pending upstream source and record its identity. Unknown conflicts remain reviewable. |
| `./dev.ps1 build` | Incremental local x64 build, with source identity in binary resources. |
| `./dev.ps1 build -Architecture both` | x86/x64 builds and resource/provenance verification. |
| `./dev.ps1 test` | CPU/WARP, ABI, scheduler, receive/lifecycle/cache, scene/precision, dialog, version, dependency and updater regressions. No CUDA or live-player tests. |
| `./dev.ps1 bundle` | Build a portable x64 test bundle under `_artifacts/<source-id>/x64`; existing different artifacts are not overwritten. Add `-Architecture both` to include the full lightweight setup. |
| `./dev.ps1 publish` | Validate clean committed master, push normally, and let CI publish the source revision. |
| `./dev.ps1 verify` | Download only the small public assets and verify their source, versions, labels, hashes and pins. |
| `./dev.ps1 dependencies -Component rife-runtime` | Explicitly approve the reviewed committed source/recipe in dependency workflow configuration, then separately publish it; also accepts `rife-models` or `maxine`. Experiments use branch-context builders. |

Visual Studio C++ tools and the pinned Git submodules are required. Initialize missing submodules with `git submodule update --init --recursive`. NASM is downloaded once with its pinned SHA-256 and cached under `_tools`. Build preparation restores Windows utility paths before and after developer-shell initialization and passes NASM's absolute path to MSBuild; it does not change the system/user PATH settings. Native build outputs reuse `_bin`. Tests do not install the renderer or evict TensorRT caches. The retained reusable installer CI fixtures additionally cover temporary installation, failure/rollback, architecture rejection and idempotence under both PowerShell hosts. The updater checksum test intentionally supplies an invalid hash and reports PASS only when the package is rejected before execution.

Routine confirmed changes can use normal commits on canonical master. Larger changes should use a review branch/PR, with CI passing before integration. No commit-tree/update-ref publishing, permanent clones, or mass source transplants are needed. After publication, fast-forward the primary checkout to the published master SHA, retain intentional edits, and run `verify`. GitHub Actions cannot update the desktop itself.

## RIFE throughput comparisons

Compare **GPU threads: 1** and **2** using the same clip segment, model, processing
size, precision, feature reuse, scene detector, presentation size and requested
rate. Run RIFE alone first, then repeat with identical Maxine settings. Keep other
GPU workloads idle and capture statistics without an OCR overlay disrupting playback.
Measure delivered FPS, late/stale-drop changes over the same interval, presentation
lateness and RIFE/Maxine timing p95. Record the actual adaptive target as well:
a changed cap or an already-satisfied request can conceal a throughput difference.
Alternate the order and repeat before choosing a default; inference-only synth-cap
does not measure the whole pipeline. These comparisons require explicit GPU/player
test authorization, separate from `dev.ps1 test`, which does not run CUDA inference.

Direct CUDA output lazily acquires a worker's pooled D3D output only if that route
is unavailable or fails. Existing cached/registered textures remain valid for
fallback; this change avoids unnecessary pool growth rather than reclaiming live
registrations. The runtime ABI tests inject CUDA, allocation and inference failures
without NVIDIA hardware. Maxine synchronization and prepare-ahead defaults are
unchanged; revisiting them requires new delivery/correctness evidence.

## Versions

The upstream component follows upstream's actual convention: `git rev-list --count <merged-upstream-SHA>`, with major/minor/build read from that upstream tree. `upstream.version.json` pins the actual merged SHA so moved remote refs do not relabel old builds. Sync writes the pin in the same merge commit. Manual integrations should run `tools/Dev/Version.ps1 -RecordUpstream <SHA>` before committing the merge.

The fork revision is `2844 + git rev-list --first-parent --count 250608d03e3f0030423ee03924498a3999726231..HEAD`. This preserves the verified `bea5902b` baseline as r2850. Each new published source commit, including an upstream integration, advances the fork revision. No-op syncs, reruns and packaging retries reuse the existing version. Shallow/incomplete histories fail instead of inventing a revision.

Public format: **MPC Video Renderer 0.10.9.2605 + Maxine/RIFE r2850**, tag **v0.10.9.2605-maxine-rife.2850**. Numeric Windows FileVersion remains **0.10.9.2850** for existing comparisons. ProductVersion exposes both components; standard resource Comments include the full fork and upstream SHAs. Portable/setup manifests include those SHAs, binary hashes, build flavor and source digest.

Local builds default to local flavor. Their product labels and bundle directory contain the commit and, when dirty, a digest of tracked/untracked source. `MPCVR_TEST_REVISION` remains supported only for identifiable local/test builds. Official flavor rejects dirty source or a test override. The upstream-compatible `build_mpcvr.cmd` also distinguishes local archives and refuses overwrites; `dev.ps1` is the recommended command.

## CI and publishing

`main.yml` builds and validates renderer source, verifies immutable dependency pins without large downloads, runs the retained installer fixtures, and packages only `MPCVR-Setup.zip`, `MPCVR-Portable.zip` and `SHA256SUMS.txt`. Setup includes the exact small renderer and scripts; large components resolve from its embedded lock. Updater and setup downloads use exact tags and verified content caches. The separate [dependency repository](dependency-repository.md) publishes only changed components and reuses unchanged asset hashes, including architecture packs across runtime revisions.

Release promotion is serialized and checks that the source is still fork master before publication and again before marking latest. Published assets are verified first; reruns reuse a matching release, partial drafts resume without asset replacement, and stale jobs never become latest. Publication does not delete the preceding release.

The upstream job performs a normal merge, both builds and regressions, then a guarded fast-forward push. It explicitly dispatches `main.yml` because a push using Actions' GITHUB_TOKEN does not itself start a normal push workflow. Unknown conflicts leave master untouched and save the original/upstream SHAs, unmerged Git stages and conflict diff as a downloadable CI artifact. It opens one tracking issue per upstream SHA, with a link to the failed sync run, for a manually resolved integration PR. It does not push the raw upstream branch: that can replay upstream workflow commits and GitHub rejects workflow updates from GITHUB_TOKEN. When upstream directly changes the fork's workflows, manual integration requires credentials with workflow-write permission. No-op synchronization does not build, bump or release.

Upstream implementations stay under their original filenames (`VideoRenderer.cpp`, `VideoRenderer.h`, `DX11VideoProcessor.h`). RIFE settings and playback overrides are kept in `RifeVideoRenderer.inl`, included by the renderer translation unit, with explicit declarations in the original headers. This preserves normal three-way merge context instead of making every upstream edit compete with a replacement wrapper. `tools/Dev/test_upstream_merge.py`, included in `dev.ps1 test`, exercises unrelated edits through Git's real merger and checks that all fork source survives; incompatible edits must still conflict. Keep future fork changes localized rather than adding automatic ours/theirs selection or source-specific conflict handlers.

Upstream synchronization uses `git merge -Xrenormalize` and a scoped LF text rule for `Source/VideoRenderer.cpp`. This lets Git compare CRLF upstream history with the fork's LF source without a whole-file conflict. Overlapping code edits still require review; runtime packages and license checksums are not normalized. The merge regression tests exercise unrelated and overlapping edits in an isolated Git fixture with mixed line endings.

Heavy TensorRT/CUDA and model jobs are manual/reusable dependency builders, not prerequisites for every renderer change. Comprehensive installer workflows are reusable checks called once by normal CI; the redundant parser-only and library-probe workflows remain manual historical diagnostics. The pinned Trivy/Syft supply-chain checks remain active.

## Compatibility and cleanup

Run the new setup once to update old installed restore scripts; old clients recognize only legacy filenames, and cannot be migrated remotely. Old bundled setup ZIPs also pin their old runtime-release URLs. Removing those releases breaks fresh installs from those old ZIPs. The new setup/updater requires no historical main-repository dependency release.

The initial [cleanup plan](release-cleanup-plan.md) was explicitly approved and completed on 2026-10-13: 85 obsolete release records and 83 release-only tags were removed after verification and preservation. Git history, protected source/milestone tags, dependency releases and local recovery inventories were retained. This fork also has explicitly approved `MPCVR_KEEP_ONE_RELEASE=true` retention enabled. The publisher verifies/promotes the replacement, verifies older releases in the new version format, saves their exact small packages/metadata in a 90-day recovery artifact, checks master/latest again, and removes only older ancestor releases and their release-only tags. Dependency releases, legacy tags, drafts, experiments and upstream milestones are excluded. Retention is disabled when the variable is absent; do not enable it on another repository without approval. Installed rollback backups remain intact; download CI recovery artifacts for permanent retention of removed package bytes.
