#!/bin/bash
# Refreshes the mirror from every tracked source (mirror_reader -- the only
# process with Documents Folder / Full Disk access), then runs every
# configured destination against it (see destinations.conf). This script
# itself never opens anything under ~/Documents -- it only launches
# mirror_reader and whatever destination commands are configured -- so it
# needs no TCC grant of its own.
#
# Sources and destinations are both independently fault-isolated: one
# source being unreachable doesn't stop mirror_reader from syncing the
# others (see mirror_reader.c), and one destination failing (e.g. Onyx
# being stopped) doesn't stop the others from running (e.g.
# local-knowledge-search's index still refreshes). The final exit code is
# non-zero if anything failed, so launchd/monitoring can tell -- but every
# source and every destination is still attempted regardless of any other
# one's outcome.
set -e

# Log rotation: launchd redirects this run's stdout/stderr to sync.log via
# an fd it opened before this script started, so renaming the file (not
# truncating it in place, which would leave the already-open fd writing at
# a stale offset and pad the new file with a hole) is safe mid-run -- this
# run's own output keeps flowing into the renamed file, and launchd creates
# a fresh sync.log for the next scheduled run. Keeps at most one rotated
# generation (sync.log.1), so total log footprint is capped at ~2x the
# threshold below.
LOG_FILE="$HOME/.knowledge-base-sync/sync.log"
LOG_MAX_BYTES=$((5 * 1024 * 1024))
if [ -f "$LOG_FILE" ]; then
    log_size=$(stat -f%z "$LOG_FILE" 2>/dev/null || echo 0)
    if [ "$log_size" -gt "$LOG_MAX_BYTES" ]; then
        mv -f "$LOG_FILE" "$LOG_FILE.1"
    fi
fi

any_failed=0

# mirror_reader itself already isolates failures per source internally and
# returns non-zero if any one source's collapse guard tripped -- that must
# not block destinations from running against whatever the other sources
# (or the previously-synced content) still provide.
if ! "$HOME/.knowledge-base-sync/mirror_reader"; then
    echo "mirror_reader: at least one source failed (see its own output above)" >&2
    any_failed=1
fi

# destinations.conf: one "label: shell command" per line, blank lines and
# "#" comments ignored. Falls back to the original fixed two-destination
# pipeline (Onyx upload, then local-knowledge-search's index) if the file
# is missing or empty, so an unconfigured machine behaves as before.
DEST_CONF="$HOME/.knowledge-base-sync/destinations.conf"

run_destination() {
    local label="$1" cmd="$2"
    echo "--- destination: $label ---"
    # Run in a genuine subshell (bash -c), not `eval` in this shell -- a
    # destination command that itself calls `exit` (directly, or via some
    # shell function/wrapper it invokes) must only end that subshell, never
    # this script. `eval` would let such an `exit` terminate run_sync.sh
    # outright, defeating the whole point of per-destination isolation.
    if bash -c "$cmd"; then
        echo "[$label] OK"
    else
        echo "[$label] FAILED (exit $?)" >&2
        any_failed=1
    fi
}

if [ -s "$DEST_CONF" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%%#*}"
        # trim leading/trailing whitespace without invoking a subshell
        line="${line#"${line%%[![:space:]]*}"}"
        line="${line%"${line##*[![:space:]]}"}"
        [ -z "$line" ] && continue
        label="${line%%:*}"
        cmd="${line#*:}"
        label="${label#"${label%%[![:space:]]*}"}"
        label="${label%"${label##*[![:space:]]}"}"
        cmd="${cmd#"${cmd%%[![:space:]]*}"}"
        cmd="${cmd%"${cmd##*[![:space:]]}"}"
        [ -z "$label" ] || [ -z "$cmd" ] && continue
        run_destination "$label" "$cmd"
    done < "$DEST_CONF"
else
    run_destination "onyx" "/usr/bin/python3 \"$HOME/.knowledge-base-sync/sync.py\""
    run_destination "local-knowledge-search" "cd \"$HOME/Local-Tools/local-knowledge-search\" && \"$HOME/Local-Tools/local-knowledge-search/venv/bin/python3\" build_index.py"
fi

exit "$any_failed"
