#!/usr/bin/env python3
"""
Retire a labeled source: removes its uploaded files from the Onyx
connector, drops its entries from manifest.json, and deletes its
mirror/<label>/ subtree on disk.

This does NOT touch source_dir.conf. Remove (or comment out) the source's
own line there yourself -- before or after running this -- so mirror_reader
stops re-syncing it on the next run. Afterward, re-run build_index.py in
local-knowledge-search to drop the retired files from the search index too
(nothing gets re-embedded, since the cache is keyed by content hash, but
the now-missing records are pruned).

Only labeled sources can be retired this way -- the legacy unlabeled source
mirrors flat at the mirror root with no distinguishing prefix, so there's
no safe way to identify "everything from that source" apart from knowing
every top-level directory it produced. Give it a label in source_dir.conf
first (see README's migration note) if you want to retire it.

Usage:
    python3 retire_source.py <label>              # asks for confirmation
    python3 retire_source.py <label> --yes        # skips confirmation
    python3 retire_source.py <label> --dry-run    # show what would happen, do nothing
"""
import json
import shutil
import subprocess
import sys

import sync


def main() -> None:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    dry_run = "--dry-run" in sys.argv
    skip_confirm = "--yes" in sys.argv

    if len(args) != 1:
        sys.exit(__doc__)
    label = args[0]

    if not label or not all(c.isalnum() or c in "-_" for c in label):
        sys.exit(f"Label must be alphanumeric/-/_ only (matches mirror_reader's own rule), got: {label!r}")

    manifest = sync.load_manifest()
    prefix = f"{label}/"
    matching = [rel for rel in manifest if rel.startswith(prefix)]
    mirror_dir = sync.ROOT / label

    if not matching and not mirror_dir.exists():
        sys.exit(f"Nothing to retire: no manifest entries under '{prefix}' and {mirror_dir} doesn't exist.")

    print(f"Label: {label}")
    print(f"Manifest entries to remove: {len(matching)}")
    for rel in matching[:10]:
        print(f"  - {rel}")
    if len(matching) > 10:
        print(f"  ... and {len(matching) - 10} more")
    print(f"Mirror directory to delete: {mirror_dir} ({'exists' if mirror_dir.exists() else 'does not exist'})")

    if dry_run:
        print("\n--dry-run: no changes made.")
        return

    if not skip_confirm:
        reply = input(f"\nPermanently remove these {len(matching)} file(s) from Onyx and delete {mirror_dir}? [y/N] ")
        if reply.strip().lower() not in ("y", "yes"):
            sys.exit("Aborted, nothing changed.")

    file_ids = [manifest[rel]["file_id"] for rel in matching if "file_id" in manifest[rel]]
    if file_ids:
        token = sync.load_token()
        cmd = [
            "curl", "-sS",
            "-H", f"Authorization: Bearer {token}",
            "-X", "POST", f"{sync.ONYX_BASE}/manage/admin/connector/{sync.CONNECTOR_ID}/files/update",
            "-F", f"file_ids_to_remove={json.dumps(file_ids)}",
        ]
        result = subprocess.run(cmd, capture_output=True, text=True)
        try:
            response = json.loads(result.stdout)
        except json.JSONDecodeError:
            sys.exit(f"Non-JSON response from Onyx, aborting before touching manifest/mirror:\n{result.stdout}\n{result.stderr}")
        if "file_paths" not in response:
            sys.exit(f"Removal failed, aborting before touching manifest/mirror: {response}")

    for rel in matching:
        manifest.pop(rel, None)
    sync.save_manifest(manifest)

    if mirror_dir.exists():
        shutil.rmtree(mirror_dir)

    print(f"\nRetired '{label}': dropped {len(matching)} manifest entr{'y' if len(matching) == 1 else 'ies'}, deleted {mirror_dir}.")
    if file_ids:
        print(f"Removed {len(file_ids)} file(s) from the Onyx connector.")
    else:
        print("No file_ids were present in the manifest for these entries, so nothing was removed from Onyx (they may predate the manifest tracking file_ids, or were never uploaded).")
    print("Remember to remove its line from source_dir.conf, and re-run build_index.py in local-knowledge-search.")


if __name__ == "__main__":
    main()
