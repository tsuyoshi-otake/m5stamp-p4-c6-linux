# Pinned upstream source submodules

The five directories in this folder are Git submodules pinned by the parent
repository's index and `firmware/versions.lock.json`:

| Path | Purpose |
| --- | --- |
| `linux/` | Linux v6.18.35 base for the RV32 NOMMU port |
| `buildroot/` | Buildroot 2025.02.15 rootfs and toolchain framework |
| `esp-idf/` | ESP-IDF v5.5.3 P4 boot-shim and C6 firmware framework |
| `esp-hosted/` | ESP-Hosted-NG SDIO transport |
| `why2025-linux/` | Board-specific native-P4 reference implementation |

Initialise the pinned source trees with:

```bash
git submodule sync --recursive
git submodule update --init --recursive
python3 ../tools/verify-source-lock.py
```

The parent repository commits only submodule gitlinks, not generated objects,
toolchains, build directories, flash images, credentials, or per-device keys.
This keeps the exact source revisions in the repository while allowing a
clean checkout to choose an external build/cache volume.  The Linux gitlink
uses the official GitHub mirror because the kernel.org endpoint ignores Git's
blob filter during shallow fetch; its pinned commit is identical to the
canonical `linux.git` lock entry.

For an external source cache instead of the checked-in submodules, use
`firmware/tools/fetch-sources.sh /absolute/path/to/cache`; both modes are
validated against the same immutable manifest.
