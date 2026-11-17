"""Refresh the source-only inventory (portable across Git's Windows line endings)."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
manifest = root / "SOURCE-MANIFEST.json"
data = {
    "revision": "coord-r005",
    "upstream": "mauri870/DLSSNR-RDNA3 a2b4edebcd236c8c6d189b340542ce0bf0c3d673",
    "license": "MIT",
    "weights_included": False,
    "hash_policy": "SHA-256 of source bytes with CRLF normalized to LF; manifest excludes itself",
    "files": {
        p.relative_to(root).as_posix(): hashlib.sha256(p.read_bytes().replace(b"\r\n", b"\n")).hexdigest()
        for p in sorted(root.rglob("*"))
        if p.is_file() and p != manifest and "__pycache__" not in p.parts
    },
}
manifest.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
