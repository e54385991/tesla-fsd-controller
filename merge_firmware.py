"""
PlatformIO extra_script — post-build step.

Produces two files per environment after each successful build:
  firmware_<env>.bin      — merged binary (Web Flasher, flash at 0x0)
  firmware_<env>_ota.bin  — app-only binary (OTA via web panel)
"""

import re
import shutil
import subprocess
import sys
import os
Import("env")


def _resolve_esptool(env, piohome):
    """Return (cmd_prefix, is_v5) compatible with both official PlatformIO and pioarduino."""
    python_exe = env.subst("$PYTHONEXE") or sys.executable
    uploader = env.subst("$UPLOADER").strip().strip('"').strip("'")

    candidates = []
    if uploader and os.path.isfile(uploader):
        candidates.append(uploader)
    for rel in (
        os.path.join("tool-esptool", "esptool.exe"),
        os.path.join("tool-esptool", "esptool"),
        os.path.join("tool-esptoolpy", "esptool.exe"),
        os.path.join("tool-esptoolpy", "esptool"),
        os.path.join("tool-esptoolpy", "esptool.py"),
    ):
        p = os.path.join(piohome, "packages", rel)
        if os.path.isfile(p) and p not in candidates:
            candidates.append(p)

    if not candidates:
        raise Exception("esptool not found in PlatformIO packages")

    tool = candidates[0]
    prefix = [python_exe, tool] if tool.lower().endswith(".py") else [tool]

    is_v5 = not tool.lower().endswith(".py")
    try:
        ver = subprocess.run(prefix + ["version"], capture_output=True, text=True, timeout=10)
        m = re.search(r"esptool(?:\.py)?\s+v?(\d+)\.", ver.stdout + ver.stderr, re.IGNORECASE)
        if m:
            is_v5 = int(m.group(1)) >= 5
    except Exception:
        pass
    return prefix, is_v5


def merge_firmware(source, target, env):
    build_dir   = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    env_name    = env["PIOENV"].replace("-", "_")
    piohome = os.path.join(os.path.expanduser("~"), ".platformio")

    app_bin    = os.path.join(build_dir, "firmware.bin")
    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    arduino_dir = None
    try:
        arduino_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    except Exception:
        arduino_dir = None
    if not arduino_dir:
        arduino_dir = os.path.join(piohome, "packages", "framework-arduinoespressif32")
    boot_app0  = os.path.join(arduino_dir, "tools", "partitions", "boot_app0.bin")

    # Read version from fsd_version.h
    version = "unknown"
    version_h = os.path.join(project_dir, "include", "fsd_version.h")
    if os.path.exists(version_h):
        with open(version_h) as f:
            for line in f:
                if "FIRMWARE_VERSION" in line:
                    m = re.search(r'"([^"]+)"', line)
                    if m:
                        version = m.group(1)
                    break

    # Debug builds go into the debug/ subfolder; release builds stay in root.
    is_debug = "debug" in env_name
    out_dir  = os.path.join(project_dir, "debug") if is_debug else project_dir
    os.makedirs(out_dir, exist_ok=True)

    merged_out = os.path.join(out_dir, f"{env_name}_v{version}.bin")
    ota_out    = os.path.join(out_dir, f"{env_name}_v{version}_ota.bin")

    # OTA binary = plain app image (no bootloader / partitions)
    shutil.copy(app_bin, ota_out)
    print(f"[merge] OTA binary   : {os.path.basename(ota_out)}")

    # Chip-specific flash parameters
    mcu = env.subst("$BOARD_MCU").lower()
    if "esp32s3" in mcu:
        chip, boot_addr, flash_freq, flash_size = "esp32s3", "0x0000", "80m", "8MB"
    else:
        chip, boot_addr, flash_freq, flash_size = "esp32", "0x1000", "40m", "4MB"

    prefix, is_v5 = _resolve_esptool(env, piohome)
    if is_v5:
        cmd = prefix + [
            "--chip", chip, "merge-bin",
            "-o", merged_out,
            "--flash-mode", "dio", "--flash-freq", flash_freq, "--flash-size", flash_size,
            boot_addr, bootloader,
            "0x8000",  partitions,
            "0xE000",  boot_app0,
            "0x10000", app_bin,
        ]
    else:
        cmd = prefix + [
            "--chip", chip, "merge_bin",
            "-o", merged_out,
            "--flash_mode", "dio", "--flash_freq", flash_freq, "--flash_size", flash_size,
            boot_addr, bootloader,
            "0x8000",  partitions,
            "0xE000",  boot_app0,
            "0x10000", app_bin,
        ]

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        raise Exception("esptool merge_bin failed — see output above")
    print(f"[merge] Merged binary : {os.path.basename(merged_out)}  (flash at 0x0)")


env.AddPostAction("$BUILD_DIR/firmware.bin", merge_firmware)
