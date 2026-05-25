# BlueZ Agent Notes

Use this file as the project-level default for AI coding agents. Keep changes small, prefer source inputs over generated autotools outputs, and validate with the narrowest build or test command that matches the area you touched.

## Source Of Truth

- Build and install defaults are documented in `README` and `INSTALL`.
- Autotools feature switches and compiler/profile flags live in `configure.ac` and `acinclude.m4`.
- The top-level build graph is in `Makefile.am`, which includes `Makefile.tools`, `Makefile.obexd`, `Makefile.mesh`, and `Makefile.plugins`.
- Runtime daemon debug options are documented in `src/bluetoothd.rst.in` and `mesh/bluetooth-meshd.rst.in`.

## Repository Shape

- `src/` contains the main `bluetoothd` daemon and shared daemon infrastructure.
- `client/` contains `bluetoothctl`.
- `monitor/` contains `btmon` tracing tooling.
- `obexd/` and `mesh/` are separate daemons with their own build slices.
- `tools/`, `emulator/`, `peripheral/`, and `unit/` contain test tools, emulation helpers, and unit-test coverage.
- `debian/` contains distro packaging inputs. Treat packaging changes as release-facing.

## Generated Files

- Prefer editing `configure.ac`, `acinclude.m4`, and `Makefile.am` inputs instead of generated files such as `configure`, `Makefile.in`, `aclocal.m4`, `config.h.in`, `ltmain.sh`, `compile`, `depcomp`, `install-sh`, `missing`, and `test-driver`.
- Do not regenerate autotools outputs unless the task explicitly requires build-system regeneration.
- Do not use `make maintainer-clean` casually. It removes checked-in generated build files and the vendored `ell` symlink tree.

## Build Profiles

### Debug-Oriented Build

- Prefer a separate build directory when comparing profiles. `INSTALL` confirms VPATH builds are supported.
- Default debug configure flags are:
  - `--enable-debug`
  - `--disable-optimization`
  - `--enable-testing` when the change affects test tools or emulator-backed flows
- Optional sanitizer flags are available through `acinclude.m4`:
  - `--enable-asan`
  - `--enable-lsan`
  - `--enable-ubsan`
- Use `--enable-maintainer-mode` only when you want stricter warning gates and maintainer-only validation. It adds `-Werror` and extra warnings, so it is useful for build-system work but can be too strict for unrelated environment issues.

Recommended pattern from the source root:

```sh
SRC=$PWD
mkdir -p "$SRC/../bluez-build-debug"
cd "$SRC/../bluez-build-debug"
"$SRC/configure" --enable-debug --disable-optimization --enable-testing
make -j"$(nproc)"
make check
```

### Release-Oriented Build

- Use the packaging-style install paths from `README` unless the task says otherwise:
  - `--prefix=/usr`
  - `--mandir=/usr/share/man`
  - `--sysconfdir=/etc`
  - `--localstatedir=/var`
- Keep release builds conservative. Do not enable `--enable-testing`, `--enable-experimental`, or `--enable-deprecated` unless the release task explicitly needs them.
- Feature trimming for release/package work is usually done with configure switches such as `--disable-tools`, `--disable-monitor`, `--disable-client`, `--disable-systemd`, `--disable-udev`, and `--disable-cups`.
- `Makefile.am` defines `AM_DISTCHECK_CONFIGURE_FLAGS`; use `make distcheck` for release/build-system validation instead of inventing a custom tarball flow.

Recommended pattern from the source root:

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

## Build Pitfalls

- Mesh and BTP client builds require the `ell` source tree adjacent to this repository unless you configure with `--enable-external-ell`.
- Manpage builds require `rst2man` when `--enable-manpages` is on.
- `--enable-testing` pulls in emulator-backed tools and may depend on kernel features that are not present on every machine.
- Maintainer mode can also enable coverage, `dbus-run-session`, and valgrind-assisted test wrappers when those tools are installed.

## Debugging Runtime Behavior

- `bluetoothd` supports foreground logging with `--nodetach` and file-filtered debug logs with `--debug=<glob-or-file-list>`.
- `bluetooth-meshd` supports `--nodetach`, `--debug`, and `--dbus-debug`.
- When debugging logging behavior, inspect the daemon-specific logging helpers in `src/log.c`, `src/log.h`, `obexd/src/log.c`, and `obexd/src/log.h` before changing command-line parsing.

## Validation Bias

- After changing daemon code, prefer the narrowest executable validation available: targeted `make check`, a relevant tester under `tools/`, or a focused rebuild of the affected slice.
- After changing autotools inputs, prefer `make check` for the touched profile first and `make distcheck` for release-facing or packaging-facing work.
- If you touch packaging in `debian/`, validate with a release-oriented configure profile rather than a debug profile.

## Related Customizations

- Use `.github/instructions/debian-release-packaging.instructions.md` for Debian packaging and release metadata work.
- Use `.github/instructions/daemon-runtime-debugging.instructions.md` for daemon entrypoint, logging, and signal-path changes.
- Use `.github/skills/debug-build-validate/SKILL.md` when you need a repeatable BlueZ debug or release build-validation workflow.