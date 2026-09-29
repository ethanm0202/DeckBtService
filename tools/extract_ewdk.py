"""Extract the EWDK UDF ISO to a directory without elevation.

`tar.exe` only sees the ISO9660 stub (a README telling you the payload is UDF/ISO-13346), and
`Mount-DiskImage` needs administrator rights. pycdlib reads UDF directly, so this runs as a
normal user.

Usage:
    python tools\extract_ewdk.py <iso> <destdir>
Install the dependency with: python -m pip install pycdlib
"""

from __future__ import annotations

import os
import sys
import time

import pycdlib

CHUNK = 1 << 22  # 4 MiB


def main() -> int:
    if len(sys.argv) != 3:
        return int(bool(sys.stderr.write(__doc__)))
    iso_path, dest = sys.argv[1], sys.argv[2]

    iso = pycdlib.PyCdlib()
    iso.open(iso_path)
    if not iso.has_udf():
        print("ERROR: image has no UDF filesystem")
        iso.close()
        return 1

    files = 0
    dirs = 0
    total = 0
    failures = 0
    t0 = time.time()
    last = 0.0

    for dirname, dirlist, filelist in iso.walk(udf_path="/"):
        local_dir = os.path.join(dest, dirname.lstrip("/").replace("/", os.sep))
        os.makedirs(local_dir, exist_ok=True)
        dirs += len(dirlist)

        for fname in filelist:
            udf_path = f"{dirname.rstrip('/')}/{fname}"
            out_path = os.path.join(local_dir, fname)
            try:
                with open(out_path, "wb") as fh:
                    iso.get_file_from_iso_fp(fh, udf_path=udf_path, blocksize=CHUNK)
            except Exception as exc:  # keep going; report at the end
                failures += 1
                print(f"  error: {udf_path}: {exc}", flush=True)
                continue
            files += 1
            total += os.path.getsize(out_path)
            now = time.time()
            if now - last > 5.0:
                last = now
                rate = total / max(now - t0, 1e-6) / (1 << 20)
                print(
                    f"  {files:>6} files  {total / (1 << 30):7.2f} GiB  {rate:6.1f} MiB/s  {udf_path}",
                    flush=True,
                )

    iso.close()
    dt = max(time.time() - t0, 1e-6)
    if failures:
        print(f"FAILED: {failures} file(s) could not be extracted; destination is incomplete.", flush=True)
        return 1
    print(
        f"DONE  {files} files, {dirs} dirs, {total / (1 << 30):.2f} GiB in {dt / 60:.1f} min "
        f"({total / dt / (1 << 20):.1f} MiB/s)",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
