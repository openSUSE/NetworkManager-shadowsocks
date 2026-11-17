#!/bin/sh
set -eu
display_dir=$(mktemp -d /tmp/nmss-ui-XXXXXX)
Xvfb -displayfd 3 -screen 0 1024x900x24 -nolisten tcp 3>"$display_dir/display" >"$display_dir/log" 2>&1 &
display_pid=$!
trap 'kill "$display_pid" 2>/dev/null || true; wait "$display_pid" 2>/dev/null || true' EXIT HUP INT TERM
attempt=0
while [ ! -s "$display_dir/display" ]; do
    attempt=$((attempt + 1))
    if [ "$attempt" -gt 30 ]; then cat "$display_dir/log"; exit 1; fi
    sleep 0.1
done
# Build workers need not have a login session or a valid machine-id for
# D-Bus autolaunch. Give each test its own bus and avoid desktop a11y services.
DISPLAY=":$(cat "$display_dir/display")" GDK_BACKEND=x11 GSETTINGS_BACKEND=memory \
    GTK_A11Y=none NO_AT_BRIDGE=1 GIO_USE_VFS=local \
    GTK_THEME=Adwaita GSK_RENDERER=cairo dbus-run-session -- "$@"
