/* SPDX-License-Identifier: LGPL-2.1-or-later
 * DLL redirection ("DotLocal", patch 1040). A folder beside a program named
 * after it plus ".local" (game.exe.local) is searched first for its DLLs.
 * Some launchers built with MinGW Qt ship every DLL there and none beside the
 * program, so Wine without the patch stops at their imports (c0000135).
 *
 * Built in several ways by tests/test_wine_dotlocal.py:
 *  -DPROBE_DLL=N  a DLL whose probe_which() returns N: probe.dll is copy 1
 *                 beside the programs and copy 2 in each .local folder;
 *                 built for x86-64 as well, for an image of the wrong
 *                 architecture;
 *  -DCHAIN        chain.dll, only in the .local folder, imported by
 *  -DONLY_LOCAL   only_local.dll, only in the .local folder too;
 *  -DIMPORTS      dotlocal.exe: imports only_local.dll, loads probe.dll by
 *                 name and prints which copy it got (2 expected), then
 *                 loads wrongfirst.dll (x86-64 in .local, copy 1 beside the
 *                 program) and wrongonly.dll (x86-64, only in .local);
 *  (neither)      manifested.exe, built with its own manifest: it ignores its
 *                 .local folder as Windows does, so it gets copy 1. */
#include <windows.h>
#include <stdio.h>

#if defined(PROBE_DLL)
__declspec(dllexport) int probe_which(void) { return PROBE_DLL; }
#elif defined(CHAIN)
__declspec(dllexport) int chain_value(void) { return 7; }
#elif defined(ONLY_LOCAL)
__declspec(dllimport) int chain_value(void);
__declspec(dllexport) int only_local(void) { return chain_value(); }
#else
#ifdef IMPORTS
__declspec(dllimport) int only_local(void);
#endif

static int which_copy(const char *name)
{
    HMODULE module = LoadLibraryA(name);
    int (*which)(void);

    if (!module)
    {
        printf("%s error=%lu\n", name, GetLastError());
        return -1;
    }
    which = (void *)GetProcAddress(module, "probe_which");
    printf("%s which=%d\n", name, which ? which() : 0);
    return 0;
}

int main(void)
{
#ifdef IMPORTS
    printf("import only_local=%d\n", only_local());
#endif
    if (which_copy("probe.dll")) return 2;
#ifdef IMPORTS
    which_copy("wrongfirst.dll");
    which_copy("wrongonly.dll");
#endif
    return 0;
}
#endif
