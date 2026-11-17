#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""A Direct3D 12 resource on a shared heap, and its NT handle, on a device
without VK_KHR_external_memory_win32 (wine/patches/1027).
tests/d3d12_shared_resource.c runs under the host Wine with DXVK's dxgi and
vkd3d-proton's d3d12 and d3d12core, extracted by pw_install's own code from
the pinned releases, as tests/test_d3d12_control.py sets them up.

A PS5's Vulkan driver has no external memory fds, so winevulkan's devices
there have no VK_KHR_external_memory_win32. vkd3d-proton still asks for
the Win32 handle of a shared resource's memory; without 1027 the function
it gets is NULL and the program dies at address 0, as Unreal Engine 5 games
did while they started. VKD3D_DISABLE_EXTENSIONS leaves the extension out
of vkd3d-proton's device on the host the same way.

It needs a Vulkan GPU (lavapipe will do), the host Wine from
tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine), x86_64-w64-mingw32-gcc and the two
releases, so it runs only with PROSPERO_TEST_D3D12=1 and is skipped
otherwise."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "d3d12_shared_resource.c"
sys.path.insert(0, str(ROOT / "tools"))
import pw_install  # noqa: E402

DXVK_VERSION = "2.6.2"
VKD3D_VERSION = "3.0.1"


def build(compiler: str, exe: Path) -> None:
    subprocess.run([compiler, "-O1", "-Wall", "-Wextra", "-Werror", "-o", str(exe), str(SOURCE), "-ld3d12"],
                   check=True)


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    compiler = shutil.which("x86_64-w64-mingw32-gcc")
    if os.environ.get("PROSPERO_TEST_D3D12") != "1":
        if compiler:   # the program still has to build
            with tempfile.TemporaryDirectory() as directory:
                build(compiler, Path(directory) / "d3d12_shared_resource.exe")
            print("d3d12 shared resource built; run skipped (PROSPERO_TEST_D3D12=1 runs it on a Vulkan GPU)")
        else:
            print("d3d12 shared resource skipped: needs x86_64-w64-mingw32-gcc")
        return 0
    assert os.access(wine, os.X_OK) and compiler, "needs the host Wine and x86_64-w64-mingw32-gcc"

    with tempfile.TemporaryDirectory() as directory:
        exe = Path(directory) / "d3d12_shared_resource.exe"
        build(compiler, exe)
        env = dict(os.environ, WINEPREFIX=str(Path(directory) / "pfx"), WINEDEBUG="-all",
                   WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d")
        subprocess.run([wine, "wineboot", "-i"], env=env, check=True, capture_output=True)
        windows = Path(env["WINEPREFIX"]) / "drive_c" / "windows"
        url, digest = pw_install.dxvk_release(DXVK_VERSION)
        pw_install.extract_dxvk(pw_install.fetch(url, digest, f"dxvk-{DXVK_VERSION}.tar.gz"), DXVK_VERSION, windows)
        url, digest = pw_install.vkd3d_proton_release(VKD3D_VERSION)
        pw_install.extract_vkd3d_proton(pw_install.fetch(url, digest, url.rsplit("/", 1)[-1]), VKD3D_VERSION,
                                        windows)
        env["WINEDLLOVERRIDES"] += ";dxgi,d3d12,d3d12core=n"
        env["VKD3D_DISABLE_EXTENSIONS"] = "VK_KHR_external_memory_win32"
        # vkd3d-proton keeps its pipeline cache in the current directory.
        done = subprocess.run([wine, str(exe)], capture_output=True, text=True, env=env, timeout=300,
                              cwd=directory)
        subprocess.run([str(Path(wine).with_name("wineserver")), "-k"], env=env, capture_output=True)
        lines = [text for text in done.stdout.splitlines() if text.startswith("D3D12_SHARED_RESOURCE ")]
        assert lines, f"no result line:\n{done.stdout}{done.stderr[-4000:]}"
        line = lines[-1]
        assert "verdict=" in line, f"stopped at {line} (exit {done.returncode:#x}):\n{done.stderr[-4000:]}"
        assert done.returncode == 0 and line.endswith("verdict=pass"), line
    print(f"d3d12 shared resource passed without VK_KHR_external_memory_win32: {line.split(' ', 1)[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
