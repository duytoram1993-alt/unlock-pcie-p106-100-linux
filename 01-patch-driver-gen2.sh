#!/usr/bin/env bash
# ==============================================================================
# 01-patch-driver-gen2.sh - Wrapper for 01-patch-driver-gen2.py
#
# Usage:
#   bash 01-patch-driver-gen2.sh check [--ko /path/to/nvidia.ko*]
#   sudo bash 01-patch-driver-gen2.sh apply [--ko /path/to/nvidia.ko*]
#   sudo bash 01-patch-driver-gen2.sh revert [--ko /path/to/nvidia.ko*]
#   sudo bash 01-patch-driver-gen2.sh restore-orig [--ko /path/to/nvidia.ko*]
# ==============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCHER="${SCRIPT_DIR}/01-patch-driver-gen2.py"

ACTION="${1:-}"
case "${ACTION}" in
  check|apply|revert|restore-orig)
    shift
    exec python3 "${PATCHER}" "${ACTION}" "$@"
    ;;
  *)
    echo "Usage: [sudo] bash $0 {check|apply|revert|restore-orig} [--ko /path/to/nvidia.ko*]"
    exit 2
    ;;
esac
