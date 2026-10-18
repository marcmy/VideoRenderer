# MPCVR Setup

Download **MPCVR-Setup.zip**, extract it, close MPC-HC, and run `Install-MPCVR-Maxine-RIFE.cmd`.

The small ZIP includes the exact x86/x64 renderer and installation scripts. Before installation it detects the NVIDIA GPU, resolves `dependencies.lock.json`, and downloads the required Maxine runtime, RIFE models, common TensorRT/CUDA runtime, and only the applicable SM75/SM86/SM89/SM120 architecture packs. All dependency URLs use exact immutable releases in `marcmy/VideoRenderer-Dependencies`; sizes and SHA-256 hashes are checked before use.

Verified downloads are cached under `%LOCALAPPDATA%\MPCVideoRenderer\downloads`. Setup checks the installed Maxine and RIFE files against their pinned dependencies and skips components that already match, including download, extraction and runtime installation. Renderer and updater installation still proceed. A small `dependency-receipt.json` in each runtime root records the dependency identity and expected file hashes; setup rechecks the files on every update. Missing or damaged files, changed pins or a newly required GPU pack cause the affected component to be installed using the existing transactional installer.

Older installations without receipts can be adopted without reinstallation when their original verified archives remain in the download cache. If neither a valid receipt nor the matching cached archives are available, setup installs the component once to establish verified state. Existing RIFE engine caches remain subject to their existing version/compatibility checks; skipped components and their engine caches are left in place.

Setup installs the Maxine runtime and its NVIDIA notices, the RIFE runtime and models, the K-Lite restore/update shortcut, and finally the custom renderer using the existing transactional installers. It preserves their backup/rollback behavior and performs runtime preflight afterward. RIFE models include 4.4, 4.6, 4.15 Lite, 4.25 and 4.25 Lite; the current default remains 4.25. NVIDIA components retain their separate license terms.

**MPCVR-Portable.zip** contains renderer files for manual installation. **SHA256SUMS.txt** verifies both downloads. There is no per-renderer runtime prerelease and no duplicate renderer ZIP alias.

Run this setup once to migrate restore/update shortcuts installed by older builds. Their local scripts cannot be updated remotely. The current updater prefers the small setup so future dependency changes are reconciled as well, and supports portable/legacy filenames as fallbacks. Old bundled setups pin historical runtime URLs; deleting those releases breaks fresh installs from those old setups. Use the current setup instead.

`-ValidateOnly -NoPause` checks the lightweight setup offline without querying a live GPU, downloading dependencies, or changing an installation. Optional `-GpuInventoryJson` fixtures validate architecture selection. The same installer retains compatibility with full bundled payloads.

A first install needs Internet access. Later installs can reuse all verified cache entries. Fully offline historical packages and diagnostic artifacts have been preserved locally; normal renderer updates do not generate another gigabyte-scale offline package.
