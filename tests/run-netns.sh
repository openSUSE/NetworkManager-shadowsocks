#!/bin/sh
# Capture the parent namespace before entering the private namespace. /proc/1
# may be hidden by ptrace restrictions after entering an unprivileged user ns.
set -eu
SS_TEST_PARENT_NETNS=$(readlink /proc/self/ns/net)
export SS_TEST_PARENT_NETNS
exec unshare --user --map-root-user --net "$@"
