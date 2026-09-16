# minhook (vendored)

Trimmed copy of the MinHook sources this mod statically links into
`Dishonored2HeadTracking.asi`. Only the files CMake compiles are kept; the
upstream build system, DLL resources, and tests are omitted.

## Snapshot

- Upstream: https://github.com/TsudaKageyu/minhook
- Tag: `v1.3.4`
- Commit: `c3fcafdc10146beb5919319d0683e44e3c30d537`
- License: BSD-2-Clause. `LICENSE.txt` is upstream's file reproduced verbatim
  and covers both Tsuda Kageyu's copyright and Vyacheslav Patkov's separate
  copyright on Hacker Disassembler Engine 32/64, which `src/hde/` is derived
  from. Do not trim it: BSD-2-Clause clause 1 requires the notice, the
  conditions, and the disclaimer to travel with the source.

## Local modifications

BSD-2-Clause permits modification. Ours are listed here so a reader never has
to diff against upstream to find them:

- `src/hook.c` - `MH_Initialize` allocates from `GetProcessHeap()` instead of a
  private `HeapCreate` heap, and `MH_Uninitialize` no longer calls
  `HeapDestroy`. Avoids conflicting with the game's own heap management.

Every other upstream file kept here is byte-identical to the v1.3.4 tag
(ignoring line-ending and BOM normalization applied by this repo's
`.gitattributes`). This README is ours, not upstream's.
