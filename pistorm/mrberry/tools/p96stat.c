/* P96STAT - show the live state of berryvp.library while a film plays or
 * hangs (P96W33).  Run it from a second Shell.  It does NOT open the library
 * (OpenLibrary would run the library's open code); it only finds the already
 * loaded library in the exec list and reads the table at libbase+124.
 * Usage: P96STAT [count]   (count = how many times, 1 s apart)
 * Build: m68k-amigaos-gcc -O2 -noixemul -o P96STAT p96stat.c */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <stdio.h>
#include <stdlib.h>

extern struct ExecBase *SysBase;

static ULONG le32(volatile UBYTE *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

static int show(void)
{
    struct Library *lib;
    ULONG *t, *s;
    Forbid();
    lib = (struct Library *)FindName(&SysBase->LibList, (CONST_STRPTR)"berryvp.library");
    Permit();
    if (!lib) { printf("berryvp.library is not loaded.\n"); return 10; }
    printf("%s", lib->lib_IdString ? (char *)lib->lib_IdString : "berryvp.library\n");
    t = *(ULONG **)((UBYTE *)lib + 124);
    if (!t || t[0] != 0x50393644UL) {
        printf("No P96W33 state table (old library, or no film started yet).\n");
        return 10;
    }
    t++;
    while ((LONG)t[0] != -1) {
        ULONG kind = t[0], v;
        const char *name = (const char *)t[1];
        ULONG *addr = (ULONG *)t[2];
        if (kind == 1) {
            UBYTE *base = (UBYTE *)*addr;
            v = base ? le32(base + t[3]) : 0;
        } else {
            v = *addr;
        }
        printf("%10lu  $%08lx  %s\n", (unsigned long)v, (unsigned long)v, name);
        t += 4;
    }
    s = *(ULONG **)((UBYTE *)lib + 120);
    if (s && s[0] == 0x50393653UL)
        printf("clock: media=%lu sys=%lu | shown pts=%lu sys=%lu | port_mode=%lu\n",
               (unsigned long)s[3], (unsigned long)s[4], (unsigned long)s[7],
               (unsigned long)s[8], (unsigned long)s[15]);
    return 0;
}

int main(int argc, char **argv)
{
    int n = (argc > 1) ? atoi(argv[1]) : 1, i, r = 0;
    if (n < 1) n = 1;
    for (i = 0; i < n; i++) {
        if (i) { printf("----\n"); fflush(stdout); }
        r = show();
        if (r) break;
        if (i + 1 < n) {
            Delay(50);                          /* 1 second */
        }
    }
    return r;
}
