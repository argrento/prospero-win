#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""A program's <name>.local folder is searched first for its DLLs, for its
imports, theirs and LoadLibrary by name, unless the program has its own
manifest; an image there for another architecture is skipped for a copy in
the search path, or fails as a bad format (wine/patches/1040).

Needs the host Wine from tools/build_host_wine.sh (PROSPERO_HOST_WINE, default
.deps/wine-host/install/usr/bin/wine) and the i686 and x86_64 mingw compilers plus windres;
without them the check is skipped with a message, since the host gate runs
without them."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "tests" / "wine_dotlocal.c"
MANIFEST = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="Prospero.DotLocal.Test" version="1.0.0.0" processorArchitecture="x86"/>
</assembly>
"""


def main() -> int:
    wine = os.environ.get("PROSPERO_HOST_WINE", str(ROOT / ".deps/wine-host/install/usr/bin/wine"))
    compiler = shutil.which("i686-w64-mingw32-gcc")
    compiler64 = shutil.which("x86_64-w64-mingw32-gcc")
    windres = shutil.which("i686-w64-mingw32-windres")
    if not os.access(wine, os.X_OK) or not compiler or not compiler64 or not windres:
        print("wine dotlocal skipped: needs the host Wine, the i686 and x86_64 mingw compilers and windres")
        return 0
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        app = root / "app"
        build = root / "build"
        for folder in (app / "dotlocal.exe.local", app / "manifested.exe.local", build):
            folder.mkdir(parents=True)

        def cc(output: Path, *flags: str, tool: str = compiler) -> None:
            subprocess.run([tool, "-O1", *flags, "-o", str(output), str(SOURCE)], check=True)

        cc(app / "probe.dll", "-shared", "-DPROBE_DLL=1")
        cc(app / "dotlocal.exe.local/probe.dll", "-shared", "-DPROBE_DLL=2")
        shutil.copy(app / "dotlocal.exe.local/probe.dll", app / "manifested.exe.local/probe.dll")
        chain = app / "dotlocal.exe.local/chain.dll"
        cc(chain, "-shared", "-DCHAIN")
        only_local = app / "dotlocal.exe.local/only_local.dll"
        cc(only_local, "-shared", "-DONLY_LOCAL", str(chain))
        cc(app / "wrongfirst.dll", "-shared", "-DPROBE_DLL=1")
        cc(app / "dotlocal.exe.local/wrongfirst.dll", "-shared", "-DPROBE_DLL=3", tool=compiler64)
        cc(app / "dotlocal.exe.local/wrongonly.dll", "-shared", "-DPROBE_DLL=3", tool=compiler64)
        cc(app / "dotlocal.exe", "-DIMPORTS", str(only_local))
        (build / "app.manifest").write_text(MANIFEST)
        (build / "app.rc").write_text('1 24 "app.manifest"\n')
        resource = build / "app.o"
        subprocess.run([windres, "-I", str(build), str(build / "app.rc"), "-O", "coff", "-o", str(resource)],
                       check=True, cwd=build)
        cc(app / "manifested.exe", str(resource))

        env = dict(os.environ, WINEPREFIX=str(root / "pfx"), WINEDEBUG="-all",
                   WINEDLLOVERRIDES="winemenubuilder.exe=d")
        # Started from another folder, so only the program's own folder holds copy 1.
        done = subprocess.run([wine, str(app / "dotlocal.exe")], capture_output=True, text=True, env=env, cwd=root)
        assert done.returncode == 0, f"dotlocal.exe: {done.returncode}\n{done.stdout}{done.stderr}"
        assert "import only_local=7" in done.stdout, done.stdout
        assert "probe.dll which=2" in done.stdout, done.stdout
        assert "wrongfirst.dll which=1" in done.stdout, done.stdout
        assert "wrongonly.dll error=193" in done.stdout, done.stdout  # ERROR_BAD_EXE_FORMAT
        done = subprocess.run([wine, str(app / "manifested.exe")], capture_output=True, text=True, env=env, cwd=root)
        assert done.returncode == 0, f"manifested.exe: {done.returncode}\n{done.stdout}{done.stderr}"
        assert "probe.dll which=1" in done.stdout, done.stdout
    print("wine dotlocal passed: a program's .local folder comes first, unless it has a manifest")
    return 0


if __name__ == "__main__":
    sys.exit(main())
