[English](README.md) | [Tiếng Việt](README.vi.md)

# unlock-pcie-p106-100-linux

Unlock **PCIe Gen2 (`5.0 GT/s`)** on NVIDIA **P106-100** (`10DE:1C07` / `10DE:1C06`) and other Pascal mining GPUs (**P106-090 `10DE:1C09`**, **P104-100 `10DE:1B87`**) under **Linux** with the proprietary NVIDIA driver.

> **For 3D/gaming FPS throttling (~15–45 FPS cap):** That issue is caused by NVIDIA's Restricted SKU table, which is handled independently in the companion repository [remove-black-list-nvidia-linux](https://github.com/duytoram1993-alt/remove-black-list-nvidia-linux). Both tools are 100% independent and can be used separately or together.

---

## Real-World Verified Result

Verified on **Fedora 45 KDE Plasma** (kernel `7.3.3-300.fc45.x86_64`, proprietary driver `akmod-nvidia-580xx 580.196.07`, Gigabyte **P106-100 `10DE:1C07`**, Intel Core i7-3770K + H61 motherboard):

| Register / Metric | Before (Cold Boot) | After (`p106-gen2.service`) |
|---|---|---|
| **GPU `0000:01:00.0` `LNKSTA`** | `0x1081` (`2.5 GT/s x8`) | **`0x1082` (`5.0 GT/s x8` — Gen2)** |
| **Root Port `0000:00:01.0` `LNKSTA`** | `0x1081` (`2.5 GT/s x8`) | **`0x1082` (`5.0 GT/s x8` — Gen2)** |
| **`LNKCAP2` / `LNKCTL2`** | `0x00000002` / `TLS=1` | **`0x00000006` / `TLS=2`** |
| **`0x2241C` (`PUNITS.PCI`)** | `0x00000000` | **`0x00000081`** (latched `CAP2=0x06` in ~13 µs) |
| **Stability / Errors** | — | **0 Xid errors, 0 Gen1 fallbacks** (runs automatically at boot before KDE) |

> **Note on link width (`x4` / `x8` / `x16`):** This tool unlocks **link speed** (`2.5 GT/s → 5.0 GT/s`, doubling PCIe bandwidth per lane). Physical lane width is determined by your card PCB and motherboard slot and cannot be changed in software.

---

## How It Works (Why P106-100 Needed `0x2241C`)

Unlocking PCIe Gen2 on Linux requires solving **two** separate locks:

1. **Hardware capability declaration (`gen2kr_p106.ko` via `kretprobe` on `nv_pci_probe`):**
   - On cold boot, Pascal mining cards advertise only `2.5 GT/s` (`LNKCAP2 = 0x00000002`, `EXP_HEAD.VER = 1`).
   - Specifically on cards with cold-boot `0x8841C = 0xC0B46500` (such as Gigabyte P106-100 `10DE:1C07`), unlocking `LNKCAP2` from `0x02 → 0x06` requires setting **all** of:
     - XP3G overrides (`0x8E110=1`, `0x8E11C=4`, `0x8E120=0`, `0x8E12C=0x200000`)
     - `0x88610 = (v & ~(1<<12)) | 1` & `0x8860C |= 1` (switches `EXP_HEAD.VER` from `1 → 2`)
     - **`0x2241C` (`PUNITS.PCI`) `|= 0x81`** (live `PCIE_SPEED=FULL` + `PCIE_VERSION=2`, guarded against poison/non-zero upper 24 bits)
     - `0x8841C` (`bit15=1`, `bit11=1`, downgrade gate `[14:13]=0`, `bit12=0`; fallback `bit13=1`)
     - `0x8C040` `SPEED[19:18] = 1` (`5.0 GT/s` in Pascal encoding — **NEVER** sets `bit0 COMMIT`)
     - `0x8872C = 6` → `LNKCAP2` flips to `0x00000006` → Endpoint & Root Port `LNKCTL2.TLS = 2` → Root Port Link Retrain.
2. **Preventing the NVIDIA driver from downgrading back to Gen1 (`01-patch-driver-gen2.py`):**
   - Every time an app opens CUDA/Vulkan/GL (`rm_init_adapter`), NVIDIA RM calls `set_link_speed(mode=1)` and forces `0x8C040.SPEED=2` (Gen1).
   - A **2-byte patch** shifts the entry range check (`lea -1` → `lea -2`, `cmp $2` → `cmp $1`) so `mode==1` bails cleanly returning `NV_OK`, leaving the link permanently at Gen2 without needing aggressive polling.

---

## Requirements

- **Proprietary (`closed`) NVIDIA Linux driver** (tested on 580.xx series; supports `.ko`, `.ko.xz` with CRC32, `.ko.zst`, `.ko.gz`).
- **Kernel headers & build tools** to compile `gen2kr_p106.ko`:
  - Fedora / RHEL: `sudo dnf install kernel-devel gcc make psmisc`
  - Debian / Ubuntu: `sudo apt install linux-headers-$(uname -r) build-essential psmisc`
  - Arch Linux: `sudo pacman -S linux-headers base-devel psmisc`
- **Secure Boot disabled** and kernel with `CONFIG_KRETPROBES=y` (default on mainstream distros).
- **Primary display on iGPU or another GPU** (recommended; P106-100 is headless).

---

## Step-by-Step Usage

```bash
git clone https://github.com/duytoram1993-alt/unlock-pcie-p106-100-linux.git
cd unlock-pcie-p106-100-linux
```

### Step 1: Build the kernel module & run the read-only BAR0 fingerprint probe

```bash
# Compile gen2kr_p106.ko (regular user, no sudo)
bash 02-push-gen2.sh build

# Read-only check: verifies BAR0_FINGERPRINT=MATCH without writing any register
sudo python3 00-probe-bar0.py
```

**STOP if output shows `BAR0_FINGERPRINT=MISMATCH`!** Only proceed when you see `BAR0_FINGERPRINT=MATCH`.

### Step 2: Apply the 2-byte anti-downgrade patch to `nvidia.ko`

```bash
bash 01-patch-driver-gen2.sh check
sudo bash 01-patch-driver-gen2.sh apply
```

*(If your distro embeds `nvidia.ko` inside initramfs — e.g. Ubuntu/Debian/Arch with early KMS — rebuild initramfs with `update-initramfs -u` or `mkinitcpio -P`.)*

### Step 3: Install the boot service (recommended) & reboot

Running via the boot service ensures PCIe Gen2 is pushed **before** `nvidia-powerd` or your display manager opens `/dev/nvidia*` (avoiding kernel unbind hangs):

```bash
sudo bash 03-install-boot-service.sh install
sudo reboot
```

### Step 4: Verify true PCIe Gen2 status

> **NEVER trust `nvidia-smi` for PCIe Gen!** `nvidia-smi` reports cached driver values and may still show Gen1 when the hardware is running at Gen2. Always verify hardware registers or sysfs:

```bash
sudo bash 02-push-gen2.sh status
# Or directly from sysfs:
cat /sys/bus/pci/devices/0000:01:00.0/current_link_speed
# Expected: 5.0 GT/s PCIe
```

---

## Safety Rules Built Into This Tool

1. **Never writes `0x8C040` bit0 (`COMMIT`):** Writing `COMMIT` externally drops the GPU off the PCIe bus (`ZOMBIE` state requiring a full power cycle). Both `ret_handler` and `guard_fn` double-check `lk != 0xFFFFFFFF && (lk & 1) == 0`.
2. **Never unbinds a busy GPU:** `02-push-gen2.sh` refuses to `unbind` if any process holds `/dev/nvidia*`, preventing uninterruptible kernel D-state hangs.
3. **Read-only fingerprint gate:** `gen2kr_p106.ko` verifies `BAR0 0x88084 == LNKCAP` and `0x880A4 == LNKCAP2` before registering the kretprobe.
4. **No infinite retrain spam:** `guard_fn` automatically stops if `LNKCAP2 != 0x06` or after 5 consecutive failed retrains, and `02-push-gen2.sh` unloads the module cleanly if Gen2 fails.
5. **SELinux compatible:** Installs `gen2kr_p106.ko` under `/lib/modules/<kver>/extra/p106-gen2/` (`modules_object_t`) and script under `/usr/local/sbin/` (`bin_t`) so Fedora/RHEL enforcing SELinux boots cleanly.

---

## Maintenance & Rollback

### After a kernel or NVIDIA driver update
```bash
sudo bash 01-patch-driver-gen2.sh apply
bash 02-push-gen2.sh build
sudo bash 03-install-boot-service.sh install
sudo reboot
```

### Complete Uninstall / Rollback
```bash
sudo bash 02-push-gen2.sh stop
sudo bash 03-install-boot-service.sh uninstall
sudo bash 01-patch-driver-gen2.sh revert
sudo reboot
```

---

## Credits & License

This project builds on reverse-engineering and Linux/Windows Pascal unlock research from:
- [WJNKAC/nvidia-p104-unlock-gen2-for-linux](https://github.com/WJNKAC/nvidia-p104-unlock-gen2-for-linux) — Linux `kretprobe` architecture (`gen2kr.c`), 2-byte `set_link_speed` patch, and safety methodology
- [DaZuiZZZZ/P106-Unlock-Gen2](https://github.com/DaZuiZZZZ/P106-Unlock-Gen2) — P106-100 / P106-090 BAR0 mirror fingerprint validation and register baselines
- `skbskhud/p104-gen2-unlock` — `0x2241C` (`PUNITS.PCI`) & `0x8841C[14:13]` / `0x8872C` hardware state matrix

Licensed under the **GNU General Public License v2.0** ([LICENSE](LICENSE)). Use at your own risk; direct PCIe/BAR0 register manipulation may cause instability or require a reboot on incompatible hardware batches.
