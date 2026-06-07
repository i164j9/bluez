#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BUILD_DIR=${BUILD_DIR:-build/debug}
RUN_DIR=${RUN_DIR:-$ROOT/$BUILD_DIR/private-bus-run}
SOCKET="$RUN_DIR/system_bus_socket"
BTD_LOG="$RUN_DIR/bluetoothd.log"
TEST_LOG="$RUN_DIR/profile-tester.log"
STATUS_LOG="$RUN_DIR/status.log"
XDG_DIR="$RUN_DIR/xdg-runtime"
BTMON_LOG="$RUN_DIR/private-bus.btmon"
BTD_BIN="$ROOT/$BUILD_DIR/src/bluetoothd"
TESTER_BIN="$ROOT/$BUILD_DIR/tools/profile-tester"

BUS_PID=
BTD_PID=
BTMON_PID=

filter_bluetoothd_log() {
	[ -f "$BTD_LOG" ] || return 0

	tmp_log="$BTD_LOG.tmp"
	grep -v -E 'Failed to remove UUID: Authentication Failed \(0x05\)|Failed to remove UUID: Invalid Index \(0x11\)|Failed to set mode: Invalid Index \(0x11\)' \
		"$BTD_LOG" >"$tmp_log" 2>/dev/null || true
	mv -f "$tmp_log" "$BTD_LOG" 2>/dev/null || true
	chmod 664 "$BTD_LOG" 2>/dev/null || true
}

write_status() {
	status=${1:-unknown}
	printf 'exit=%s\n' "$status" >"$STATUS_LOG.tmp" 2>/dev/null || return 0
	mv -f "$STATUS_LOG.tmp" "$STATUS_LOG" 2>/dev/null || true
	chmod 644 "$STATUS_LOG" 2>/dev/null || true
}

restore_bluetooth() {
	systemctl start bluetooth >/dev/null 2>&1 || \
	service bluetooth start >/dev/null 2>&1 || true
}

cleanup() {
	ret=${1:-$?}

	trap - EXIT INT TERM

	write_status "$ret"
	if [ -n "$BTD_PID" ]; then
		kill "$BTD_PID" 2>/dev/null || true
		wait "$BTD_PID" 2>/dev/null || true
	fi
	filter_bluetoothd_log
	if [ -n "$BUS_PID" ]; then
		kill "$BUS_PID" 2>/dev/null || true
		wait "$BUS_PID" 2>/dev/null || true
	fi
	if [ -n "$BTMON_PID" ]; then
		kill "$BTMON_PID" 2>/dev/null || true
		wait "$BTMON_PID" 2>/dev/null || true
	fi
	restore_bluetooth
	exit "$ret"
}

trap 'cleanup $?' EXIT INT TERM

mkdir -p "$RUN_DIR"
mkdir -p "$XDG_DIR"
chown "$(id -u)":"$(id -g)" "$RUN_DIR" "$XDG_DIR" 2>/dev/null || true
chmod 755 "$RUN_DIR" 2>/dev/null || true
chmod 700 "$XDG_DIR"
rm -f "$SOCKET" "$BTD_LOG" "$TEST_LOG" "$STATUS_LOG" "$BTMON_LOG"
write_status running

unset DBUS_SESSION_BUS_ADDRESS DBUS_STARTER_ADDRESS DBUS_STARTER_BUS_TYPE
export XDG_RUNTIME_DIR="$XDG_DIR"

if [ -n "${LSAN_OPTIONS:-}" ]; then
	export LSAN_OPTIONS="$LSAN_OPTIONS:fast_unwind_on_malloc=0:malloc_context_size=30"
else
	export LSAN_OPTIONS="fast_unwind_on_malloc=0:malloc_context_size=30"
fi

systemctl stop bluetooth >/dev/null 2>&1 || \
	service bluetooth stop >/dev/null 2>&1 || true

if command -v btmon >/dev/null 2>&1; then
	btmon -w "$BTMON_LOG" >/dev/null 2>&1 &
	BTMON_PID=$!
fi

BUS_PID=$(dbus-daemon --session --address="unix:path=$SOCKET" --fork --print-pid 1)
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$SOCKET"

cd "$ROOT"
"$BTD_BIN" --nodetach --compat --debug=src/profile.c:src/device.c:src/adapter.c >"$BTD_LOG" 2>&1 &
BTD_PID=$!

until gdbus call --address="$DBUS_SYSTEM_BUS_ADDRESS" \
		--dest org.freedesktop.DBus \
		--object-path /org/freedesktop/DBus \
		--method org.freedesktop.DBus.ListNames >/dev/null 2>&1; do
	if ! kill -0 "$BTD_PID" 2>/dev/null; then
		echo 'bluetoothd exited before private system bus became ready' >&2
		cleanup 1
	fi
done

TESTER_DEBUG_ARG=
if [ "${PROFILE_TESTER_DEBUG:-0}" = "1" ]; then
	TESTER_DEBUG_ARG=-d
fi

if "$TESTER_BIN" $TESTER_DEBUG_ARG >"$TEST_LOG" 2>&1; then
	ret=0
else
	ret=$?
fi

tail -n 160 "$TEST_LOG" 2>/dev/null || true
cleanup "$ret"