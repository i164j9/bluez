---
name: debug-build-validate
description: "Use when preparing a BlueZ debug build, sanitizer build, release build, distcheck run, or build validation for debugging and release work."
---

# Debug Build Validate

Use this skill when the task requires a concrete BlueZ build or validation run, especially for debugging and release-oriented changes.

## Inputs To Inspect First

- `configure.ac`
- `acinclude.m4`
- `Makefile.am`
- `README`
- `INSTALL`
- `debian/rules` for Debian-specific release builds

## Profile Selection

Choose the smallest profile that matches the task.

### Debug Profile

Use for runtime debugging, log-path changes, signal handling, or reproducing crashes.

Recommended flags:

```sh
--enable-debug --disable-optimization
```

Add these only when needed:

```sh
--enable-testing
--enable-asan
--enable-lsan
--enable-ubsan
```

### Release Profile

Use for packaging, install-layout work, documentation tied to built artifacts, or build-system validation.

Recommended flags:

```sh
--prefix=/usr --mandir=/usr/share/man --sysconfdir=/etc --localstatedir=/var
```

## Execution Pattern

Prefer an out-of-tree build directory for each profile.

Debug example:

```sh
SRC=$PWD
mkdir -p "$SRC/../bluez-build-debug"
cd "$SRC/../bluez-build-debug"
"$SRC/configure" --enable-debug --disable-optimization --enable-testing
make -j"$(nproc)"
make check
```

Release example:

```sh
SRC=$PWD
mkdir -p "$SRC/../bluez-build-release"
cd "$SRC/../bluez-build-release"
"$SRC/configure" \
  --prefix=/usr \
  --mandir=/usr/share/man \
  --sysconfdir=/etc \
  --localstatedir=/var
make -j"$(nproc)"
make check
make distcheck
```

## BlueZ-Specific Checks

- If mesh or BTP client is enabled, ensure the `ell` source tree is adjacent to the repository or use `--enable-external-ell`.
- If manpages are enabled, ensure `rst2man` is available.
- `--enable-maintainer-mode` adds stricter warnings and can turn warning-only environments into failures. Use it intentionally, mainly for build-system work.
- `debian/rules` is intentionally broader than the minimal upstream release profile. Follow it when the task is Debian packaging, not when the task is generic upstream validation.

## Output Expectations

- Report the exact profile used.
- Report the exact configure flags used.
- State whether validation was a focused build, `make check`, or `make distcheck`.
- If validation could not run, name the missing prerequisite instead of guessing.