---
description: "Use when modifying BlueZ daemon runtime code, logging, signals, command-line options, or startup/shutdown behavior in src/, obexd/, or mesh/."
---

# Daemon Runtime Debugging

- Use the daemon entrypoints as the first anchor:
  - `src/main.c` for `bluetoothd`
  - `obexd/src/main.c` for `obexd`
  - `mesh/main.c` for `bluetooth-meshd`
- Check the logging helpers before changing option parsing or debug behavior:
  - `src/log.c` and `src/log.h`
  - `obexd/src/log.c` and `obexd/src/log.h`
  - `mesh/util.c` for mesh debug enablement
- Runtime debug switches differ by daemon:
  - `bluetoothd`: `--nodetach`, `--debug=<pattern>`, `--experimental`, `--testing`
  - `obexd`: `--nodetach`, `--debug[=<pattern>]`, `SIGUSR2` enables all debug descriptors
  - `bluetooth-meshd`: `--nodetach`, `--debug`, `--dbus-debug`
- `bluetoothd` and `obexd` both treat `SIGUSR2` as a runtime debug toggle. Preserve that behavior unless the task explicitly changes signal handling.
- For foreground debugging, prefer runtime checks that keep logs on stderr rather than background service behavior.
- When a change touches startup, config loading, or shutdown, validate from the daemon entrypoint that owns the option or signal path before reading wider subsystems.
- Prefer narrow validation:
  - focused rebuild of the affected daemon slice
  - relevant tester under `tools/` when available
  - `make check` only after the local daemon-path validation is clean
- Use the daemon manpage sources for user-visible option behavior:
  - `src/bluetoothd.rst.in`
  - `mesh/bluetooth-meshd.rst.in`