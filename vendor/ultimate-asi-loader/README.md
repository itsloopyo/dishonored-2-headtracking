# ultimate-asi-loader (vendored)

This directory contains a bundled copy of the upstream mod loader. It is the install-time
source of truth: install.cmd extracts directly from here and never reaches out to the network.
Refresh manually with `pixi run update-deps`, then commit.

## Snapshot

- Asset: `Ultimate-ASI-Loader_x64.zip`
- Tag: `v9.7.4`
- Commit: `6b440669144c4a0bef5718ab155df160d231cd42`
- Upstream URL: https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v9.7.4/Ultimate-ASI-Loader_x64.zip
- SHA-256: `8272d83b2692662098746f2d0ad0e2d85f3c8358ab1d63f75fbe835c2c8135fd`
- Fetched at: 2026-09-08T08:17:44.9557331+01:00
- Source: github

Do not edit this directory by hand. Run ``pixi run package`` (or CI release) to refresh.

## Committed artifact

Only `dinput8.dll` is committed; the upstream zip is a download intermediate
and is deleted after extraction.

- File: `dinput8.dll` (extracted from the asset above, unmodified)
- SHA-256: `fa266e3513d02c08a1b808f28c10538a489eaffaa4b0707f7cc1066e71b5afd7`
