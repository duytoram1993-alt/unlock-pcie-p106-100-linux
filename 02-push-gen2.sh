#!/usr/bin/env bash
# ==============================================================================
# 02-push-gen2.sh - Build, install (SELinux-safe), load gen2kr_p106.ko, and
#                   push NVIDIA P106-100 / Pascal mining GPU to PCIe Gen2 (5.0 GT/s)
#
# MANDATORY SAFETY RULE:
#   NEVER unbind/bind the GPU while any process holds /dev/nvidia* (display
#   manager, browser, Steam, CUDA). This script hard-blocks if /dev/nvidia*
#   is occupied to prevent an uninterruptible D-state kernel hang.
#
# Usage:
#   bash 02-push-gen2.sh build          # Compile gen2kr_p106.ko (no sudo needed)
#   sudo bash 02-push-gen2.sh status    # Inspect module, driver patch, & PCIe link
#   sudo bash 02-push-gen2.sh start     # Load module & push Gen2 (when GPU is idle)
#   sudo bash 02-push-gen2.sh stop      # Unload gen2kr_p106
#
# Optional environment variables:
#   DEV_ID=0x1c07           Override PCI Device ID (auto-detected if unset)
#   TARGET_BDF=0000:01:00.0 Override PCI BDF (auto-detected if unset)
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="${P106_GEN2_DIR:-${SCRIPT_DIR}}"
KMOD_DIR="${REPO_DIR}/kmod"
BUILD_KO="${KMOD_DIR}/gen2kr_p106.ko"
KVER="$(uname -r)"
SYS_KO_DIR="/lib/modules/${KVER}/extra/p106-gen2"
SYS_KO="${SYS_KO_DIR}/gen2kr_p106.ko"
PROBE_PY="${REPO_DIR}/00-probe-bar0.py"
PATCHER="${REPO_DIR}/01-patch-driver-gen2.py"

DEV_ID="${DEV_ID:-}"
TARGET_BDF="${TARGET_BDF:-}"

detect_gpu() {
  if [ -n "${TARGET_BDF}" ] && [ -r "/sys/bus/pci/devices/${TARGET_BDF}/device" ]; then
    if [ -z "${DEV_ID}" ]; then
      DEV_ID="$(tr -d '[:space:]' < "/sys/bus/pci/devices/${TARGET_BDF}/device" | tr '[:upper:]' '[:lower:]')"
    fi
    return 0
  fi

  local want_ids=("1c07" "1c06" "1c09" "1b87")
  if [ -n "${DEV_ID}" ]; then
    local clean="${DEV_ID#0x}"
    want_ids=("${clean,,}")
  fi

  local id d v dev
  for id in "${want_ids[@]}"; do
    for d in /sys/bus/pci/devices/*; do
      [ -r "$d/vendor" ] && [ -r "$d/device" ] || continue
      v="$(tr -d '[:space:]' < "$d/vendor" | tr '[:upper:]' '[:lower:]')"
      dev="$(tr -d '[:space:]' < "$d/device" | tr '[:upper:]' '[:lower:]')"
      if [ "$v" = "0x10de" ] && [ "$dev" = "0x${id}" ]; then
        TARGET_BDF="$(basename "$d")"
        DEV_ID="0x${id}"
        return 0
      fi
    done
  done

  echo "[ERROR] No supported NVIDIA Pascal mining GPU found (10de:1c07/1c06/1c09/1b87)." >&2
  echo "        Pass DEV_ID=0xXXXX TARGET_BDF=0000:xx:xx.0 if needed." >&2
  return 1
}

ko_matches_kernel() {
  local ko="$1"
  [ -f "${ko}" ] || return 1
  local vermagic
  vermagic="$(modinfo -F vermagic "${ko}" 2>/dev/null || true)"
  [[ "${vermagic}" == "${KVER}"* ]]
}

do_build() {
  echo "=== Building gen2kr_p106.ko for kernel ${KVER} ==="
  make -C "${KMOD_DIR}" clean >/dev/null 2>&1 || true
  make -C "${KMOD_DIR}" all
  ls -la "${BUILD_KO}"
}

install_kmod_sys() {
  # Install into /lib/modules/<kver>/extra/p106-gen2/ so modprobe & SELinux kmod_t allow boot loading
  if ! ko_matches_kernel "${BUILD_KO}"; then
    if [ -n "${SUDO_USER:-}" ]; then
      su -s /bin/bash "${SUDO_USER}" -c "make -C '${KMOD_DIR}' clean >/dev/null 2>&1 || true; make -C '${KMOD_DIR}' all"
    else
      do_build
    fi
  fi
  install -D -m 0644 "${BUILD_KO}" "${SYS_KO}"
  if command -v restorecon >/dev/null 2>&1; then
    restorecon -Rv "${SYS_KO_DIR}" >/dev/null 2>&1 || true
  fi
  depmod -a "${KVER}"
}

do_status() {
  detect_gpu
  echo "=== Loaded Kernel Modules ==="
  lsmod | grep -E '^gen2kr_p106|^nvidia' || true
  echo "=== Driver Patch Status (set_link_speed) ==="
  python3 "${PATCHER}" check || true
  echo "=== PCIe Link (${TARGET_BDF}, ${DEV_ID}) ==="
  printf "  current_link_speed = %s\n" "$(cat "/sys/bus/pci/devices/${TARGET_BDF}/current_link_speed")"
  printf "  current_link_width = x%s\n" "$(cat "/sys/bus/pci/devices/${TARGET_BDF}/current_link_width")"
  if [ "$(id -u)" -eq 0 ]; then
    echo "=== BAR0 & PCIe Config Probe ==="
    TARGET_BDF="${TARGET_BDF}" python3 "${PROBE_PY}" || true
  fi
}

check_gpu_idle() {
  if ! command -v fuser >/dev/null 2>&1; then
    echo "[WARN] 'fuser' not installed; checking /proc/*/fd for /dev/nvidia* holders..." >&2
    local p
    p="$(ls -l /proc/[0-9]*/fd/* 2>/dev/null | grep '/dev/nvidia' || true)"
    if [ -n "${p}" ]; then
      echo "[SAFETY BLOCK] Processes are currently holding /dev/nvidia*:" >&2
      echo "${p}" >&2
      return 1
    fi
    return 0
  fi
  local users=""
  users="$(fuser /dev/nvidia* 2>/dev/null || true)"
  if [ -n "${users}" ]; then
    echo "[SAFETY BLOCK] Processes are currently holding /dev/nvidia*:" >&2
    fuser -v /dev/nvidia* 2>&1 || true
    echo "" >&2
    echo "Refusing to unbind GPU while occupied (prevents kernel D-state hang)." >&2
    echo "Either install the boot service (runs before display manager):" >&2
    echo "  sudo bash ${REPO_DIR}/03-install-boot-service.sh install && sudo reboot" >&2
    echo "Or switch to a TTY (Ctrl+Alt+F3), stop your display manager, push, and restart it:" >&2
    echo "  sudo systemctl stop display-manager" >&2
    echo "  sudo bash ${REPO_DIR}/02-push-gen2.sh start" >&2
    echo "  sudo systemctl start display-manager" >&2
    return 1
  fi
  return 0
}

do_start() {
  [ "$(id -u)" -eq 0 ] || { echo "[ERROR] Root privileges (sudo) required." >&2; exit 1; }
  detect_gpu

  # 1. Ensure up-to-date module is installed under /lib/modules/<kver>/extra/p106-gen2/
  if ! ko_matches_kernel "${SYS_KO}" || { [ -f "${BUILD_KO}" ] && [ "${BUILD_KO}" -nt "${SYS_KO}" ]; }; then
    install_kmod_sys
  fi

  # 2. Ensure nvidia is loaded so kallsyms exposes nv_pci_probe
  modprobe nvidia
  modprobe nvidia_uvm 2>/dev/null || true

  # 2b. Temporarily pause nvidia-powerd if active (it holds /dev/nvidiactl)
  local powerd_was_active=0
  if command -v systemctl >/dev/null 2>&1 && systemctl is-active --quiet nvidia-powerd.service 2>/dev/null; then
    powerd_was_active=1
    systemctl stop nvidia-powerd.service || true
    sleep 0.3
  fi

  # 3. Hard-verify GPU is completely idle before unbind/bind
  if ! check_gpu_idle; then
    [ "${powerd_was_active}" -eq 1 ] && systemctl start nvidia-powerd.service 2>/dev/null || true
    return 1
  fi

  # 4. Reload gen2kr_p106 cleanly (resets 'triggered' flag)
  if lsmod | grep -q '^gen2kr_p106 '; then
    rmmod gen2kr_p106
  fi

  echo "=== Loading gen2kr_p106 (dev_id=${DEV_ID}) ==="
  modprobe gen2kr_p106 "dev_id=${DEV_ID}"

  echo "=== Triggering nv_pci_probe (unbind -> sleep 1 -> bind) on ${TARGET_BDF} ==="
  if [ -e "/sys/bus/pci/drivers/nvidia/${TARGET_BDF}" ]; then
    echo "${TARGET_BDF}" > /sys/bus/pci/drivers/nvidia/unbind
    sleep 1
  fi
  echo "${TARGET_BDF}" > /sys/bus/pci/drivers/nvidia/bind

  # 5. Poll up to 10 seconds for Gen2 (5.0 GT/s)
  echo "=== Waiting for PCIe Gen2 link ==="
  local spd=""
  for _ in $(seq 1 20); do
    sleep 0.5
    spd="$(cat "/sys/bus/pci/devices/${TARGET_BDF}/current_link_speed" 2>/dev/null || true)"
    if [[ "${spd}" == "5.0 GT/s"* ]]; then
      break
    fi
  done

  if [ "${powerd_was_active}" -eq 1 ]; then
    systemctl start nvidia-powerd.service 2>/dev/null || true
  fi

  echo "=== Result ==="
  echo "  current_link_speed = ${spd}"
  echo "  current_link_width = x$(cat "/sys/bus/pci/devices/${TARGET_BDF}/current_link_width")"
  dmesg | grep gen2kr_p106 | tail -n 20 || true

  if [[ "${spd}" == "5.0 GT/s"* ]]; then
    echo "[SUCCESS] GPU ${TARGET_BDF} (${DEV_ID}) is now running at PCIe Gen2 (5.0 GT/s)!"
    return 0
  else
    if lsmod | grep -q '^gen2kr_p106 '; then
      rmmod gen2kr_p106 2>/dev/null || true
    fi
    echo "[WARN] Link did not reach 5.0 GT/s (gen2kr_p106 unloaded). Inspect dmesg output above." >&2
    return 1
  fi
}

do_stop() {
  [ "$(id -u)" -eq 0 ] || { echo "[ERROR] Root privileges (sudo) required." >&2; exit 1; }
  if lsmod | grep -q '^gen2kr_p106 '; then
    rmmod gen2kr_p106
    echo "[OK] Unloaded gen2kr_p106."
  else
    echo "gen2kr_p106 is not currently loaded."
  fi
}

ACTION="${1:-}"
case "${ACTION}" in
  build)        do_build ;;
  install-kmod) [ "$(id -u)" -eq 0 ] || { echo "[ERROR] Root privileges (sudo) required." >&2; exit 1; }; install_kmod_sys ;;
  status)       do_status ;;
  start)        do_start ;;
  stop)         do_stop ;;
  *)
    echo "Usage: [sudo] bash $0 {build|install-kmod|status|start|stop}"
    exit 2
    ;;
esac
