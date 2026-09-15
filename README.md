# knowledge-base-sync

> **Status:** published for reference. Issues and pull requests are closed; this is a personal tool shared as-is, not a maintained project. macOS only.

Keeps one or more folders of markdown files (listed in `source_dir.conf`,
currently just `~/Documents/Projects`) synced into one or more configured
destinations (listed in `destinations.conf` — currently an Onyx file
connector and the [local-knowledge-search](https://github.com/komplettsystem/local-knowledge-search)
search index), every 30 minutes, without ever granting broad Full Disk
Access to a shared interpreter.

## Why it's built this way

`~/Documents` is a macOS TCC-protected folder. A naive `launchd`-scheduled
Python script that reads it directly needs Full Disk Access granted to
whatever binary runs it — typically the system `python3`, which is shared by
every other script on the machine. Granting FDA to a shared interpreter is a
much bigger blast radius than intended (Mail, Photos, Messages, other users'
files — not just this one folder), so this project avoids it entirely with a
sources → mirror → destinations design, where sources and destinations are
each independently fault-isolated (one failing never blocks the others) and
both are config-driven rather than hardcoded:

1. **`mirror_reader.c`** — a small compiled C binary, built specifically for
   this job. It's the *only* thing that ever reads any tracked source
   folder. The list of folders is read at runtime from `source_dir.conf`
   (one `label: path` per line — path relative to `$HOME` unless it starts
   with `/`; blank lines and `#` comments ignored; a line with no `:` is
   treated as a single unlabeled/legacy source). `source_dir.conf` lives
   outside Documents, so reading it needs no TCC access — adding, renaming,
   or removing a tracked folder is a config edit, no recompile. Falls back
   to a single `Documents/antigravity` entry if the file is missing or
   empty.

   Each source is mirrored into its own `mirror/<label>/...` subtree (an
   unlabeled/legacy entry mirrors flat at the mirror root instead), so two
   sources can never collide on a shared relative path, and the collapse
   guard (below) is checked **per source** — a cloud-drive folder that's
   temporarily unmounted refuses to touch just its own subtree, while every
   other source keeps syncing normally.

   TCC grants are per-binary, but not uniform across folder types: a plain
   subfolder under `~/Documents` needs "Documents Folder" access (System
   Settings → Privacy & Security → Files and Folders); a mounted cloud-drive
   folder (e.g. `~/Library/CloudStorage/GoogleDrive-.../My Drive/...`) isn't
   in that list at all and will likely need **Full Disk Access** granted to
   this binary instead — a real permission-scope tradeoff worth weighing
   before adding a cloud-drive source. Either way, only ever grant it to
   this one compiled binary, never to a shared interpreter.

   It mirrors every `.md` file from each source into its subtree under
   `~/.knowledge-base-sync/mirror` — outside the protected tree — and refuses to touch
   a source's subtree if that source's scan collapses to less than half of
   what's already there (a broken/blocked scan, an unmounted drive, or
   `source_dir.conf` pointing at a folder that no longer exists, all look
   identical to "everything got deleted"; this guard turns that into a safe
   no-op instead of data loss for that source — see the incident this
   fixed, below).

2. **Destinations** — one or more independent consumers of the mirror,
   listed in `destinations.conf` (one `label: shell command` per line;
   blank lines and `#` comments ignored). `run_sync.sh` runs each command
   in its own subshell and keeps going regardless of any single one's
   outcome — a destination command that itself calls `exit` only ends its
   own subshell, never the script. Falls back to the original fixed
   pair (Onyx upload, then the local-knowledge-search index) if the file is
   missing or empty.

   **`sync.py`** (the Onyx destination) reads only from
   `~/.knowledge-base-sync/mirror` (never Documents), so it needs no
   special permission at all. Diffs against a manifest
   (`~/.knowledge-base-sync/manifest.json`, not tracked in git — see
   `.gitignore`) and uploads only what changed via Onyx's `files/update`
   endpoint, rather than re-uploading everything each run. Carries the same
   "refuse if the scan collapsed" guard as an independent second layer.

   This decoupling is what keeps, say, `local-knowledge-search`'s index
   refreshing on schedule even while the Onyx destination is failing loudly
   (Onyx stopped, unreachable, whatever) — before this, a hard-coded
   two-step script under `set -e` meant one destination's failure silently
   stopped every destination after it.

`run_sync.sh` sequences sources then destinations, and a `launchd`
LaunchAgent (`com.knowledgebase.sync.plist.template` here; the live copy is
installed at `~/Library/LaunchAgents/com.knowledgebase.sync.plist`) runs it
every 30 minutes. Its own exit code is non-zero if *anything* failed (a
source or a destination), so launchd/log-monitoring can tell — but nothing
failing ever prevents anything else from being attempted.

**Incident this design fixes:** an earlier, simpler version ran `sync.py`
directly under `launchd` against `~/Documents` with no guard. A permission
gap under `launchd` (present even though interactive-terminal testing never
showed it, since Terminal.app's own trust doesn't extend to `launchd`-spawned
processes) made every scan return 0 files, which the old logic read as "the
user deleted everything" and removed 334 of 357 tracked files from the Onyx
connector. Restored from the untouched source of truth (disk); the
`mirror_reader` + collapse-guard design above is what replaced it.

## Files

| File | Role |
|---|---|
| `mirror_reader.c` | Source for the only binary allowed to touch the tracked source folders. Compile with `cc -O2 -o mirror_reader mirror_reader.c`. |
| `source_dir.conf` | One `label: path` per line (path relative to `$HOME`, or absolute). Edit this to add/move/rename tracked folders — no recompile needed. Not tracked in git (machine-specific paths). |
| `destinations.conf` | One `label: shell command` per line. Edit this to add/remove/disable destinations — no code change needed. Not tracked in git (machine-specific paths/commands). |
| `sync.py` | The Onyx destination: incremental diff-and-upload to the Onyx connector. Needs no special permissions. Also importable as a module (see `retire_source.py`). |
| `retire_source.py` | One-off admin tool: removes a labeled source's files from Onyx, drops them from the manifest, and deletes its `mirror/<label>/` subtree. See "Retiring a source" below. |
| `run_sync.sh` | Rotates `sync.log` if it's grown past 5MB, refreshes the mirror from every source, then runs every configured destination. |
| `com.knowledgebase.sync.plist.template` | Reference copy of the LaunchAgent definition; the deployed one lives in `~/Library/LaunchAgents/`. |
| `mirror/`, `manifest.json`, `sync.log`(`.1`), `token`, `mirror_reader` (binary) | Generated/runtime state or secrets — gitignored, not source. |

## Installation

```bash
cd ~/.knowledge-base-sync   # or wherever you keep this checked out
cc -O2 -o mirror_reader mirror_reader.c
chmod +x run_sync.sh
cat > source_dir.conf <<'EOF'
# label: path (relative to $HOME, or absolute). Defaults to a single
# Documents/antigravity entry if this file is missing or empty.
projects: Documents/your-folder-name
# gdrive-jobs: /Users/you/Library/CloudStorage/GoogleDrive-you@x.com/My Drive/Jobs
EOF
```

Each labeled source mirrors into its own `mirror/<label>/...` subtree. Note
that switching an *existing* source from the legacy unlabeled form to a
labeled one is a one-time migration, not a rename: the next run copies
everything into the new `mirror/<label>/` location and leaves the old flat
copies in place (they're a different mirror subtree as far as the collapse
guard is concerned). Clean up the stale flat copies yourself afterward, and
re-run `build_index.py` in `local-knowledge-search` — the content hasn't
changed so nothing gets re-embedded, but the recorded paths do.

Configure destinations:

```bash
cat > destinations.conf <<'EOF'
# label: shell command. Falls back to the onyx + local-knowledge-search
# pair below if this file is missing or empty. Comment out a line to
# pause that destination without losing the config.
onyx: /usr/bin/python3 "$HOME/.knowledge-base-sync/sync.py"
local-knowledge-search: cd "$HOME/Local-Tools/local-knowledge-search" && "$HOME/Local-Tools/local-knowledge-search/venv/bin/python3" build_index.py
EOF
```

Write your Onyx personal access token (never commit this):

```bash
echo -n "your_onyx_pat_here" > token
chmod 600 token
```

Update `CONNECTOR_ID` and `ONYX_BASE` at the top of `sync.py` for your own
Onyx deployment and connector.

Install the LaunchAgent:

```bash
sed "s|/Users/YOUR_USERNAME|$HOME|g" com.knowledgebase.sync.plist.template \
  > ~/Library/LaunchAgents/com.knowledgebase.sync.plist
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.knowledgebase.sync.plist
```

Grant `mirror_reader` folder access when macOS prompts on its first real run
— Files and Folders → Documents Folder for a plain `~/Documents` subfolder,
or Full Disk Access for a mounted cloud-drive folder (see above). It will
not appear in these lists until it has actually attempted the access once.

Verify it's running and healthy:

```bash
launchctl print gui/$(id -u)/com.knowledgebase.sync
tail -f sync.log
```

## Retiring a source

Deleting a source's line from `source_dir.conf` stops `mirror_reader` from
re-syncing it, but nothing automatically cleans up what it already synced —
`sync.py` just walks the whole mirror tree with no idea a source was ever
retired, so its files would sit in the mirror and keep getting uploaded to
Onyx (and appearing in `local-knowledge-search` results) indefinitely. Use
`retire_source.py` instead:

```bash
python3 retire_source.py <label> --dry-run   # see what would be removed
python3 retire_source.py <label>             # asks for confirmation, then does it
```

This removes the label's files from the Onyx connector, drops them from
`manifest.json`, and deletes `mirror/<label>/`. It does not touch
`source_dir.conf` — remove that line yourself, and re-run `build_index.py`
in `local-knowledge-search` afterward to drop the retired records from the
search index (nothing gets re-embedded; the cache is keyed by content hash,
so this just prunes now-missing entries). Only labeled sources can be
retired this way — the legacy unlabeled source mirrors flat with no
distinguishing prefix, so migrate it to a label first if you need to retire
it (see the migration note above).

## Storage growth

Neither the mirror nor the log grows without bound under normal use:

- **`mirror/`** tracks each *active* source 1:1 — deletions in the source
  propagate to deletions in the mirror on the very next run, per source.
  It does not accumulate stale content on its own.
- **`sync.log`** is rotated by `run_sync.sh` once it exceeds 5MB, keeping
  one prior generation (`sync.log.1`) — capped at roughly 10MB total.

The one thing that *does* accumulate silently is a **retired source**: if
you delete a source's line from `source_dir.conf` without also running
`retire_source.py` for it, its already-mirrored files are never revisited
or cleaned up by anything — see "Retiring a source" above.

## Safety notes

- Never grant Documents/Full Disk Access to `python3`, `bash`, or any other
  shared interpreter for this job — only to the compiled `mirror_reader`
  binary. A plain shell/Python *script* does not get its own TCC identity;
  only a distinct compiled binary does.
- The collapse guard — checked independently per source in `mirror_reader`,
  and again across the whole mirror in `sync.py` (refuse if the scan
  returns under 50% of what's already tracked) — is what stands between a
  permission regression (or an unmounted cloud drive) and mass data loss.
  Don't remove it.
- Labels in `source_dir.conf` become literal `mirror/<label>/` directory
  names — `mirror_reader` restricts them to `[A-Za-z0-9_-]` and skips (with
  a stderr warning) any line that doesn't fit, so a stray colon or slash in
  a path can't be mistaken for a label and can't escape the mirror root.
- `destinations.conf` commands run via `bash -c` in `run_sync.sh` — this
  file is machine-local config you write yourself, the same trust level as
  a crontab entry, not untrusted input. `bash -c` (a real subshell) rather
  than `eval` is what makes one destination calling `exit` end only its own
  subshell, never `run_sync.sh` itself.
- `token` and `manifest.json` are gitignored on purpose — the former is a
  credential, the latter is regenerable local state tied to this specific
  Onyx connector's file IDs.
