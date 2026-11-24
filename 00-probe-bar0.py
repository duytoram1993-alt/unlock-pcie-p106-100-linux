#!/usr/bin/env python3
"""
00-probe-bar0.py - Read-only PCIe & BAR0 mirror fingerprint probe (writes NOTHING)

Checks:
  1. PCIe config space registers on the GPU and upstream Root Port (LNKCAP, LNKSTA, LNKCAP2, LNKCTL2)
  2. BAR0 mirror registers (0x88084 vs LNKCAP, 0x880A4 vs LNKCAP2) -> MATCH / MISMATCH
  3. On kernels with CONFIG_IO_STRICT_DEVMEM=y (e.g., Fedora/RHEL), userspace resource0 mmap
     returns EINVAL while nvidia is bound -> automatically falls back to gen2kr_p106.ko probe_only=1.

Usage:
  sudo python3 00-probe-bar0.py
  TARGET_BDF=0000:01:00.0 sudo -E python3 00-probe-bar0.py
"""
import mmap
import os
import struct
import subprocess
import sys

TARGET_IDS = ("1c07", "1c06", "1c09", "1b87")

BAR0_REGS = [
    (0x88078, "EXP_HEAD       (PCIe cap version in bits[19:16])"),
    (0x88084, "LNKCAP mirror  (MaxSpeed=bits[3:0], MaxWidth=bits[9:4])"),
    (0x88088, "LNKSTA mirror  (CurSpeed=bits[19:16], CurWidth=bits[25:20])"),
    (0x880A4, "LNKCAP2 mirror (0x02=Gen1 only, 0x06=Gen1+Gen2)"),
    (0x880A8, "LNKCTL2 mirror (TLS=bits[3:0])"),
    (0x8841C, "PRIV_MISC_1    (bit15 preset, bits[14:13] downgrade gate)"),
    (0x8860C, "VSEC_DEVICE"),
    (0x88610, "VSEC_HIERARCHY (bit0/bit12)"),
    (0x8872C, "XVE_OVR        (write 6 to trigger LTSSM)"),
    (0x2241C, "PUNITS.PCI     (bit7=PCIE_SPEED, bit0=PCIE_VERSION)"),
    (0x2157C, "FUSE GEN23_DIS (read-only)"),
    (0x21580, "FUSE GEN3_DIS  (read-only)"),
    (0x8C040, "LINK_CONFIG_0  (SPEED=bits[19:18]: Pascal 1=5GT, 2=2.5GT; bit0=COMMIT)"),
    (0x8C1C0, "PL_LINK_RATE"),
    (0x8C2C0, "CYA_0          (bit2=DIS_G2)"),
    (0x8E110, "XP3G_OVR0"),
    (0x8E11C, "XP3G_OVR3"),
    (0x8E120, "XP3G_VAL0"),
    (0x8E12C, "XP3G_VAL3"),
]


def find_gpu_bdf():
    env_bdf = os.environ.get("TARGET_BDF")
    if env_bdf:
        return env_bdf
    base = "/sys/bus/pci/devices"
    matches = []
    for dev in sorted(os.listdir(base)):
        try:
            v = open(f"{base}/{dev}/vendor").read().strip().lower().replace("0x", "")
            d = open(f"{base}/{dev}/device").read().strip().lower().replace("0x", "")
            if v == "10de" and d in TARGET_IDS:
                matches.append((dev, d))
        except OSError:
            continue
    if len(matches) == 1:
        return matches[0][0]
    if not matches:
        raise RuntimeError("No supported NVIDIA Pascal mining GPU found (10de:1c07/1c06/1c09/1b87)")
    raise RuntimeError(f"Multiple GPUs found {matches}; set TARGET_BDF=0000:xx:xx.0")


def find_upstream_bdf(gpu_bdf):
    real = os.path.realpath(f"/sys/bus/pci/devices/{gpu_bdf}")
    parent = os.path.basename(os.path.dirname(real))
    if os.path.exists(f"/sys/bus/pci/devices/{parent}/config"):
        return parent
    return None


def find_pcie_cap(cfg):
    ptr = cfg[0x34] & 0xFC
    seen = set()
    while ptr and ptr not in seen and ptr < 0x100:
        seen.add(ptr)
        if cfg[ptr] == 0x10:
            return ptr
        ptr = cfg[ptr + 1] & 0xFC
    return None


def read_pcie_regs(bdf):
    with open(f"/sys/bus/pci/devices/{bdf}/config", "rb") as f:
        cfg = f.read(256)
    cap = find_pcie_cap(cfg)
    if cap is None:
        raise RuntimeError(f"PCIe Capability (0x10) not found on {bdf}")
    lnkcap  = struct.unpack_from("<I", cfg, cap + 0x0C)[0]
    lnkctl  = struct.unpack_from("<H", cfg, cap + 0x10)[0]
    lnksta  = struct.unpack_from("<H", cfg, cap + 0x12)[0]
    lnkcap2 = struct.unpack_from("<I", cfg, cap + 0x2C)[0]
    lnkctl2 = struct.unpack_from("<H", cfg, cap + 0x30)[0]
    return cap, lnkcap, lnkctl, lnksta, lnkcap2, lnkctl2


def speed_name(code):
    return {1: "Gen1 (2.5 GT/s)", 2: "Gen2 (5.0 GT/s)", 3: "Gen3 (8.0 GT/s)"}.get(code, f"Unknown({code})")


def probe_via_kernel_module(dev_id_hex):
    script_dir = os.path.dirname(os.path.abspath(__file__))
    kver = os.uname().release
    sys_ko = f"/lib/modules/{kver}/extra/p106-gen2/gen2kr_p106.ko"
    local_ko = os.path.join(script_dir, "kmod", "gen2kr_p106.ko")
    ko_path = sys_ko if os.path.exists(sys_ko) else local_ko
    if not os.path.exists(ko_path):
        print(f"  [ERROR] {ko_path} not found. Build it first: bash 02-push-gen2.sh build", file=sys.stderr)
        return 1

    lsmod_out = subprocess.check_output(["lsmod"], text=True)
    already_loaded = any(line.startswith("gen2kr_p106 ") for line in lsmod_out.splitlines())
    if already_loaded:
        print("  (gen2kr_p106 is currently active - showing recent kernel logs instead of reloading)")
        dmesg = subprocess.check_output(["dmesg"], text=True, errors="replace")
        lines = [l for l in dmesg.splitlines() if "gen2kr_p106:" in l][-18:]
        for l in lines:
            print(" ", l)
        return 0

    print("  (Userspace BAR0 mmap blocked by CONFIG_IO_STRICT_DEVMEM=y while nvidia is bound;")
    print("   automatically using read-only gen2kr_p106.ko probe_only=1 - NO registers written!)")
    try:
        if ko_path == sys_ko:
            subprocess.check_call(["modprobe", "gen2kr_p106", f"dev_id={dev_id_hex}", "probe_only=1"])
        else:
            subprocess.check_call(["insmod", ko_path, f"dev_id={dev_id_hex}", "probe_only=1"])
    finally:
        subprocess.run(["rmmod", "gen2kr_p106"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    dmesg = subprocess.check_output(["dmesg"], text=True, errors="replace")
    all_probe = [l for l in dmesg.splitlines() if "gen2kr_p106: [probe]" in l]
    start_idx = 0
    for idx, line in enumerate(all_probe):
        if "--- BAR0 snapshot" in line:
            start_idx = idx
    probe_lines = all_probe[start_idx:] if start_idx or (all_probe and "--- BAR0 snapshot" in all_probe[0]) else all_probe[-18:]
    for l in probe_lines:
        print(" ", l)

    joined = "\n".join(probe_lines)
    print("\n============================================================")
    if "BAR0_FINGERPRINT=MATCH" in joined:
        print("RESULT: BAR0 MIRROR FINGERPRINT = MATCH (safe to proceed)")
        return 0
    if "BAR0_FINGERPRINT=MISMATCH" in joined:
        print("RESULT: BAR0 MIRROR FINGERPRINT = MISMATCH (DO NOT PROCEED!)")
        return 2
    print("RESULT: Could not parse probe output from dmesg.")
    return 1


def main():
    if os.geteuid() != 0:
        print("[ERROR] Root privileges (sudo) required to read PCIe config space and BAR0.", file=sys.stderr)
        return 1

    bdf = find_gpu_bdf()
    up_bdf = find_upstream_bdf(bdf)
    vid = open(f"/sys/bus/pci/devices/{bdf}/vendor").read().strip()
    did = open(f"/sys/bus/pci/devices/{bdf}/device").read().strip()
    bar0_phys = int(open(f"/sys/bus/pci/devices/{bdf}/resource").readline().split()[0], 16)

    print("============================================================")
    print(f"GPU BDF      : {bdf}  (ID: {vid}:{did})")
    print(f"Root Port    : {up_bdf or 'unknown'}")
    print(f"BAR0 Phys    : 0x{bar0_phys:08x}")
    print("============================================================")

    cap, lnkcap, lnkctl, lnksta, lnkcap2, lnkctl2 = read_pcie_regs(bdf)
    print(f"\n[1] PCIe Config Space - GPU ({bdf}, cap=0x{cap:02x}):")
    print(f"  LNKCAP  = 0x{lnkcap:08x}  MaxSpeed={lnkcap & 0xF} ({speed_name(lnkcap & 0xF)})  MaxWidth=x{(lnkcap >> 4) & 0x3F}")
    print(f"  LNKSTA  = 0x{lnksta:04x}      CurSpeed={lnksta & 0xF} ({speed_name(lnksta & 0xF)})  CurWidth=x{(lnksta >> 4) & 0x3F}")
    print(f"  LNKCAP2 = 0x{lnkcap2:08x}  Vector=0x{(lnkcap2 >> 1) & 0x7F:02x}")
    print(f"  LNKCTL2 = 0x{lnkctl2:04x}      TLS={lnkctl2 & 0xF} ({speed_name(lnkctl2 & 0xF)})")

    if up_bdf:
        rcap, r_lcap, _, r_lsta, r_lcap2, r_lctl2 = read_pcie_regs(up_bdf)
        print(f"\n[2] PCIe Config Space - Root Port ({up_bdf}, cap=0x{rcap:02x}):")
        print(f"  LNKCAP  = 0x{r_lcap:08x}  MaxSpeed={r_lcap & 0xF} ({speed_name(r_lcap & 0xF)})  MaxWidth=x{(r_lcap >> 4) & 0x3F}")
        print(f"  LNKSTA  = 0x{r_lsta:04x}      CurSpeed={r_lsta & 0xF} ({speed_name(r_lsta & 0xF)})  CurWidth=x{(r_lsta >> 4) & 0x3F}")
        print(f"  LNKCTL2 = 0x{r_lctl2:04x}      TLS={r_lctl2 & 0xF} ({speed_name(r_lctl2 & 0xF)})")

    print("\n[3] BAR0 Registers (read-only):")
    res0_path = f"/sys/bus/pci/devices/{bdf}/resource0"
    try:
        fd = os.open(res0_path, os.O_RDONLY | os.O_SYNC)
        try:
            mm = mmap.mmap(fd, 0x90000, mmap.MAP_SHARED, mmap.PROT_READ, offset=0)
            vals = {}
            for off, desc in BAR0_REGS:
                v = struct.unpack_from("<I", mm, off)[0]
                vals[off] = v
                extra = ""
                if off == 0x88078:
                    extra = f" | VER={(v >> 16) & 0xF}"
                elif off == 0x88084:
                    extra = f" | MaxSpeed={v & 0xF}, MaxWidth=x{(v >> 4) & 0x3F}"
                elif off == 0x88088:
                    extra = f" | CurSpeed={(v >> 16) & 0xF}, CurWidth=x{(v >> 20) & 0x3F}"
                elif off == 0x2241C:
                    extra = f" | VER={v & 1}, SPD={(v >> 7) & 1}"
                elif off == 0x8C040:
                    extra = f" | SPEED={(v >> 18) & 3}, COMMIT={v & 1}"
                elif off == 0x8C2C0:
                    extra = f" | DIS_G2={(v >> 2) & 1}"
                print(f"  0x{off:05x} = 0x{v:08x}   {desc}{extra}")
            mm.close()
        finally:
            os.close(fd)
    except OSError:
        return probe_via_kernel_module(did)

    m_lnkcap = vals[0x88084]
    m_lnkcap2 = vals[0x880A4]
    match = (m_lnkcap == lnkcap) and (m_lnkcap2 == lnkcap2)
    print("\n============================================================")
    print(f"LNKCAP  config=0x{lnkcap:08x}  BAR0_mirror=0x{m_lnkcap:08x}")
    print(f"LNKCAP2 config=0x{lnkcap2:08x}  BAR0_mirror=0x{m_lnkcap2:08x}")
    if match:
        print("RESULT: BAR0 MIRROR FINGERPRINT = MATCH (safe to proceed)")
        return 0
    print("RESULT: BAR0 MIRROR FINGERPRINT = MISMATCH (DO NOT PROCEED!)")
    return 2


if __name__ == "__main__":
    sys.exit(main())
