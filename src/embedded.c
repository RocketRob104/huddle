/* Compiles data/season_YYYY.json straight into the executable. The snapshot is
 * reachable through embedded_season_start/end. SEASON_FILE is an absolute path
 * supplied by the Makefile. */
#include "embedded.h"

#ifndef SEASON_FILE
#error "build with -DSEASON_FILE=\"/abs/path/season.json\" (the Makefile does this)"
#endif

#if defined(__APPLE__)
__asm__(".section __DATA,__const\n"
        ".globl _embedded_season_start\n"
        ".balign 16\n"
        "_embedded_season_start:\n"
        ".incbin \"" SEASON_FILE "\"\n"
        ".globl _embedded_season_end\n"
        "_embedded_season_end:\n"
        ".byte 0\n"
        ".text\n");
#else
__asm__(".section .rodata\n"
        ".globl embedded_season_start\n"
        ".balign 16\n"
        "embedded_season_start:\n"
        ".incbin \"" SEASON_FILE "\"\n"
        ".globl embedded_season_end\n"
        "embedded_season_end:\n"
        ".byte 0\n"
        ".text\n");
#endif
