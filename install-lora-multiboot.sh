#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# install-lora-multiboot.sh -- one script to flash a Heltec V4 from blank to multi-slot
#
# Usage:
#   ./install-lora-multiboot.sh [OPTIONS]
#
# Options:
#   --table TABLE         Partition table: quadboot (default) or dualboot
#   --port PORT           Serial port (auto-detected if omitted)
#   --baud BAUD           Baud rate (default: 460800)
#   --flash-size MB       Flash size in MB, or 0 to ask the chip (default: 16)
#   --dry-run             Validate and print plan, write nothing
#   --bootloader FILE     Bootloader binary for first-time setup
#   --part-table-bin FILE Partition table binary for first-time setup
#   --otadata FILE        OTA data partition binary for first-time setup
#   --app SLOT=FILE       Firmware image for slot (repeatable)
#   --meshcore FILE       MeshCore firmware for slot 0 (convenience)
#   --meshtastic FILE     Meshtastic firmware for slot 1 (convenience)
#   --update-bootloader FILE  Update only the bootloader
#   --update-part-table FILE  Update only the partition table
#   --list                Show layout and exit
#   --erase SLOT          Erase slot firmware, keep settings
#   --erase-settings SLOT Erase slot firmware AND settings (destructive)
#   --boot SLOT           Set default boot slot
#   -h, --help            Show this help
#
# Examples:
#   # First-time setup with both firmwares
#   ./install-lora-multiboot.sh --bootloader boot.bin --part-table-bin part.bin --meshcore meshcore.bin --meshtastic meshtastic.bin
#
#   # Add Meshtastic to a board that already has MeshCore
#   ./install-lora-multiboot.sh --meshtastic meshtastic.bin
#
#   # Update bootloader only (preserves all slots and settings)
#   ./install-lora-multiboot.sh --update-bootloader new_boot.bin
#
#   # List what's on the board
#   ./install-lora-multiboot.sh --list
#
#   # Reflash Meshtastic slot without losing settings
#   ./install-lora-multiboot.sh --app 1=new_meshtastic.bin
#
#   # Erase a bad MeshCore image but keep channel keys
#   ./install-lora-multiboot.sh --erase 0

set -euo pipefail

# Trap for cleanup on interrupt
trap 'log_warn "Interrupted"; exit 130' INT TERM

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
TOOLS_DIR="$ROOT_DIR/tools"
PARTITION_DIR="$ROOT_DIR/firmware/partitions"
FLASH_TOOL="$TOOLS_DIR/flash.py"

# Defaults
TABLE="quadboot"
PORT=""
BAUD="460800"
FLASH_SIZE="16"
DRY_RUN=false
BOOTLOADER=""
PART_TABLE_BIN=""
OTADATA=""
APPS=()
MESHCORE=""
MESHTASTIC=""
UPDATE_BOOTLOADER=""
UPDATE_PART_TABLE=""
LIST_ONLY=false
ERASE_SLOT=""
ERASE_SETTINGS_SLOT=""
BOOT_SLOT=""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() { echo -e "${BLUE}[INFO]${NC} $*"; }
log_ok() { echo -e "${GREEN}[OK]${NC} $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_err() { echo -e "${RED}[ERR]${NC} $*" >&2; }

usage() {
    sed -n '2,45p' "$0" | sed 's/^# \?//'
    exit 0
}

die() { log_err "$*"; exit 1; }

require_file() {
    local file="$1" what="$2"
    [[ -f "$file" ]] || die "$what: no such file: $file"
    [[ -r "$file" ]] || die "$what: cannot read: $file"
}

check_esptool() {
    command -v esptool >/dev/null 2>&1 || command -v esptool.py >/dev/null 2>&1 || \
        die "esptool not found. Install with: pip install esptool"
}

run_flash_tool() {
    local args=("$@")
    local dry_run_flag=()
    [[ "$DRY_RUN" == true ]] && dry_run_flag=("--dry-run")
    local port_arg=()
    [[ -n "$PORT" ]] && port_arg=(--port "$PORT")
    python3 "$FLASH_TOOL" --table "$TABLE" "${dry_run_flag[@]}" "${port_arg[@]}" --baud "$BAUD" --flash-size "$FLASH_SIZE" "${args[@]}"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --table) TABLE="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --baud) BAUD="$2"; shift 2 ;;
        --flash-size) FLASH_SIZE="$2"; shift 2 ;;
        --dry-run) DRY_RUN=true; shift ;;
        --bootloader) BOOTLOADER="$2"; shift 2 ;;
        --part-table-bin) PART_TABLE_BIN="$2"; shift 2 ;;
        --otadata) OTADATA="$2"; shift 2 ;;
        --app) APPS+=("$2"); shift 2 ;;
        --meshcore) MESHCORE="$2"; shift 2 ;;
        --meshtastic) MESHTASTIC="$2"; shift 2 ;;
        --update-bootloader) UPDATE_BOOTLOADER="$2"; shift 2 ;;
        --update-part-table) UPDATE_PART_TABLE="$2"; shift 2 ;;
        --list) LIST_ONLY=true; shift ;;
        --erase) ERASE_SLOT="$2"; shift 2 ;;
        --erase-settings) ERASE_SETTINGS_SLOT="$2"; shift 2 ;;
        --boot) BOOT_SLOT="$2"; shift 2 ;;
        -h|--help) usage ;;
        *) die "Unknown option: $1. Use --help for usage." ;;
    esac
done

# Validate table choice
[[ "$TABLE" == "quadboot" || "$TABLE" == "dualboot" ]] || die "Table must be 'quadboot' or 'dualboot'"

# Validate mutually exclusive operations
op_count=0
[[ -n "$UPDATE_BOOTLOADER" || -n "$UPDATE_PART_TABLE" ]] && ((op_count++))
[[ -n "$ERASE_SLOT" ]] && ((op_count++))
[[ -n "$ERASE_SETTINGS_SLOT" ]] && ((op_count++))
[[ -n "$BOOT_SLOT" ]] && ((op_count++))
[[ -n "$BOOTLOADER" || -n "$PART_TABLE_BIN" || ${#APPS[@]} -gt 0 ]] && ((op_count++))
[[ "$LIST_ONLY" == true ]] && ((op_count++))
[[ $op_count -gt 1 ]] && die "Only one operation at a time. Use --help for usage."

# Validate slot numbers if provided
validate_slot() {
    local slot="$1"
    [[ "$slot" =~ ^[0-9]+$ ]] || die "Slot must be a number, got: $slot"
    if [[ "$TABLE" == "quadboot" ]]; then
        [[ $slot -ge 0 && $slot -le 4 ]] || die "Slot must be 0-4 for quadboot, got: $slot"
    else
        [[ $slot -ge 0 && $slot -le 2 ]] || die "Slot must be 0-2 for dualboot, got: $slot"
    fi
}
[[ -n "$ERASE_SLOT" ]] && validate_slot "$ERASE_SLOT"
[[ -n "$ERASE_SETTINGS_SLOT" ]] && validate_slot "$ERASE_SETTINGS_SLOT"
[[ -n "$BOOT_SLOT" ]] && validate_slot "$BOOT_SLOT"

# Check esptool exists
check_esptool

# Verify partition table exists
[[ -f "$PARTITION_DIR/$TABLE.csv" ]] || die "Partition table not found: $PARTITION_DIR/$TABLE.csv"

log_info "lora-multiboot installer"
log_info "Table: $TABLE, Flash: ${FLASH_SIZE}MB, Port: ${PORT:-auto}"

# --list shows layout and exits
if [[ "$LIST_ONLY" == true ]]; then
    run_flash_tool list
    exit 0
fi

# Handle convenience flags by converting to --app
if [[ -n "$MESHCORE" ]]; then
    require_file "$MESHCORE" "MeshCore firmware"
    APPS+=("ota_0=$MESHCORE")
    log_info "Will provision slot 0 (MeshCore) from $MESHCORE"
fi

if [[ -n "$MESHTASTIC" ]]; then
    require_file "$MESHTASTIC" "Meshtastic firmware"
    APPS+=("ota_1=$MESHTASTIC")
    log_info "Will provision slot 1 (Meshtastic) from $MESHTASTIC"
fi

# --update-bootloader or --update-part-table
if [[ -n "$UPDATE_BOOTLOADER" || -n "$UPDATE_PART_TABLE" ]]; then
    [[ -n "$UPDATE_BOOTLOADER" ]] && require_file "$UPDATE_BOOTLOADER" "Bootloader"
    [[ -n "$UPDATE_PART_TABLE" ]] && require_file "$UPDATE_PART_TABLE" "Partition table"
    log_info "Updating system layer (slots and settings preserved)"
    run_flash_tool update-system \
        ${UPDATE_BOOTLOADER:+--bootloader "$UPDATE_BOOTLOADER"} \
        ${UPDATE_PART_TABLE:+--part-table-bin "$UPDATE_PART_TABLE"} \
        ${OTADATA:+--otadata "$OTADATA"}
    log_ok "System update complete"
    exit 0
fi

# --erase or --erase-settings
if [[ -n "$ERASE_SLOT" ]]; then
    log_warn "Erasing firmware in slot $ERASE_SLOT (settings preserved)"
    run_flash_tool app "$ERASE_SLOT" --erase
    log_ok "Slot $ERASE_SLOT firmware erased, settings kept"
    exit 0
fi

if [[ -n "$ERASE_SETTINGS_SLOT" ]]; then
    log_err "DESTRUCTIVE: This erases firmware AND settings (channel keys, node database)"
    log_err "This CANNOT be undone. The node's identity on the mesh will be lost."
    read -rp "Type 'ERASE SETTINGS' to confirm: " confirm
    [[ "$confirm" == "ERASE SETTINGS" ]] || die "Aborted"
    run_flash_tool app "$ERASE_SETTINGS_SLOT" --erase-settings
    log_ok "Slot $ERASE_SETTINGS_SLOT fully erased"
    exit 0
fi

# --boot
if [[ -n "$BOOT_SLOT" ]]; then
    log_info "Setting default boot slot to $BOOT_SLOT"
    # This requires the device to be running firmware with the CLI
    # For now, we use the flash tool's approach via NVS - would need device-side support
    log_warn "Boot slot selection requires firmware CLI. Use 'python tools/flash.py app $BOOT_SLOT' after firmware boots."
    exit 0
fi

# First-time full flash or slot provisioning
if [[ -n "$BOOTLOADER" || -n "$PART_TABLE_BIN" || ${#APPS[@]} -gt 0 ]]; then
    if [[ -n "$BOOTLOADER" && -n "$PART_TABLE_BIN" ]]; then
        # Full first-time setup
        require_file "$BOOTLOADER" "Bootloader"
        require_file "$PART_TABLE_BIN" "Partition table"
        log_info "First-time full flash: bootloader + partition table + slots"
        
        FLASH_ARGS=("--bootloader" "$BOOTLOADER" "--part-table-bin" "$PART_TABLE_BIN")
        [[ -n "$OTADATA" ]] && { require_file "$OTADATA" "OTA data"; FLASH_ARGS+=("--otadata" "$OTADATA"); }
        
        for app_spec in "${APPS[@]}"; do
            FLASH_ARGS+=("--app" "$app_spec")
        done
        
        run_flash_tool full "${FLASH_ARGS[@]}"
        log_ok "Full flash complete. Board will boot into first provisioned slot."
    else
        # Slot-only operations (requires existing partition table on board)
        for app_spec in "${APPS[@]}"; do
            # Parse slot=file, allowing file paths that contain =
            slot="${app_spec%%=*}"
            file="${app_spec#*=}"
            [[ -n "$slot" && -n "$file" ]] || die "--app expects SLOT=FILE, got $app_spec"
            require_file "$file" "Slot $slot firmware"
            log_info "Writing $file to slot $slot"
            run_flash_tool app "$slot" --app "$file"
        done
        log_ok "Slot provisioning complete"
    fi
    exit 0
fi

# No action specified
usage