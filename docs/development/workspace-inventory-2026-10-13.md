# Workspace reconciliation, 2026-10-13

The authoritative development directory is `C:\Users\marcm\src\VideoRenderer`.
The desktop project API identifies this directory as the VideoRenderer project.
The original ChatGPT-to-Work handoff retained a different default session cwd.
Adding folders to the project did not move the chat or combine their Git databases.
Commands in this session can safely operate in the authoritative directory using
an explicit working directory. No additional permanent clone or new chat is needed.

## Inventory before reconciliation

| Checkout | HEAD | Local content and purpose |
| --- | --- | --- |
| Original `src\VideoRenderer` | `75f29085` | Independent `.git`; clean master, 106 reachable commits behind current fork after fetching; one old detached sync worktree. |
| Documents handoff `work\VideoRenderer-rife` | `bea5902b` | Independent `.git`; confirmed 2850, five untracked diagnostics, 10,943 ignored paths containing research/build/runtime history. |
| `videorenderer-publish-local-20261010` | `bea5902b` | Linked to the handoff common directory; clean tracked source, 250 ignored compiler outputs. |
| `videorenderer-local-sync-20261006` | `f2a3026c` | Historical staged integration, 133 working-file snapshot paths. 117 match current source; 15 are older variants; one unique private diagnostic. |
| `videorenderer-local-sync-20261009` | `b7a88955` | Historical staged integration, 134 snapshot paths. 118 match current source; 15 are older variants; one unique private diagnostic. |
| `videorenderer-license-ci-20261009` | `250608d0` | Clean historical vendor-notice validation. |
| `videorenderer-sync-20261006` | `4d5ecadc` | Clean tracked source; two unique untracked sync-validation files. |
| `videorenderer-sync-7ba46185` | `f193d6d6` | Linked to original common directory; clean tracked source, 224 ignored build files. |
| Old `VideoRenderer-rife-baseline-4f76664` entry | `4f76664a` | Historical/prunable worktree metadata points to a missing directory. Metadata was preserved; it was not pruned. |

Handoff branch histories and the safety stash were imported with ordinary Git
fetch into `refs/remotes/preserved-handoff/*`. Two older side branches contain
unmerged historical commits; they are retained as experiments, not replayed over
the newer working implementation. Their complete histories are in the Git bundle.

## Verified preservation

- The original checkout fast-forwarded to `bea5902b1a98488fce1cfc9df9480d388a6bc2af`.
- Five current untracked diagnostic files were copied without collisions, with
  matching SHA-256 hashes. They remain private, excluded from normal publication.
- `_preserved/workflow-migration-20261013` contains the before-state inventory,
  source hashes, staged/working patches, exact unique working-file ZIPs, complete
  Git history bundles and `reconciliation.json`.
- All original checkouts, worktrees, stashes, ignored dependencies, research and
  experimental builds remain in place. No historical release or installed renderer
  was modified. The old ignored assets remain recoverable at their original paths.
- Required BaseClasses/minhook submodules were initialized at their pinned commits
  in the primary checkout. They are dependencies, not a replacement main clone.

## Causes of repeated workflow failures

1. Project membership was confused with the active chat's cwd; two independent
   Git databases were maintained without a clear authoritative checkout.
2. GitHub-only upstream automation could advance the fork while the original
   desktop master stayed stale. Later work occurred in handoff-linked worktrees.
3. Historical staged integrations and untracked source were not consistently
   distinguished from current implementation, forcing repeated mass comparison.
4. Public binaries used first-parent fork revisions while release names used
   `GITHUB_RUN_NUMBER`; local override builds could repeat numbers across source states.
5. Renderer CI unconditionally built/repacked Maxine, RIFE runtime and models,
   coupling a small renderer update to a roughly 930 MB setup and duplicate packs.
6. Upstream sync accumulated source-hash-specific conflict handlers instead of
   providing an ordinary reviewable conflict proposal.
7. Release publication had no serialized current-master promotion contract.

Future development uses the primary checkout and reusable documented commands.
Historical folders are preservation locations, not routine integration destinations.
