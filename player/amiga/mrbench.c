/*
 * mrbench - run `mrplay --benchmark` over every video file in a drawer,
 * once per display/C2P mode, and print one result line per run (also
 * written as CSV).
 *
 *   mrbench <drawer> [--modes=c2p|all|name,name,...] [--audio]
 *           [--player=PROGDIR:mrplay] [--out=RAM:mrbench.csv]
 *           [--extra="<more mrplay flags>"]
 *
 * mrplay itself is unchanged: each run is a separate mrplay process, and
 * mrbench just waits for it (SystemTagList blocks), so it takes no CPU
 * while a clip plays. Output goes to T:mrbench.log and is parsed after the
 * run (amiga/mr_bench_parse.h). Ctrl-C stops between runs; ESC stops the
 * clip that is playing (reported as stopped=user).
 *
 * mrplay plays in real time, so "fps" is capped at the clip's own rate. The
 * per-frame times (decode, conversion, display) and "max" (the frame rate
 * those times would allow without pacing) are what tell modes apart.
 * Audio is off unless --audio is given, so the numbers measure video only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <utility/tagitem.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include "mr_bench_parse.h"
#include "mintvid_version.h"

MINTVID_DECLARE_VERSION(mrbench_version, "mrbench");
static const char mrbench_stack[] __attribute__((used)) = "$STACK:16384";

#define MRPLAY_STACK_SIZE 320000UL
#define MAX_FILES         256
#define PATH_CAP          512
#define CMD_CAP           1400

typedef struct bench_mode {
    const char *name;
    const char *flags;
    int in_c2p_set;            /* part of the default --modes=c2p set */
} bench_mode;

/* Every mode uses the same flags the GUIs emit (core/mr_play_options.c). */
static const bench_mode g_modes[] = {
    { "standard", "--aga --wpa",         1 },  /* WritePixelArray8 */
    { "portable", "--aga --c2p",         1 },
    { "riva",     "--aga --riva-c2p",    1 },
    { "kalms",    "--aga --kalms-c2p",   1 },
    { "direct",   "--aga --direct-c2p",  1 },
    { "shadow",   "--aga --shadow",      1 },
    { "fused32",  "--aga --fused32",     1 },
    { "ecs32",    "--aga --ecs32",       1 },  /* same 32 colours as fused32 */
    { "akiko",    "--aga --cd32",        0 },
    { "ham8",     "--aga --ham",         0 },
    { "ham6",     "--aga --ham6",        0 },
    { "ehb",      "--aga --ehb",         0 },
    { "window",   "--aga-window",        0 },
    { "cgx",      "",                    0 },  /* RTG WritePixelArray */
    { "p96",      "--p96",               0 },
    { "rtg-half", "--rtg-half",          0 },
};
#define MODE_COUNT ((int)(sizeof(g_modes) / sizeof(g_modes[0])))

static const char *const g_exts[] = {
    ".avi", ".mp4", ".m4v", ".mov", ".mkv", ".mpg", ".mpeg", ".m1v",
    ".m2v", ".vob", ".ts", ".m2ts", ".mts", ".dv", ".flv", ".3gp", 0
};

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int ends_with_ci(const char *s, const char *ext)
{
    size_t ls = strlen(s), le = strlen(ext), i;
    if (ls < le) return 0;
    for (i = 0; i < le; i++)
        if (lower((unsigned char)s[ls - le + i]) != ext[i]) return 0;
    return 1;
}

static int is_video(const char *name)
{
    int i;
    for (i = 0; g_exts[i]; i++)
        if (ends_with_ci(name, g_exts[i])) return 1;
    return 0;
}

static void join_path(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    if (n && dir[n - 1] != ':' && dir[n - 1] != '/')
        snprintf(out, cap, "%s/%s", dir, name);
    else
        snprintf(out, cap, "%s%s", dir, name);
}

/* Collect the drawer's video files (not recursive). */
static int scan_drawer(const char *dir, char **files, int max)
{
    BPTR lock = Lock((STRPTR)dir, ACCESS_READ);
    struct FileInfoBlock *fib;
    int count = 0;

    if (!lock) return -1;
    fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) { UnLock(lock); return -1; }
    if (Examine(lock, fib) && fib->fib_DirEntryType > 0) {
        while (count < max && ExNext(lock, fib)) {
            char path[PATH_CAP];
            if (fib->fib_DirEntryType >= 0) continue;          /* a drawer */
            if (!is_video((const char *)fib->fib_FileName)) continue;
            join_path(path, sizeof(path), dir, (const char *)fib->fib_FileName);
            files[count] = (char *)malloc(strlen(path) + 1);
            if (!files[count]) break;
            strcpy(files[count], path);
            count++;
        }
    } else {
        count = -1;
    }
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return count;
}

static int mode_selected(const bench_mode *m, const char *spec)
{
    size_t n = strlen(m->name);
    const char *p = spec;
    if (!strcmp(spec, "all")) return 1;
    if (!strcmp(spec, "c2p")) return m->in_c2p_set;
    while ((p = strstr(p, m->name)) != NULL) {
        int start = p == spec || p[-1] == ',';
        int end = p[n] == 0 || p[n] == ',';
        if (start && end) return 1;
        p += n;
    }
    return 0;
}

static const char *base_name(const char *path)
{
    const char *b = path, *p;
    for (p = path; *p; p++)
        if (*p == '/' || *p == ':') b = p + 1;
    return b;
}

/* Where each run's output goes: T: if it can be written, else RAM:. */
static const char *g_log_file = "T:mrbench.log";

static int pick_log_file(void)
{
    static const char *const cands[] = { "T:mrbench.log", "RAM:mrbench.log" };
    int i;
    for (i = 0; i < 2; i++) {
        BPTR fh = Open((STRPTR)cands[i], MODE_NEWFILE);
        if (fh) { Close(fh); g_log_file = cands[i]; return 1; }
    }
    return 0;
}

#define RUN_NO_LOG   (-2L)   /* could not open the log file */
#define RUN_NO_START (-1L)   /* SystemTagList could not run the command */

/* Run one mrplay. Returns its return code, or RUN_NO_LOG/RUN_NO_START
 * with the DOS error in *err. */
static long run_one(const char *player, const char *file, const char *flags,
                    int audio, const char *extra, long *err, int show_cmd)
{
    char cmd[CMD_CAP];
    struct TagItem tags[4];
    BPTR out, in;
    long rc;

    *err = 0;
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" %s --benchmark%s%s%s",
             player, file, flags, audio ? "" : " --no-audio",
             extra[0] ? " " : "", extra);
    if (show_cmd) printf("mrbench: first command: %s\n", cmd);
    out = Open((STRPTR)g_log_file, MODE_NEWFILE);
    if (!out) { *err = IoErr(); return RUN_NO_LOG; }
    in = Open((STRPTR)"NIL:", MODE_OLDFILE);
    tags[0].ti_Tag = SYS_Output;   tags[0].ti_Data = (ULONG)out;
    tags[1].ti_Tag = SYS_Input;    tags[1].ti_Data = (ULONG)in;
    tags[2].ti_Tag = NP_StackSize; tags[2].ti_Data = MRPLAY_STACK_SIZE;
    tags[3].ti_Tag = TAG_DONE;     tags[3].ti_Data = 0;
    rc = SystemTagList((STRPTR)cmd, tags);
    /* SystemTagList closes the handles only when the command started. */
    if (rc == -1) {
        *err = IoErr();
        Close(out);
        if (in) Close(in);
        return RUN_NO_START;
    }
    return rc;
}

/* Parse the log; keep its last non-empty line as the reason for a run
 * that printed no summary. */
static void read_result(mr_bench_result *r, char *last, size_t cap)
{
    char line[512];
    FILE *f = fopen(g_log_file, "r");
    mr_bench_reset(r);
    if (cap) last[0] = 0;
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        mr_bench_parse_line(r, line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
            line[--n] = 0;
        if (n && cap) {
            char *q;
            snprintf(last, cap, "%s", line);
            for (q = last; *q; q++) if (*q == '"') *q = '\'';   /* CSV-safe */
        }
    }
    fclose(f);
}

static void print_header(FILE *csv)
{
    printf("%-22s %-9s %-16s %6s %6s %6s %6s %6s %6s %5s\n",
           "file", "mode", "c2p", "fps", "max", "dec", "conv", "disp",
           "shown", "drop");
    if (csv)
        fprintf(csv, "file,mode,backend,c2p,depth,status,elapsed_ms,decoded,"
                     "presented,dropped,fps,max_fps,decode_us,conversion_us,"
                     "display_us,core_us\n");
}

static void print_row(FILE *csv, const char *file, const char *mode,
                      const mr_bench_result *r, const char *status)
{
    unsigned long fps = mr_bench_fps10(r), max = mr_bench_max_fps10(r);
    const char *c2p = r->c2p[0] ? r->c2p : r->backend[0] ? r->backend : "-";
    char name[23];

    snprintf(name, sizeof(name), "%s", base_name(file));
    if (!r->have_summary) {
        printf("%-22s %-9s %s\n", name, mode, status);
    } else {
        printf("%-22s %-9s %-16.16s %4lu.%lu %4lu.%lu %6lu %6lu %6lu %6lu %5lu\n",
               name, mode, c2p, fps / 10, fps % 10, max / 10, max % 10,
               r->decode_us, r->conv_us, r->display_us, r->presented,
               r->dropped);
    }
    if (csv)
        fprintf(csv, "\"%s\",%s,\"%s\",%s,%s,\"%s\",%lu,%lu,%lu,%lu,%lu.%lu,"
                     "%lu.%lu,%lu,%lu,%lu,%lu\n",
                base_name(file), mode, r->backend, r->c2p[0] ? r->c2p : "",
                r->depth, status, r->elapsed_ms, r->decoded, r->presented,
                r->dropped, fps / 10, fps % 10, max / 10, max % 10,
                r->decode_us, r->conv_us, r->display_us, r->core_us);
}

static void usage(void)
{
    int i;
    printf("usage: mrbench <drawer> [--modes=c2p|all|name,...] [--audio]\n"
           "               [--player=PROGDIR:mrplay] [--out=RAM:mrbench.csv]\n"
           "               [--extra=\"<mrplay flags>\"]\n"
           "modes:");
    for (i = 0; i < MODE_COUNT; i++)
        printf(" %s%s", g_modes[i].name, g_modes[i].in_c2p_set ? "*" : "");
    printf("\n(* = in the default c2p set)\n"
           "fps = frames shown per second (capped at the clip's rate);\n"
           "max = frame rate the per-frame times allow; dec/conv/disp in us.\n");
}

int main(int argc, char **argv)
{
    const char *dir = NULL, *modes = "c2p", *player = "PROGDIR:mrplay";
    const char *out_path = "RAM:mrbench.csv", *extra = "";
    char *files[MAX_FILES];
    static char player_path[PATH_CAP];
    int audio = 0, nfiles, i, m, runs = 0, stop = 0, no_start = 0;
    FILE *csv;
    BPTR plock;

    for (i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--modes=", 8)) modes = argv[i] + 8;
        else if (!strcmp(argv[i], "--audio")) audio = 1;
        else if (!strncmp(argv[i], "--player=", 9)) player = argv[i] + 9;
        else if (!strncmp(argv[i], "--out=", 6)) out_path = argv[i] + 6;
        else if (!strncmp(argv[i], "--extra=", 8)) extra = argv[i] + 8;
        else if (argv[i][0] == '-' || dir) { usage(); return 5; }
        else dir = argv[i];
    }
    if (!dir) { usage(); return 5; }

    for (m = 0; m < MODE_COUNT; m++)
        if (mode_selected(&g_modes[m], modes)) break;
    if (m == MODE_COUNT) {
        printf("mrbench: no known mode in \"%s\"\n", modes);
        usage();
        return 5;
    }

    /* Resolve the player here, where PROGDIR: means mrbench's drawer; the
     * shell SystemTagList starts has a PROGDIR: of its own. */
    plock = Lock((STRPTR)player, ACCESS_READ);
    if (!plock) {
        printf("mrbench: cannot find the player %s (DOS error %ld).\n"
               "         Put mrbench beside mrplay, or use --player=<path>.\n",
               player, IoErr());
        return 10;
    }
    if (NameFromLock(plock, (STRPTR)player_path, sizeof(player_path)))
        player = player_path;
    UnLock(plock);
    if (!pick_log_file()) {
        printf("mrbench: cannot write a log file in T: or RAM: (DOS error %ld)\n",
               IoErr());
        return 10;
    }

    nfiles = scan_drawer(dir, files, MAX_FILES);
    if (nfiles < 0) { printf("mrbench: cannot read drawer %s\n", dir); return 10; }
    if (nfiles == 0) { printf("mrbench: no video files in %s\n", dir); return 5; }

    csv = fopen(out_path, "w");
    if (!csv) printf("mrbench: cannot write %s, results on screen only\n", out_path);
    printf("mrbench: %d file(s), audio %s, player %s, log %s\n", nfiles,
           audio ? "on" : "off", player, g_log_file);
    print_header(csv);

    for (i = 0; i < nfiles && !stop; i++) {
        for (m = 0; m < MODE_COUNT && !stop; m++) {
            mr_bench_result r;
            long rc, err;
            char last[128], status[160];

            if (!mode_selected(&g_modes[m], modes)) continue;
            if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) { stop = 1; break; }
            rc = run_one(player, files[i], g_modes[m].flags, audio, extra,
                         &err, runs == 0);
            read_result(&r, last, sizeof(last));
            if (rc == RUN_NO_LOG)
                snprintf(status, sizeof(status), "no-log (DOS error %ld)", err);
            else if (rc == RUN_NO_START)
                snprintf(status, sizeof(status), "not-started (DOS error %ld)", err);
            else if (!r.have_summary)
                snprintf(status, sizeof(status), "failed rc=%ld: %s", rc,
                         last[0] ? last : "(no output)");
            else
                snprintf(status, sizeof(status), "%s", r.stopped[0] ? r.stopped : "eof");
            no_start = rc < 0 ? no_start + 1 : 0;
            print_row(csv, files[i], g_modes[m].name, &r, status);
            if (csv) fflush(csv);
            runs++;
            if (no_start >= 3) {
                printf("mrbench: mrplay could not be started 3 times in a row, "
                       "giving up\n");
                stop = 2;
            }
        }
    }
    if (stop == 1) printf("mrbench: stopped (Ctrl-C)\n");
    printf("mrbench: %d run(s)%s%s\n", runs, csv ? ", CSV in " : "",
           csv ? out_path : "");
    if (csv) fclose(csv);
    DeleteFile((STRPTR)g_log_file);
    for (i = 0; i < nfiles; i++) free(files[i]);
    return 0;
}
