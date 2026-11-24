#!/usr/bin/env python3
"""
01-patch-driver-gen2.py - 2-byte patch for NVIDIA Linux driver (nvidia.ko*)
                          to stop RM from downgrading PCIe link back to Gen1.

How it works:
  In NVIDIA RM's set_link_speed(mode) function (.text):
    Original:
      lea -0x1(%rdx), %eax    ; 8d 42 ff   (valid range: mode in {1, 2, 3})
      ...
      cmp $0x2, %eax          ; 83 f8 02
      ja  <bail>              ; 0f 87 ...  (returns 0 / NV_OK)

    Patched (2 bytes):
      lea -0x2(%rdx), %eax    ; 8d 42 fe   (valid range shifted to {2, 3})
      ...
      cmp $0x1, %eax          ; 83 f8 01
      ja  <bail>              ; 0f 87 ...  (mode==1 underflows -> bails cleanly with NV_OK)

  Result: Calls requesting Gen1 (mode==1) return NV_OK without writing SPEED=2 to 0x8C040,
  while Gen2/Gen3 (mode 2/3) and invalid modes (>=4) behave identically to stock.

Supports raw .ko, .ko.xz (CRC32 verified), .ko.zst, .ko.gz.
Completely independent from the mining-card blacklist (.rodata) patch.
"""
import argparse
import glob
import gzip
import hashlib
import lzma
import os
import shutil
import subprocess
import sys
import tempfile

GEN2_ORIG  = bytes.fromhex("8d42ff415641554989fd41544531e4534883ed404889751089550c83f8020f87")
GEN2_PATCH = bytes.fromhex("8d42fe415641554989fd41544531e4534883ed404889751089550c83f8010f87")


def find_nvidia_ko(explicit_path=None):
    if explicit_path:
        return explicit_path
    try:
        out = subprocess.check_output(["modinfo", "-n", "nvidia"], text=True, stderr=subprocess.DEVNULL).strip()
        if out and os.path.exists(out):
            return out
    except Exception:
        pass

    kver = os.uname().release
    patterns = [
        f"/lib/modules/{kver}/extra/nvidia*/nvidia.ko*",
        f"/lib/modules/{kver}/updates/dkms/nvidia.ko*",
        f"/lib/modules/{kver}/updates/nvidia*.ko*",
        f"/lib/modules/{kver}/extramodules/nvidia.ko*",
        f"/lib/modules/{kver}/kernel/drivers/video/nvidia.ko*",
        f"/usr/lib/modules/{kver}/extramodules/nvidia.ko*",
    ]
    for pat in patterns:
        for match in sorted(glob.glob(pat)):
            if os.path.isfile(match):
                return match
    return f"/lib/modules/{kver}/updates/dkms/nvidia.ko"


def backup_path_for(ko_path):
    kver = os.uname().release
    base = os.path.basename(ko_path)
    return f"/var/backups/unlock-pcie-p106-100-linux/{kver}/{base}.orig"


def read_ko_bytes(ko_path):
    if ko_path.endswith(".xz"):
        with lzma.open(ko_path, "rb") as f:
            return bytearray(f.read()), "xz"
    if ko_path.endswith(".zst"):
        zstd_bin = shutil.which("zstd")
        if not zstd_bin:
            raise RuntimeError("Found .ko.zst module, but 'zstd' is not installed.")
        raw = subprocess.check_output([zstd_bin, "-d", "-c", "-q", ko_path])
        return bytearray(raw), "zst"
    if ko_path.endswith(".gz"):
        with gzip.open(ko_path, "rb") as f:
            return bytearray(f.read()), "gz"
    with open(ko_path, "rb") as f:
        return bytearray(f.read()), "raw"


def finalize_atomic_replace(tmp_file, target_path, st):
    os.chmod(tmp_file, st.st_mode & 0o7777)
    os.chown(tmp_file, st.st_uid, st.st_gid)
    if shutil.which("chcon"):
        subprocess.run(["chcon", "--reference", target_path, tmp_file],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.replace(tmp_file, target_path)
    if shutil.which("restorecon"):
        subprocess.run(["restorecon", target_path],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def write_ko_bytes_atomic(ko_path, data, fmt):
    ko_dir = os.path.dirname(os.path.abspath(ko_path))
    st = os.stat(ko_path)

    if fmt == "xz":
        fd_raw, tmp_raw = tempfile.mkstemp(prefix=".nvko_raw_", dir=ko_dir)
        tmp_out = tmp_raw + ".xz"
        try:
            with os.fdopen(fd_raw, "wb") as f:
                f.write(data)
            xz_bin = shutil.which("xz")
            if xz_bin:
                subprocess.check_call([xz_bin, "-T0", "-6", "--check=crc32", "-f", tmp_raw])
                info = subprocess.check_output([xz_bin, "--list", tmp_out], text=True)
                if "CRC32" not in info:
                    raise RuntimeError(f"Compressed .xz does not use CRC32:\n{info}")
            else:
                with lzma.open(tmp_out, "wb", format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC32, preset=6) as f:
                    f.write(data)
                os.unlink(tmp_raw)
            finalize_atomic_replace(tmp_out, ko_path, st)
        except Exception:
            for p in (tmp_raw, tmp_out):
                if os.path.exists(p):
                    try:
                        os.unlink(p)
                    except OSError:
                        pass
            raise
    elif fmt == "zst":
        zstd_bin = shutil.which("zstd")
        if not zstd_bin:
            raise RuntimeError("'zstd' is required to recompress .ko.zst modules.")
        fd_raw, tmp_raw = tempfile.mkstemp(prefix=".nvko_raw_", dir=ko_dir)
        tmp_out = tmp_raw + ".zst"
        try:
            with os.fdopen(fd_raw, "wb") as f:
                f.write(data)
            subprocess.check_call([zstd_bin, "-T0", "-q", "-f", "-o", tmp_out, tmp_raw])
            os.unlink(tmp_raw)
            finalize_atomic_replace(tmp_out, ko_path, st)
        except Exception:
            for p in (tmp_raw, tmp_out):
                if os.path.exists(p):
                    try:
                        os.unlink(p)
                    except OSError:
                        pass
            raise
    elif fmt == "gz":
        fd_tmp, tmp_out = tempfile.mkstemp(prefix=".nvko_gz_", dir=ko_dir)
        try:
            with os.fdopen(fd_tmp, "wb") as raw_f:
                with gzip.GzipFile(fileobj=raw_f, mode="wb", compresslevel=6) as gz_f:
                    gz_f.write(data)
            finalize_atomic_replace(tmp_out, ko_path, st)
        except Exception:
            if os.path.exists(tmp_out):
                os.unlink(tmp_out)
            raise
    else:
        fd_tmp, tmp_out = tempfile.mkstemp(prefix=".nvko_tmp_", dir=ko_dir)
        try:
            with os.fdopen(fd_tmp, "wb") as f:
                f.write(data)
            finalize_atomic_replace(tmp_out, ko_path, st)
        except Exception:
            if os.path.exists(tmp_out):
                os.unlink(tmp_out)
            raise


def find_all(buf, needle):
    hits = []
    pos = 0
    while True:
        i = buf.find(needle, pos)
        if i == -1:
            break
        hits.append(i)
        pos = i + 1
    return hits


def inspect_state(data):
    orig_hits = find_all(data, GEN2_ORIG)
    patched_hits = find_all(data, GEN2_PATCH)
    if len(orig_hits) == 1 and len(patched_hits) == 0:
        return "ORIG", orig_hits[0]
    if len(orig_hits) == 0 and len(patched_hits) == 1:
        return "PATCHED", patched_hits[0]
    return "UNKNOWN", (orig_hits, patched_hits)


def ensure_backup(ko_path):
    bak = backup_path_for(ko_path)
    if os.path.exists(bak):
        print(f"  [Backup] Keeping existing backup: {bak}")
        return bak
    os.makedirs(os.path.dirname(bak), exist_ok=True)
    shutil.copy2(ko_path, bak)
    print(f"  [Backup] Saved backup -> {bak}")
    return bak


def sha256_short(b):
    return hashlib.sha256(b).hexdigest()[:16]


def main():
    ap = argparse.ArgumentParser(
        description="Patch nvidia.ko(.xz/.zst/.gz) to prevent NVIDIA RM from forcing PCIe Gen1"
    )
    ap.add_argument("action", choices=["check", "apply", "revert", "restore-orig"],
                    help="Action to perform")
    ap.add_argument("--ko", default=None,
                    help="Optional explicit path to nvidia.ko / .ko.xz / .ko.zst / .ko.gz")
    args = ap.parse_args()

    ko_path = find_nvidia_ko(args.ko)
    if not os.path.exists(ko_path):
        print(f"[ERROR] NVIDIA kernel module not found: {ko_path}", file=sys.stderr)
        return 1

    bak = backup_path_for(ko_path)

    if args.action == "restore-orig":
        if os.geteuid() != 0:
            print("[ERROR] Root privileges (sudo) required for restore-orig.", file=sys.stderr)
            return 1
        if not os.path.exists(bak):
            print(f"[ERROR] Backup file not found: {bak}", file=sys.stderr)
            return 1
        st = os.stat(ko_path)
        tmp_path = ko_path + ".restore_tmp"
        shutil.copy2(bak, tmp_path)
        finalize_atomic_replace(tmp_path, ko_path, st)
        print(f"[OK] Restored {ko_path} from {bak}")
        print("Reboot to load the restored module.")
        return 0

    print(f"Module : {ko_path}")
    print(f"Backup : {bak} ({'present' if os.path.exists(bak) else 'not created yet'})")
    data, fmt = read_ko_bytes(ko_path)
    print(f"Size   : {len(data):,} bytes (uncompressed) | sha256={sha256_short(data)}... | format={fmt}")
    print()

    state, info = inspect_state(data)

    if args.action == "check":
        if state == "ORIG":
            print(f"[STATUS] UNPATCHED (ORIG) @ offset 0x{info:x}")
            print("         set_link_speed allows mode==1 (driver will downgrade PCIe link back to Gen1).")
            return 0
        if state == "PATCHED":
            print(f"[STATUS] PATCHED @ offset 0x{info:x}")
            print("         lea -1 -> lea -2 (ff->fe), cmp $2 -> cmp $1 (02->01) active.")
            return 0
        print(f"[STATUS] SIGNATURE NOT FOUND (orig_hits={info[0]}, patched_hits={info[1]})", file=sys.stderr)
        print("         Driver version may have different codegen in set_link_speed().", file=sys.stderr)
        return 2

    if os.geteuid() != 0:
        print(f"[ERROR] Root privileges (sudo) required for '{args.action}'.", file=sys.stderr)
        return 1

    if args.action == "apply":
        if state == "PATCHED":
            print(f"[OK] Already patched @ offset 0x{info:x}. No changes needed.")
            return 0
        if state != "ORIG":
            print(f"[ERROR] Could not uniquely locate signature (orig={info[0]}, patched={info[1]}). Aborting safely!", file=sys.stderr)
            return 2
        ensure_backup(ko_path)
        data[info:info + len(GEN2_PATCH)] = GEN2_PATCH
        print(f"  -> Patched 2 bytes @ offset 0x{info:x} (lea -1 -> lea -2, cmp $2 -> cmp $1)")
        expected = "PATCHED"
    else:  # revert
        if state == "ORIG":
            print(f"[OK] Module is already unpatched @ offset 0x{info:x}.")
            return 0
        if state != "PATCHED":
            print(f"[ERROR] Module does not match patched signature (orig={info[0]}, patched={info[1]}).", file=sys.stderr)
            return 2
        data[info:info + len(GEN2_ORIG)] = GEN2_ORIG
        print(f"  -> Reverted 2 bytes @ offset 0x{info:x}")
        expected = "ORIG"

    print(f"\nWriting ({fmt}) atomically to {ko_path} ...")
    write_ko_bytes_atomic(ko_path, data, fmt)

    verify_data, _ = read_ko_bytes(ko_path)
    st_after, off_after = inspect_state(verify_data)
    if st_after != expected:
        print(f"[CRITICAL] Post-write verification failed: expected {expected}, got {st_after}", file=sys.stderr)
        return 3

    print(f"[VERIFIED] state={st_after} @ offset 0x{off_after:x} | sha256={sha256_short(verify_data)}...")
    print("\n[SUCCESS] Reboot (or reload nvidia when idle) for the 2-byte Gen2 patch to take effect.")
    print("          Note: Rebuild initramfs if your distro embeds nvidia.ko in initramfs.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
