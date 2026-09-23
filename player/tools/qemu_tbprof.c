/*
 * qemu_tbprof - exact guest-instruction profile of an m68k (or any
 * qemu-user) program, from qemu's own translation-block log.
 *
 *   tools/qemu_tbprof.sh ./binary.m68k args...   (see that script)
 *
 * qemu-user prints each translated block once under "-d in_asm" (its
 * instructions) and every execution of a block under "-d exec,nochain"
 * (its start PC). Instructions per function = sum over blocks of
 * executions x block length. Unlike callgrind over qemu, which counts the
 * host instructions of qemu's JIT, this counts m68k instructions: it does
 * not depend on where code lands relative to 4 KB pages, and names the
 * guest functions directly. Blocks with no ELF symbol size (hand-written
 * .S kernels) are named from an "nm -n" listing instead: argv[3].
 *
 * Also counts callers of memcpy/memset (the block executed just before
 * each entry).
 *
 * usage: qemu_tbprof LOG [TOP-BLOCKS] [NM-LISTING]   (LOG may be a fifo)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define HB (1u << 20)
typedef struct { uint64_t pc; unsigned n; uint64_t execs; char fn[48]; char *text; } tb;
static tb *tab;

static tb *get(uint64_t pc)
{
    unsigned h = (unsigned)((pc * 0x9E3779B97F4A7C15ull) >> 44) & (HB - 1);
    while (tab[h].pc && tab[h].pc != pc) h = (h + 1) & (HB - 1);
    tab[h].pc = pc;
    return &tab[h];
}

typedef struct { char fn[48]; uint64_t insns; } fnt;
static int cmpf(const void *a, const void *b)
{
    uint64_t x = ((const fnt *)a)->insns, y = ((const fnt *)b)->insns;
    return x < y ? 1 : x > y ? -1 : 0;
}
static int cmpt(const void *a, const void *b)
{
    const tb *x = *(tb * const *)a, *y = *(tb * const *)b;
    uint64_t p = x->execs * x->n, q = y->execs * y->n;
    return p < q ? 1 : p > q ? -1 : 0;
}

static uint64_t *symaddr; static char (*symname)[48]; static int nsym;
static void load_syms(const char *path)
{
    FILE *f = fopen(path, "r"); char l[512]; int cap = 0;
    if (!f) return;
    while (fgets(l, sizeof l, f)) {
        unsigned long long a; char t, n[256];
        if (sscanf(l, "%llx %c %255s", &a, &t, n) != 3) continue;
        if (t != 't' && t != 'T') continue;
        if (nsym == cap) { cap = cap ? cap * 2 : 4096;
            symaddr = realloc(symaddr, cap * sizeof *symaddr);
            symname = realloc(symname, cap * sizeof *symname); }
        symaddr[nsym] = a; snprintf(symname[nsym], 48, "%s", n); nsym++;
    }
}
static const char *sym_for(uint64_t pc)
{
    int lo = 0, hi = nsym - 1, best = -1;
    while (lo <= hi) { int m = (lo + hi) / 2;
        if (symaddr[m] <= pc) { best = m; lo = m + 1; } else hi = m - 1; }
    return best < 0 ? "?" : symname[best];
}

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "r");
    int top = argc > 2 ? atoi(argv[2]) : 20;
    if (argc > 3) load_syms(argv[3]);
    static char line[4096];
    char curfn[48] = "";
    uint64_t curpc = 0; unsigned curn = 0; char *buf = NULL; size_t blen = 0;
    int inblock = 0;
    uint64_t total = 0;
    tab = calloc(HB, sizeof *tab);
    /* caller attribution for memcpy/memset entries */
    uint64_t callee[2] = {0, 0}; const char *cname[2] = {"memcpy", "memset"};
    static char callers[2][64][48]; static uint64_t ccount[2][64]; int ncall[2] = {0, 0};
    const char *prevfn = "?";
    { int k; for (k = 0; k < nsym; k++) { int c; for (c = 0; c < 2; c++)
        if (!strcmp(symname[k], cname[c])) callee[c] = symaddr[k]; } }
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "IN:", 3)) {
            char *p = line + 3; while (*p == ' ') p++;
            p[strcspn(p, "\n")] = 0;
            snprintf(curfn, sizeof curfn, "%s", *p ? p : "?");
            inblock = 1; curpc = 0; curn = 0; free(buf); buf = NULL; blen = 0;
            continue;
        }
        if (inblock && !strncmp(line, "0x", 2)) {
            size_t l = strlen(line);
            if (!curn) curpc = strtoull(line, NULL, 16);
            curn++;
            buf = realloc(buf, blen + l + 1); memcpy(buf + blen, line, l + 1); blen += l;
            continue;
        }
        if (inblock && curn) {
            tb *t = get(curpc);
            t->n = curn;
            snprintf(t->fn, sizeof t->fn, "%s", nsym ? sym_for(curpc) : curfn);
            free(t->text); t->text = buf; buf = NULL; blen = 0;
            inblock = 0; curn = 0;
        }
        if (!strncmp(line, "Trace ", 6)) {
            char *b = strchr(line, '[');
            uint64_t pc;
            if (!b) continue;
            b = strchr(b, '/'); if (!b) continue;
            pc = strtoull(b + 1, NULL, 16);
            tb *t = get(pc);
            int c;
            t->execs++;
            for (c = 0; c < 2; c++) if (pc == callee[c]) {
                int k; for (k = 0; k < ncall[c]; k++) if (!strcmp(callers[c][k], prevfn)) break;
                if (k == ncall[c] && k < 64) { snprintf(callers[c][k], 48, "%s", prevfn); ncall[c]++; }
                if (k < 64) ccount[c][k]++;
            }
            prevfn = t->fn[0] ? t->fn : "?";
        }
    }
    {
        fnt *fs = calloc(HB, sizeof *fs); int nf = 0; unsigned i; int j;
        tb **hot = calloc(HB, sizeof *hot); int nh = 0;
        for (i = 0; i < HB; i++) if (tab[i].pc && tab[i].n) {
            uint64_t c = tab[i].execs * tab[i].n; total += c;
            for (j = 0; j < nf; j++) if (!strcmp(fs[j].fn, tab[i].fn)) break;
            if (j == nf) { snprintf(fs[nf].fn, 48, "%s", tab[i].fn); nf++; }
            fs[j].insns += c;
            hot[nh++] = &tab[i];
        }
        qsort(fs, nf, sizeof *fs, cmpf);
        printf("total guest insns: %llu\n", (unsigned long long)total);
        { int c, k; for (c = 0; c < 2; c++) for (k = 0; k < ncall[c]; k++)
            printf("caller %s <- %s : %llu calls\n", cname[c], callers[c][k], (unsigned long long)ccount[c][k]); }
        for (j = 0; j < nf && j < 40; j++)
            printf("%6.2f%% %12llu %s\n", 100.0 * fs[j].insns / total,
                   (unsigned long long)fs[j].insns, fs[j].fn);
        qsort(hot, nh, sizeof *hot, cmpt);
        for (j = 0; j < nh && j < top; j++)
            printf("\n== block %d: %s pc=%llx n=%u execs=%llu (%.2f%%)\n%s", j,
                   hot[j]->fn, (unsigned long long)hot[j]->pc, hot[j]->n,
                   (unsigned long long)hot[j]->execs,
                   100.0 * hot[j]->execs * hot[j]->n / total,
                   hot[j]->text ? hot[j]->text : "");
    }
    return 0;
}
