# Repository Agent Instructions

## Authoritative checkout and preservation

- On marcm's desktop, routine development belongs in `C:\Users\marcm\src\VideoRenderer`.
  This is the Codex project's primary checkout. The Documents/Codex handoff and
  existing `videorenderer-*` worktrees are preserved historical work, not defaults.
- Adding a directory to a Codex project does not migrate an existing chat's cwd.
  Check the actual directory and Git common directory at task start. Pass the
  authoritative directory explicitly to shell tools; use `git -C` for other repos.
- If the current session cannot read/write the primary checkout, stop before
  implementation and prepare a verified handoff for a new local project session.
  Do not create another persistent clone to bypass the mismatch.
- Preserve all committed, staged, unstaged, untracked and ignored work. Equal HEAD
  commits do not establish equal working trees. Never reset, clean, force-push,
  delete historical worktrees/builds, or overwrite experimental artifacts.
- `_preserved/workflow-migration-20261013` contains inventory, Git bundles, source
  hashes and historical staged/working snapshots. The originals remain intact.
  The `preserved-handoff/*` references retain the old branch and stash histories.
- Temporary integration worktrees are appropriate for actual divergence or merge
  conflicts. Prefer direct development in the primary checkout otherwise.
- Never modify the installed MPC-HC renderer, clear user engine caches, inspect
  the live player, or run GPU/player tests unless the user explicitly requests it.

## Normal development lifecycle

- Start with `./dev.ps1 status` (fetches unless `-Offline`): verify origin is marcmy/VideoRenderer,
  upstream is Aleksoid1978/VideoRenderer, and inspect divergence and all local work.
- Fast-forward clean local source from origin when safe. Never merge blindly into
  dirty source. Preserve intentional edits before integration, then verify the
  resulting files and transfer the reviewed result back to the primary checkout.
- Normal implementation tasks include relevant tests, reviewable commits,
  publication and local/remote verification, unless the user explicitly requests
  a local-only experiment or draft. Do not publish speculative or unvalidated work.
- Use ordinary `git add`, `commit`, `merge`, and fast-forward pushes. Do not use
  commit-tree/update-ref or bulk source transplantation as the routine mechanism.
- Small confirmed changes need no PR solely for ceremony. Substantial workflow
  changes or unknown upstream conflicts should be reviewable PRs with passing CI.
- After publication, verify the exact remote source SHA, compiled binary version,
  pinned dependencies, checksums and release metadata. Keep the authoritative
  local checkout aligned, preserving intentional uncommitted work.
- Source/build/installer checks and live GPU/playback validation are different
  evidence. Report what actually ran and any remaining runtime confirmation.

## Version and delivery contracts

- The compiled renderer revision is authoritative. Release title/tag/manifest
  must agree with it; Actions run numbers are execution IDs, not software versions.
- Keep upstream major/minor/build compatibility and the fork's first-parent
  revision base. A synchronization with no source change must not publish a release.
- Public names use `upstream.version.json`'s actual merged upstream history count
  plus the fork revision: `v0.10.9.2605-maxine-rife.2850`, for example. Record the
  upstream SHA in the same integration commit. Never use history.txt as its revision.
- Local builds default to local flavor. Official release builds require clean,
  committed source and both x86/x64 binaries. Preserve numeric Windows FileVersion
  compatibility while exposing both components and exact SHAs in product metadata.
- Local/test builds include commit and dirty-source fingerprints in visible
  product metadata, provenance manifests and unique artifact names. Never reuse
  an output directory for a different source identity.
- Reuse unchanged, pinned and checksum-verified runtime/model dependencies.
  Preserve NVIDIA notices, architecture detection, transactional installation,
  rollback and supported updater compatibility. Old shortcuts require running the
  new setup once to learn `MPCVR-Portable.zip`; do not claim they migrate remotely.
- Large dependencies belong in `marcmy/VideoRenderer-Dependencies`, independently
  published and never overwritten. Do not create per-renderer runtime prereleases.
- Never delete historical releases or their tags without explicit user approval
  of a concrete inventory/compatibility plan. New releases are verified before any
  latest promotion. Preserve predecessors by default. Only explicitly approved
  `MPCVR_KEEP_ONE_RELEASE=true` retention may remove an older verified renderer
  release after replacement verification, recovery archival and source/latest guards.
  Its CI recovery packages expire after 90 days; download them for permanent retention.
- Use `dev.ps1` and the documented commands in `docs/development/workflow.md`.
  Put transient outputs in `_artifacts`; preserve older diagnostic history.

## GitHub Actions

- Do not commit disposable, one-off, repair, probe, or trigger-only workflow files under `.github/workflows/` unless the user explicitly requests a permanent new workflow.
- Do not add workflows named or patterned like `one-time-*`, `*-trigger.yml`, temporary repair workflows, or temporary validation workflows for a single task.
- For one-off validation, prefer existing workflows, `workflow_dispatch`, local build/test scripts, or manual `gh workflow run` invocations.
- If a temporary workflow is absolutely unavoidable during development, remove it before finalizing the task or opening/updating a pull request.
- Reuse or extend an existing durable workflow when CI coverage genuinely needs to change.

## Upstream synchronization

- Fetch upstream and origin before changing source. GitHub Actions cannot update
  the desktop; reconcile the authoritative checkout when Codex next operates here.
- Unknown conflicts must leave master untouched and create a reviewable proposal;
  do not accumulate commit-specific, hard-coded conflict handlers.
- GitHub sync and release jobs must reject stale heads and preserve normal push
  semantics. No force pushes or automatic conflict-marker commits to master.
