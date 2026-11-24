#!/usr/bin/env bash
# ==============================================================================
# 03-install-boot-service.sh - Install gen2kr_p106.ko into /lib/modules (SELinux
#                              compliant) and enable a systemd oneshot service
#                              that pushes PCIe Gen2 at boot BEFORE display
#                              managers and nvidia-powerd open /dev/nvidia*.
#
# Usage:
#   sudo bash 03-install-boot-service.sh install
#   sudo bash 03-install-boot-service.sh uninstall
#   sudo bash 03-install-boot-service.sh status
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNIT_NAME="p106-gen2.service"
UNIT_PATH="/etc/systemd/system/${UNIT_NAME}"
SYS_SCRIPT="/usr/local/sbin/p106-push-gen2.sh"
KVER="$(uname -r)"
SYS_KO_DIR="/lib/modules/${KVER}/extra/p106-gen2"

ACTION="${1:-}"

case "${ACTION}" in
  install)
    [ "$(id -u)" -eq 0 ] || { echo "[ERROR] Root privileges (sudo) required." >&2; exit 1; }

    # 1. Install gen2kr_p106.ko into /lib/modules/<kver>/extra/p106-gen2/ (SELinux modules_object_t)
    echo "=== Installing gen2kr_p106.ko into ${SYS_KO_DIR} ==="
    bash "${SCRIPT_DIR}/02-push-gen2.sh" install-kmod

    # 2. Install push script to /usr/local/sbin/ (SELinux bin_t)
    install -m 0755 "${SCRIPT_DIR}/02-push-gen2.sh" "${SYS_SCRIPT}"
    if command -v restorecon >/dev/null 2>&1; then
      restorecon -v "${SYS_SCRIPT}" >/dev/null 2>&1 || true
    fi

    # 3. Create systemd unit ordered strictly before NVIDIA daemons and Display Managers
    cat > "${UNIT_PATH}" <<EOF
[Unit]
Description=NVIDIA P106-100 PCIe Gen2 Unlock (before nvidia-powerd & Display Manager)
After=systemd-modules-load.service systemd-udev-settle.service
Before=nvidia-powerd.service nvidia-persistenced.service display-manager.service sddm.service gdm.service lightdm.service plasmalogin.service ly.service
StartLimitBurst=2
StartLimitIntervalSec=60

[Service]
Type=oneshot
RemainAfterExit=yes
TimeoutStartSec=30
Environment=P106_GEN2_DIR=${SCRIPT_DIR}
ExecStart=${SYS_SCRIPT} start
ExecStop=${SYS_SCRIPT} stop

[Install]
WantedBy=multi-user.target
EOF
    if command -v restorecon >/dev/null 2>&1; then
      restorecon -v "${UNIT_PATH}" >/dev/null 2>&1 || true
    fi
    systemctl daemon-reload
    systemctl enable "${UNIT_NAME}"
    echo "[OK] Installed ${SYS_KO_DIR}/gen2kr_p106.ko and enabled ${UNIT_PATH}"
    echo "Reboot now so ${UNIT_NAME} runs cleanly before your display manager starts."
    ;;
  uninstall)
    [ "$(id -u)" -eq 0 ] || { echo "[ERROR] Root privileges (sudo) required." >&2; exit 1; }
    systemctl disable --now "${UNIT_NAME}" 2>/dev/null || true
    rm -f "${UNIT_PATH}" "${SYS_SCRIPT}"
    rm -rf "${SYS_KO_DIR}"
    depmod -a "${KVER}" || true
    systemctl daemon-reload
    echo "[OK] Removed ${UNIT_NAME}, ${SYS_SCRIPT}, and ${SYS_KO_DIR}."
    ;;
  status)
    systemctl status "${UNIT_NAME}" --no-pager || true
    ;;
  *)
    echo "Usage: sudo bash $0 {install|uninstall|status}"
    exit 2
    ;;
esac
