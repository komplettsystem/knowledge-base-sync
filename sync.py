#!/usr/bin/env python3
"""
Incremental sync of markdown docs into an Onyx file connector. Only uploads new/changed files and removes deleted ones -- not a
full rebuild.

Reads from ~/.knowledge-base-sync/mirror, NOT the tracked source folders
directly -- the mirror is kept current by the separate compiled
`mirror_reader` binary, which is the only thing that needs (or has)
Documents Folder / Full Disk access. This script deliberately needs no
special permissions at all.

Usage:
    python3 sync.py            # sync, print a summary
    python3 sync.py --dry-run  # show what would change, do nothing
"""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path.home() / ".knowledge-base-sync" / "mirror"
MANIFEST_PATH = Path.home() / ".knowledge-base-sync" / "manifest.json"
TOKEN_PATH = Path.home() / ".knowledge-base-sync" / "token"
CONNECTOR_ID = 3  # your Onyx file connector's ID
ONYX_BASE = "http://localhost:3000/api"

EXCLUDE_DIR_NAMES = {
    "node_modules", ".venv", "venv", ".git", "__pycache__",
    ".next", "dist", "build", "site-packages", ".cache",
}


def should_include(path: Path) -> bool:
    if path.name.lower() == "claude.md":
        return False
    if any(part in EXCLUDE_DIR_NAMES for part in path.parts):
        return False
    return True


def scan() -> dict[str, str]:
    """relative_path -> sha256 hash, for every tracked file on disk."""
    current = {}
    for p in ROOT.rglob("*.md"):
        if not p.is_file() or not should_include(p):
            continue
        rel = str(p.relative_to(ROOT))
        current[rel] = hashlib.sha256(p.read_bytes()).hexdigest()
    return current


def load_manifest() -> dict:
    if MANIFEST_PATH.exists():
        return json.loads(MANIFEST_PATH.read_text())
    return {}


def save_manifest(manifest: dict) -> None:
    MANIFEST_PATH.write_text(json.dumps(manifest, indent=2, sort_keys=True))


def load_token() -> str:
    if not TOKEN_PATH.exists():
        sys.exit(f"No token at {TOKEN_PATH} -- write the Onyx personal access token there first.")
    return TOKEN_PATH.read_text().strip()


def main() -> None:
    dry_run = "--dry-run" in sys.argv
    manifest = load_manifest()  # rel_path -> {"hash": ..., "file_id": ...}
    current = scan()

    # A scan that returns drastically fewer files than the manifest already
    # tracks is far more likely to indicate a broken scan (e.g. the launchd
    # process lacking Full Disk Access to ~/Documents) than a mass deletion.
    # Treating it as real deletions would wipe out the connector's file
    # store, as happened on 2026-09-11. Refuse instead.
    if manifest and len(current) < 0.5 * len(manifest):
        sys.exit(
            f"Refusing to sync: scanned only {len(current)} files but the "
            f"manifest tracks {len(manifest)}. This looks like a broken scan "
            f"(e.g. missing Full Disk Access), not real deletions. Aborting "
            f"without touching the connector or the manifest."
        )

    new_or_changed = [
        rel for rel, h in current.items()
        if rel not in manifest or manifest[rel]["hash"] != h
    ]
    deleted = [rel for rel in manifest if rel not in current]

    print(f"Scanned {len(current)} files under {ROOT}")
    print(f"New/changed: {len(new_or_changed)}")
    print(f"Deleted: {len(deleted)}")

    if not new_or_changed and not deleted:
        print("Nothing to do.")
        return

    if dry_run:
        for rel in new_or_changed:
            print("  + (new/changed)", rel)
        for rel in deleted:
            print("  - (removed)", rel)
        return

    token = load_token()
    file_ids_to_remove = [manifest[rel]["file_id"] for rel in deleted if "file_id" in manifest[rel]]
    file_ids_to_remove += [manifest[rel]["file_id"] for rel in new_or_changed if rel in manifest and "file_id" in manifest[rel]]

    cmd = [
        "curl", "-sS",
        "-H", f"Authorization: Bearer {token}",
        "-X", "POST", f"{ONYX_BASE}/manage/admin/connector/{CONNECTOR_ID}/files/update",
        "-F", f"file_ids_to_remove={json.dumps(file_ids_to_remove)}",
    ]
    for rel in new_or_changed:
        full_path = ROOT / rel
        # Preserve the relative path as the uploaded filename for clean citations.
        cmd += ["-F", f"files=@{full_path};filename={rel}"]

    result = subprocess.run(cmd, capture_output=True, text=True)
    try:
        response = json.loads(result.stdout)
    except json.JSONDecodeError:
        sys.exit(f"Non-JSON response from Onyx:\n{result.stdout}\n{result.stderr}")

    if "file_paths" not in response:
        sys.exit(f"Update failed: {response}")

    # `file_paths` is the connector's full post-update file listing, not just
    # what was just uploaded -- fetch it explicitly and match by filename
    # instead of assuming order/length. (A prior version assumed the latter;
    # that assumption silently corrupted the manifest on 2026-09-11.)
    listing = subprocess.run(
        ["curl", "-sS", "-H", f"Authorization: Bearer {token}",
         f"{ONYX_BASE}/manage/admin/connector/{CONNECTOR_ID}/files"],
        capture_output=True, text=True,
    )
    try:
        current_files = {f["file_name"]: f["file_id"] for f in json.loads(listing.stdout)["files"]}
    except (json.JSONDecodeError, KeyError):
        sys.exit(f"Uploaded successfully but couldn't confirm file_ids afterward -- manifest left untouched, fix and re-run:\n{listing.stdout}\n{listing.stderr}")

    missing = [rel for rel in new_or_changed if rel not in current_files]
    if missing:
        sys.exit(f"Uploaded but {len(missing)} file(s) not found in post-update listing -- aborting manifest update: {missing}")

    for rel in new_or_changed:
        manifest[rel] = {"hash": current[rel], "file_id": current_files[rel]}
    for rel in deleted:
        manifest.pop(rel, None)

    save_manifest(manifest)
    print(f"Synced. {len(new_or_changed)} uploaded, {len(deleted)} removed. Indexing triggered server-side.")


if __name__ == "__main__":
    main()
