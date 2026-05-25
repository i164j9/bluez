---
applyTo: "debian/**"
description: "Use when modifying Debian packaging, release metadata, patch series, package splits, or autopkgtest files in BlueZ."
---

# Debian Release Packaging

- Treat `debian/` changes as release-facing even when source code is unchanged.
- Use `debian/control`, `debian/rules`, `debian/changelog`, `debian/tests/control`, and `debian/patches/series` as the primary sources of truth.
- Keep Debian package splits aligned with `debian/control`:
  - `bluez` ships the main tools and daemons.
  - `bluez-obexd`, `bluez-meshd`, `bluez-cups`, `bluez-test-tools`, and `bluez-test-scripts` are separate package surfaces.
- Preserve the feature policy in `debian/rules` unless the packaging task explicitly changes it. Debian currently enables a broad feature set, including `--enable-debug`, `--enable-testing`, `--enable-experimental`, and `--enable-external-ell`.
- When changing Debian patches, update `debian/patches/series` in lockstep and keep the patch stack minimal.
- When changing release metadata, keep `debian/changelog` consistent with the package version and target suite.
- `debian/rules` disables `dh_auto_test`; do not claim Debian package validation from upstream `make check` alone.
- For packaging validation, prefer the narrowest relevant check:
  - review `debian/rules` configure flags for the impacted package split
  - verify `debian/tests/control` and installed-file lists when package contents change
  - use a release-oriented upstream build and `make distcheck` for source-package or build-system changes
- Link to existing documentation instead of restating it: see `README`, `INSTALL`, and `debian/README.Debian`.