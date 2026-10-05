@echo off
REM SPDX-License-Identifier: MIT
REM install-lora-multiboot.bat -- Windows wrapper for the unified installer
REM
REM This batch file calls the Python flash tool directly with a unified interface.
REM For full features, use the bash script in Git Bash / WSL, or call python tools/flash.py directly.

setlocal enabledelayedexpansion

set ROOT_DIR=%~dp0
set TOOLS_DIR=%ROOT_DIR%tools\
set FLASH_TOOL=%TOOLS_DIR%flash.py
set PARTITION_DIR=%ROOT_DIR%firmware\partitions\

set TABLE=quadboot
set PORT=
set BAUD=460800
set FLASH_SIZE=16
set DRY_RUN=
set BOOTLOADER=
set PART_TABLE_BIN=
set OTADATA=
set MESHCORE=
set MESHTASTIC=
set UPDATE_BOOTLOADER=
set UPDATE_PART_TABLE=
set LIST_ONLY=
set ERASE_SLOT=
set ERASE_SETTINGS_SLOT=
set BOOT_SLOT=
set APPS=

:parse_args
if "%~1"=="" goto :check_action
set ARG=%~1
shift

if "%ARG%"=="--table" set TABLE=%~1&shift&goto :parse_args
if "%ARG%"=="--port" set PORT=%~1&shift&goto :parse_args
if "%ARG%"=="--baud" set BAUD=%~1&shift&goto :parse_args
if "%ARG%"=="--flash-size" set FLASH_SIZE=%~1&shift&goto :parse_args
if "%ARG%"=="--dry-run" set DRY_RUN=--dry-run&goto :parse_args
if "%ARG%"=="--bootloader" set BOOTLOADER=%~1&shift&goto :parse_args
if "%ARG%"=="--part-table-bin" set PART_TABLE_BIN=%~1&shift&goto :parse_args
if "%ARG%"=="--otadata" set OTADATA=%~1&shift&goto :parse_args
if "%ARG%"=="--meshcore" set MESHCORE=%~1&shift&goto :parse_args
if "%ARG%"=="--meshtastic" set MESHTASTIC=%~1&shift&goto :parse_args
if "%ARG%"=="--update-bootloader" set UPDATE_BOOTLOADER=%~1&shift&goto :parse_args
if "%ARG%"=="--update-part-table" set UPDATE_PART_TABLE=%~1&shift&goto :parse_args
if "%ARG%"=="--list" set LIST_ONLY=1&goto :parse_args
if "%ARG%"=="--erase" set ERASE_SLOT=%~1&shift&goto :parse_args
if "%ARG%"=="--erase-settings" set ERASE_SETTINGS_SLOT=%~1&shift&goto :parse_args
if "%ARG%"=="--boot" set BOOT_SLOT=%~1&shift&goto :parse_args
if "%ARG%"=="--app" (
    set APPS=!APPS! --app %~1
    shift
    goto :parse_args
)
if "%ARG%"=="-h" goto :usage
if "%ARG%"=="--help" goto :usage

echo Unknown option: %ARG%
goto :usage

:check_action
if "%TABLE%" neq "quadboot" if "%TABLE%" neq "dualboot" (
    echo Table must be 'quadboot' or 'dualboot'
    exit /b 1
)

if not exist "%PARTITION_DIR%\%TABLE%.csv" (
    echo Partition table not found: %PARTITION_DIR%\%TABLE%.csv
    exit /b 1
)

echo [INFO] lora-multiboot installer (Windows)
echo [INFO] Table: %TABLE%, Flash: %FLASH_SIZE%MB, Port: %PORT%

if defined LIST_ONLY (
    python "%FLASH_TOOL%" --table "%TABLE%" %PORT_ARG% --baud "%BAUD%" --flash-size "%FLASH_SIZE%" list %DRY_RUN%
    exit /b 0
)

REM Build port argument
set PORT_ARG=
if defined PORT set PORT_ARG=--port "%PORT%"

if defined UPDATE_BOOTLOADER if defined UPDATE_PART_TABLE (
    echo Both --update-bootloader and --update-part-table specified. Use one at a time.
    exit /b 1
)

if defined UPDATE_BOOTLOADER (
    if not exist "%UPDATE_BOOTLOADER%" (
        echo Bootloader not found: %UPDATE_BOOTLOADER%
        exit /b 1
    )
    echo [INFO] Updating bootloader only
    python "%FLASH_TOOL%" --table "%TABLE%" %PORT_ARG% --baud "%BAUD%" --flash-size "%FLASH_SIZE%" update-system --bootloader "%UPDATE_BOOTLOADER%" %DRY_RUN%
    exit /b 0
)

if defined UPDATE_PART_TABLE (
    if not exist "%UPDATE_PART_TABLE%" (
        echo Partition table not found: %UPDATE_PART_TABLE%
        exit /b 1
    )
    echo [INFO] Updating partition table only
    python "%FLASH_TOOL%" --table "%TABLE%" %PORT_ARG% --baud "%BAUD%" --flash-size "%FLASH_SIZE%" update-system --part-table-bin "%UPDATE_PART_TABLE%" %DRY_RUN%
    exit /b 0
)

if defined ERASE_SLOT (
    echo [WARN] Erasing firmware in slot %ERASE_SLOT% (settings preserved)
    python "%FLASH_TOOL%" --table "%TABLE%" %PORT_ARG% --baud "%BAUD%" --flash-size "%FLASH_SIZE%" app %ERASE_SLOT% --erase %DRY_RUN%
    exit /b 0
)

if defined ERASE_SETTINGS_SLOT (
    echo [ERR] DESTRUCTIVE: This erases firmware AND settings (channel keys, node database)
    echo [ERR] This CANNOT be undone. The node's identity on the mesh will be lost.
    set /p CONFIRM=Type 'ERASE SETTINGS' to confirm: 
    if not "%CONFIRM%"=="ERASE SETTINGS" (
        echo Aborted
        exit /b 1
    )
    python "%FLASH_TOOL%" --table "%TABLE%" %PORT_ARG% --baud "%BAUD%" --flash-size "%FLASH_SIZE%" app %ERASE_SETTINGS_SLOT% --erase-settings %DRY_RUN%
    exit /b 0
)

if defined BOOT_SLOT (
    echo [INFO] Boot slot selection requires firmware CLI.
    echo [INFO] Use 'python tools/flash.py app %BOOT_SLOT%' after firmware boots.
    exit /b 0
)

REM Handle convenience flags
set APP_ARGS=
if defined MESHCORE (
    if not exist "%MESHCORE%" (
        echo MeshCore firmware not found: %MESHCORE%
        exit /b 1
    )
    set APP_ARGS=!APP_ARGS! --app ota_0=%MESHCORE%
    echo [INFO] Will provision slot 0 (MeshCore) from %MESHCORE%
)
if defined MESHTASTIC (
    if not exist "%MESHTASTIC%" (
        echo Meshtastic firmware not found: %MESHTASTIC%
        exit /b 1
    )
    set APP_ARGS=!APP_ARGS! --app ota_1=%MESHTASTIC%
    echo [INFO] Will provision slot 1 (Meshtastic) from %MESHTASTIC%
)
if defined APPS set APP_ARGS=!APP_ARGS! %APPS%

if defined BOOTLOADER if defined PART_TABLE_BIN (
    if not exist "%BOOTLOADER%" (
        echo Bootloader not found: %BOOTLOADER%
        exit /b 1
    )
    if not exist "%PART_TABLE_BIN%" (
        echo Partition table not found: %PART_TABLE_BIN%
        exit /b 1
    )
    echo [INFO] First-time full flash: bootloader + partition table + slots
    set FULL_ARGS=--bootloader "%BOOTLOADER%" --part-table-bin "%PART_TABLE_BIN%"
    if defined OTADATA (
        if not exist "%OTADATA%" (
            echo OTA data not found: %OTADATA%
            exit /b 1
        )
        set FULL_ARGS=!FULL_ARGS! --otadata "%OTADATA%"
    )
    python "%FLASH_TOOL%" --table "%TABLE%" %PORT_ARG% --baud "%BAUD%" --flash-size "%FLASH_SIZE%" full !FULL_ARGS! !APP_ARGS! %DRY_RUN%
    echo [OK] Full flash complete
    exit /b 0
)

if defined APP_ARGS (
    echo [INFO] Provisioning slots...
    for %%A in (!APP_ARGS!) do (
        REM This is a simplified loop - in practice each --app needs separate invocation
        echo [INFO] Use separate invocations for multiple slots, or use the bash script
    )
    echo [INFO] For multiple slots, run this script once per slot, or use the bash version
    exit /b 1
)

:usage
echo.
echo Usage: install-lora-multiboot.bat [OPTIONS]
echo.
echo Options:
echo   --table TABLE         Partition table: quadboot (default) or dualboot
echo   --port PORT           Serial port (auto-detected if omitted)
echo   --baud BAUD           Baud rate (default: 460800)
echo   --flash-size MB       Flash size in MB, or 0 to ask the chip (default: 16)
echo   --dry-run             Validate and print plan, write nothing
echo   --bootloader FILE     Bootloader binary for first-time setup
echo   --part-table-bin FILE Partition table binary for first-time setup
echo   --otadata FILE        OTA data partition binary for first-time setup
echo   --meshcore FILE       MeshCore firmware for slot 0 (convenience)
echo   --meshtastic FILE     Meshtastic firmware for slot 1 (convenience)
echo   --update-bootloader FILE  Update only the bootloader
echo   --update-part-table FILE  Update only the partition table
echo   --list                Show layout and exit
echo   --erase SLOT          Erase slot firmware, keep settings
echo   --erase-settings SLOT Erase slot firmware AND settings (destructive)
echo   --boot SLOT           Set default boot slot (requires firmware CLI)
echo   -h, --help            Show this help
echo.
echo Examples:
echo   First-time setup:
echo     install-lora-multiboot.bat --bootloader boot.bin --part-table-bin part.bin --meshcore meshcore.bin --meshtastic meshtastic.bin
echo.
echo   Add Meshtastic to existing board:
echo     install-lora-multiboot.bat --meshtastic meshtastic.bin
echo.
echo   Update bootloader only:
echo     install-lora-multiboot.bat --update-bootloader new_boot.bin
echo.
echo   List layout:
echo     install-lora-multiboot.bat --list
exit /b 0