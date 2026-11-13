// SPDX-License-Identifier: MIT
// PGO training support (host/build_snap.py --pgo-gen). Only built into instrumented exes (SNAP_PGO_GEN); empty otherwise.
// A run killed by PID never reaches the profile runtime's exit-time write, so this thread writes the counters every
// SNAP_PGO_DUMP_S seconds (default 15) to LLVM_PROFILE_FILE (default "default.profraw" in the working directory). On Windows
// each write APPENDS a profile to the file (found 2026-11-13: llvm-profdata merge summed about 20 cumulative copies, so boot
// counted 20 times and the end of the run once), so the counters are reset after each write: the appended pieces are then
// disjoint time slices and their merge is the true total. See host/README.md "PGO and ThinLTO".
#ifdef SNAP_PGO_GEN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_reset_counters(void);

static DWORD WINAPI pgo_dump_thread(void *) {
    const char *e = getenv("SNAP_PGO_DUMP_S");
    int s = e ? atoi(e) : 15;
    if (s < 1) s = 15;
    for (;;) {
        Sleep((DWORD)s * 1000);
        int rc = __llvm_profile_write_file();
        if (rc) { fprintf(stderr, "pgo: profile write failed (%d)\n", rc); fflush(stderr); }
        else __llvm_profile_reset_counters();
    }
    return 0;
}

static int g_pgo_started = [] {
    HANDLE h = CreateThread(nullptr, 0, pgo_dump_thread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    return 1;
}();
#endif
