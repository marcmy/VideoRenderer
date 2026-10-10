# SVPflow1 motion search

This is the native motion-search subset of the public SVPflow1 source archive,
version 0x4500, revision 205, from https://www.svp-team.com/files/gpl/svpflow1-src.zip.
The original per-file SHA-256 values and archive identity are in `upstream.json`.
Copyright notices are preserved. This subset and its x264 pixel kernels are
GPL-2.0-or-later; see `COPYING` and each source file's notice. MPCVR's adapter
and classifier are GPL-3.0-or-later. No SVPflow2 binary or proprietary code is used.

Ports: explicitly include cstdint; extract the two pyramid layout helpers from
MVSuperCore so MVFrame does not depend on a plugin host or JSON; call the existing
x264 CPU EMMS assembly routine where modern MSVC lacks the MMX intrinsic.
The motion-search/SIMD algorithms are otherwise unchanged.

Build requires NASM (validated with portable 3.01). Put nasm.exe on PATH or pass
`/p:NasmPath=<absolute path to nasm.exe>` to MSBuild. `SVPflow1.targets` compiles
the C++ sources without the renderer PCH and assembles the proper Windows ABI.
No CPU instruction set is globally forced; the upstream runtime CPU dispatch
chooses supported pixel kernels. The existing build workflow pins and verifies
the assembler download. NASM is a build tool, not a playback dependency.

The renderer selects an independent conservative classifier: 16x16 blocks,
one-way hierarchical HEX2 search, integer pixels, coarse SATD/final SAD, luma
only, neutral chroma. A block is bad when the SVPflow1 adjusted matching error
exceeds 15% of 255 per pixel; a cut requires over 20% bad visible area. Replica
edges make partial CPU blocks safe, and their votes are weighted by visible
area. This is **not** a claim of exact parity with the closed SVPflow2 classifier
or its documented SAD/luma thresholds. The Image comparison threshold remains
separate and is used only by that detector or the failure fallback.

Two content-only analysis pictures use MPCVR's existing GPU area reducer. Native
RIFE processing resolution and presentation remain unchanged. Analysis is once
per source pair, on an adapter owned by each worker, outside the output loop.
Device/geometry changes recreate bounded buffers; CPU state contains no previous
frame identity. Tiny/unsupported inputs and GPU failures use Image comparison.
Analysis is limited to one megapixel to bound CPU work; it is an optional accuracy
experiment, not a promised throughput improvement over Image comparison.
