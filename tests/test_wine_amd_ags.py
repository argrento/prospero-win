#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The built-in AMD GPU Services library (Wine patch 1042): Proton's amd_ags_x64 without its Unix library, built as one
of the kit's PE modules for x86-64, and kept out of the system directories so only a DLL override puts it in place of
a game's own copy.

The behaviour (agsInitialize answering from Vulkan, a game past its AMD check) is checked on the console."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/1042-amd-ags-x64-add-a-built-in-amd-gpu-services-library.patch"


def diffs():
    return {diff.splitlines()[0].split(" b/", 1)[1]: diff for diff in PATCH.read_text().split("diff --git ")[1:]}


def added(path):
    return "\n".join(line[1:] for line in diffs()[path].splitlines() if line.startswith("+") and not line.startswith("+++"))


def main():
    files = diffs()
    new = {path for path, diff in files.items() if "\nnew file mode " in diff}
    assert new == {f"dlls/amd_ags_x64/{name}" for name in
                   ("Makefile.in", "amd_ags.h", "amd_ags_x64.spec", "amd_ags_x64_main.c", "dxvk_interfaces.idl")}, new
    # Configured like every other module, in both the template and the generated script.
    assert added("configure.ac").strip() == "WINE_CONFIG_MAKEFILE(dlls/amd_ags_x64)"
    assert set(added("configure").split("\n")) == {"enable_amd_ags_x64",
                                                    "wine_fn_config_makefile dlls/amd_ags_x64 enable_amd_ags_x64"}
    # No Unix library: nothing needs libdrm, and the PE side never calls into one.
    makefile = added("dlls/amd_ags_x64/Makefile.in")
    assert "MODULE    = amd_ags_x64.dll" in makefile and "IMPORTS   = version vulkan-1 user32" in makefile
    assert "UNIXLIB" not in makefile and "DRM" not in makefile and "unixlib.c" not in makefile
    main = added("dlls/amd_ags_x64/amd_ags_x64_main.c")
    for gone in ("unixlib.h", "WINE_UNIX_CALL", "__wine_init_unix_call", "#include \"wine/heap.h\"", "heap_alloc(",
                 "heap_free("):
        assert gone not in main, gone
    # Where it came from, and an AMD device still gets a family when nothing reports its units and clocks.
    assert "ValveSoftware/wine, branch proton_10.0" in main and "Paul Gofman" in main
    assert "GNU Lesser General Public" in main
    amd = main.split("if (vk_properties->vendorID == 0x1002)", 1)[1].split("SET_DEVICE_FIELD(device, localMemoryInBytes", 1)[0]
    assert "AsicFamily_GCN4" in amd and "numCUs" not in amd
    # Games read the first device without looking at the count: no GPU must fail, not return an empty context.
    context = main.split("static AGSReturnCode init_ags_context(", 1)[1].split("\n}\n", 1)[0]
    assert "if (!context->device_count)" in context and "return AGS_FAILURE;" in context
    assert "if (ret != AGS_SUCCESS || !context->device_count)" not in context
    # A skipped device (not a GPU) gives its slot to the next one instead of being read again.
    listing = main.split("static AGSReturnCode vk_get_physical_device_properties(", 1)[1].split("\n}\n", 1)[0]
    assert "&properties[kept]" in listing and "*out_count = kept;" in listing and "--count" not in listing
    # No DXGI: the displays come without colour information instead of a call through a null factory.
    chroma = main.split("static void fill_chroma_info(", 1)[1].split("\n}\n", 1)[0]
    assert chroma.index("if (!c->dxgi_factory)") < chroma.index("IDXGIFactory1_EnumAdapters1")
    # The exports games call: agsInit (AGS 5 and older: RDR2) and agsInitialize (6.x: UNCHARTED).
    spec = added("dlls/amd_ags_x64/amd_ags_x64.spec")
    for export in ("agsInit(", "agsInitialize(", "agsDeInit(", "agsDeInitialize(", "agsCheckDriverVersion("):
        assert re.search(r"^@ stdcall " + re.escape(export), spec, re.M), export
    # Not copied into system32/syswow64: a game's own copy in its working folder would lose to it otherwise.
    # From an installed library folder (amd_ags_x64.dll) or a build tree (a folder named amd_ags_x64).
    assert ('if (!wcscmp( data.name, L"amd_ags_x64.dll" ) || !wcscmp( data.name, L"amd_ags_x64" )) continue;'
            in added("dlls/setupapi/fakedll.c"))
    # The kit builds it for x86-64 only, beside setupapi, which carries the skip.
    script = (ROOT / "tools/build_wine_ps5.sh").read_text()
    modules = " ".join(re.findall(r'^PE_MODULES="([^"\n]+)"', script, re.M))
    assert "setupapi" in modules.split() and "amd_ags_x64" not in modules.split()
    assert '[ "$arch" != x86_64 ] || modules="$modules wow64 wow64win amd_ags_x64"' in script
    print("Wine amd_ags_x64 passed: built-in AGS without a Unix library, x86-64 only, not in the system directories")


if __name__ == "__main__":
    main()
