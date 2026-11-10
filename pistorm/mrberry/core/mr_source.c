/*
 * MintVID - local-file and generic source ownership.
 */
#include "mr_source.h"
#include "mr_hls.h"
#include "mr_http.h"
#include "mr_youtube.h"
#include "mr_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef MR_DEBUG_LOGS
#define MR_DEBUG_LOGS 0
#endif

#if MR_DEBUG_LOGS
#define DBG_PRINTF(...) printf(__VA_ARGS__)
#else
#define DBG_PRINTF(...) ((void)0)
#endif

#if defined(AMIGA_M68K) || defined(__amigaos__) || defined(__AMIGA__)
#include <exec/types.h>
#include <dos/dos.h>
#include <proto/dos.h>
#endif

#define MR_SOURCE_NAME_MAX 1024
#define MR_SOURCE_ERROR_MAX 192
/* libnix defaults FILE streams to a 1 KiB buffer. Local interleaved media then
 * crosses that boundary constantly, turning sequential AVI playback into many
 * small AmigaDOS reads. Keep libc's normal fully-buffered path, but enlarge its
 * read window enough to cover several low-resolution video/audio packets. */
#define MR_LOCAL_FILE_BUFFER_SIZE (64u * 1024u)
#define MR_LOCAL_FILE_READ_CHUNK   (64u * 1024u)
#define MR_LOCAL_FILE_READAHEAD    (128u * 1024u)

/* mrberry --read=KB: size of one read from the file, and the read-ahead
 * window is two of them. The default stays at the 64 KiB / 128 KiB that was
 * tuned on local USB/SD storage, where a bigger read blocks the player for
 * too long. Over a network share every read waits a round trip, so fewer,
 * bigger reads are faster there. Set before the file is opened. */
static size_t g_read_chunk = MR_LOCAL_FILE_READ_CHUNK;
static size_t g_readahead  = MR_LOCAL_FILE_READAHEAD;

void mrb_source_set_read_kb(unsigned kb)
{
    if (kb < 64u) kb = 64u;
    if (kb > 1024u) kb = 1024u;
    g_read_chunk = (size_t)kb * 1024u;
    g_readahead = 2u * g_read_chunk;
}

struct mr_source {
    void   *ctx;
    size_t  len;
    int   (*read_at)(void *, size_t, void *, size_t);
    void  (*close)(void *);
    char    final_name[MR_SOURCE_NAME_MAX];
    int     network;
    size_t  buffer_capacity;
    int     memory_cached;
};

typedef struct {
    FILE   *file;          /* normal <=2GiB stdio path; NULL in N96Q large mode */
#if defined(AMIGA_M68K) || defined(__amigaos__) || defined(__AMIGA__)
    BPTR    dos_fh;        /* N96Q direct AmigaDOS handle for >2GiB local files */
#endif
    char    dos_path[MR_SOURCE_NAME_MAX];
    int     large_file_mode;
    size_t  pos;
    int     pos_valid;
    unsigned char *buffer;
    size_t  buffer_size;
    int     cached;
    size_t  cached_len;
    /* N68: explicit 128-KiB local-file read-ahead.  It is filled only by
     * <=64-KiB fread() calls so Poseidon/USB never sees one 128-KiB request. */
    unsigned char *readahead;
    size_t  readahead_start;
    size_t  readahead_len;
    size_t  file_len;
    /* N96Q large-file diagnostics.  These are only written after playback. */
    unsigned long lf_reopens, lf_seek_low, lf_seek_high, lf_direct_reads, lf_direct_bytes;
    int     lf_cross_announced, lf_high_seek_announced;
    /* N68B I/O probe: playback-neutral counters, written only at close. */
    unsigned long ra_hits, ra_misses, ra_fills, ra_chunk_reads;
    unsigned long ra_stitch_hits, ra_stitch_bytes;
    unsigned long ra_chunk_max_ms, ra_fill_max_ms;
    unsigned long ra_chunk_gt10ms, ra_chunk_gt20ms, ra_chunk_gt50ms;
    clock_t diag_started;
    int diag_snapshot_written;
} file_source;

static char g_source_error[MR_SOURCE_ERROR_MAX];
static mr_source_timing g_timing;
static int g_timing_enabled = 0;

void mr_source_set_timing_enabled(int enabled) { g_timing_enabled = enabled != 0; }
int  mr_source_timing_enabled(void) { return g_timing_enabled; }

static void file_maybe_write_diag_snapshot(file_source *f);

static unsigned long elapsed_ms(clock_t start)
{
    /* N97Q: clock_t is signed on the classic target.  A long playback can
     * cross its wrap point; signed subtraction then produced bogus multi-
     * million-ms samples in N68B_IO.TXT.  Do the subtraction modulo ULONG
     * instead.  Individual I/O measurements are short, so the modulo delta
     * is the real elapsed tick count across one wrap. */
    unsigned long now = (unsigned long)clock();
    unsigned long then = (unsigned long)start;
    unsigned long elapsed = now - then;
    return (unsigned long)((elapsed * 1000UL) / (unsigned long)CLOCKS_PER_SEC);
}

void mr_source_timing_get(mr_source_timing *timing)
{
    if (timing) *timing = g_timing;
}
void mr_source_timing_reset(void) { memset(&g_timing, 0, sizeof g_timing); }
void mr_source_timing_add_network(unsigned long ms) { g_timing.network_ms += ms; }
void mr_source_mark_network(mr_source *s) { if (s) s->network = 1; }
void mr_source_timing_add_hls_segment(unsigned long ms) { g_timing.hls_segment_ms += ms; }
void mr_source_timing_add_hls_playlist(unsigned long ms) { g_timing.hls_playlist_ms += ms; }


void mr_source_set_error(const char *message)
{
    size_t n;
    if (!message) message = "source open failed";
    n = strlen(message);
    if (n >= sizeof g_source_error) n = sizeof g_source_error - 1;
    memcpy(g_source_error, message, n);
    g_source_error[n] = '\0';
}

const char *mr_source_last_error(void)
{
    return g_source_error[0] ? g_source_error : "source open failed";
}

static int starts_nocase(const char *s, const char *prefix)
{
    while (*prefix) {
        int a = (unsigned char)*s++;
        int b = (unsigned char)*prefix++;
        if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
        if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
        if (a != b) return 0;
    }
    return 1;
}

int mr_source_is_url(const char *path)
{
    return path && (starts_nocase(path, "http://") ||
                    starts_nocase(path, "https://"));
}

mr_source *mr_source_create(void *ctx, size_t len,
                            int (*read_at)(void *, size_t, void *, size_t),
                            void (*close)(void *),
                            const char *final_name)
{
    mr_source *s;
    size_t n;
    /* len == MR_SOURCE_LEN_UNKNOWN marks a forward-only stream; only a truly
     * zero-length source is rejected. */
    if (!ctx || !read_at || !close || !len) return NULL;
    s = (mr_source *)mr_allocz(sizeof *s);
    if (!s) {
        close(ctx);
        mr_source_set_error("not enough memory for media source");
        return NULL;
    }
    s->ctx = ctx;
    s->len = len;
    s->read_at = read_at;
    s->close = close;
    if (!final_name) final_name = "";
    n = strlen(final_name);
    if (n >= sizeof s->final_name) n = sizeof s->final_name - 1;
    memcpy(s->final_name, final_name, n);
    s->final_name[n] = '\0';
    return s;
}

#if defined(AMIGA_M68K) || defined(__amigaos__) || defined(__AMIGA__)
static void n96q_note_advance(file_source *f, size_t old_pos, size_t new_pos)
{
    if (!f || f->lf_cross_announced) return;
    if (old_pos < 0x80000000UL && new_pos >= 0x80000000UL) {
        DBG_PRINTF("[N96Q] crossed 2GiB source offset using direct AmigaDOS Read\n");
        f->lf_cross_announced = 1;
    }
}

/* Reopen before an arbitrary reposition.  This deliberately avoids asking
 * classic Seek() to report an old position that is already above 2 GiB. */
static int n96q_dos_reopen(file_source *f)
{
    BPTR fh;
    if (!f || !f->dos_path[0]) return 0;
    if (f->dos_fh) Close(f->dos_fh);
    fh = Open((CONST_STRPTR)f->dos_path, MODE_OLDFILE);
    if (!fh) {
        f->dos_fh = 0;
        f->pos_valid = 0;
        return 0;
    }
    f->dos_fh = fh;
    f->pos = 0;
    f->pos_valid = 1;
    f->lf_reopens++;
    return 1;
}

/* Position a classic AmigaDOS handle anywhere in the unsigned 32-bit file
 * range without ever passing a negative LONG offset.
 *
 * For positions above 0x7fffffff the handle is first reopened at byte 0,
 * then moved in two positive signed-LONG steps:
 *   first = target - 0x7fffffff
 *   second = 0x7fffffff relative to current
 * Both Seek() return values therefore describe an old position <= LONG_MAX,
 * so the documented -1 error return remains unambiguous.
 *
 * N90Q already proved that the same AmigaDOS file handle can continue reading
 * after its physical position crosses 0x80000000. */
static int n96q_dos_seek_exact(file_source *f, size_t off)
{
    LONG r;
    if (!f || !f->large_file_mode || !f->dos_fh) return 0;
    if (f->pos_valid && f->pos == off) return 1;

    if (!n96q_dos_reopen(f)) return 0;

    if (off <= 0x7fffffffUL) {
        r = Seek(f->dos_fh, (LONG)off, OFFSET_BEGINNING);
        if (r == -1) {
            f->pos_valid = 0;
            return 0;
        }
        f->lf_seek_low++;
    } else {
        size_t first = off - 0x7fffffffUL; /* 1 .. 0x7fffffff */
        r = Seek(f->dos_fh, (LONG)first, OFFSET_BEGINNING);
        if (r == -1) {
            f->pos_valid = 0;
            return 0;
        }
        r = Seek(f->dos_fh, (LONG)0x7fffffffL, OFFSET_CURRENT);
        if (r == -1) {
            f->pos_valid = 0;
            return 0;
        }
        f->lf_seek_high++;
        if (!f->lf_high_seek_announced) {
            DBG_PRINTF("[N96Q] direct AmigaDOS two-step seek reached source offset $%08lx\n",
                   (unsigned long)off);
            f->lf_high_seek_announced = 1;
        }
    }
    f->pos = off;
    f->pos_valid = 1;
    return 1;
}

static int n96q_dos_read_exact(file_source *f, unsigned char *dst, size_t len)
{
    LONG got;
    size_t old_pos;
    if (!f || !f->dos_fh || (!dst && len) || len > 0x7fffffffUL) return 0;
    old_pos = f->pos;
    got = Read(f->dos_fh, dst, (LONG)len);
    if (got != (LONG)len) {
        f->pos_valid = 0;
        return 0;
    }
    f->pos += (size_t)got;
    f->lf_direct_reads++;
    f->lf_direct_bytes += (unsigned long)got;
    n96q_note_advance(f, old_pos, f->pos);
    return 1;
}
#endif

static int file_seek_exact(file_source *f, size_t off)
{
    if (!f) return 0;
#if defined(AMIGA_M68K) || defined(__amigaos__) || defined(__AMIGA__)
    if (f->large_file_mode)
        return n96q_dos_seek_exact(f, off);
#endif
    if (!f->file || off > 0x7fffffffUL) return 0;
    if (!f->pos_valid || f->pos != off) {
        if (fseek(f->file, (long)off, SEEK_SET) != 0) {
            f->pos_valid = 0;
            return 0;
        }
        f->pos = off;
        f->pos_valid = 1;
    }
    return 1;
}

static int file_read_chunked(file_source *f, size_t off, unsigned char *dst, size_t len)
{
    size_t done = 0;
    if (!file_seek_exact(f, off)) return 0;
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > g_read_chunk) chunk = g_read_chunk;
#if defined(AMIGA_M68K) || defined(__amigaos__) || defined(__AMIGA__)
        if (f->large_file_mode) {
            if (!n96q_dos_read_exact(f, dst + done, chunk)) return 0;
        } else
#endif
        {
            if (fread(dst + done, 1, chunk, f->file) != chunk) {
                f->pos_valid = 0;
                return 0;
            }
            f->pos += chunk;
        }
        done += chunk;
    }
    return 1;
}

static int file_fill_readahead(file_source *f, size_t off)
{
    size_t want, done = 0;
    clock_t fill_started;
    unsigned long fill_ms;
    if (!f || !f->readahead || off > f->file_len) return 0;
    want = f->file_len - off;
    if (want > g_readahead) want = g_readahead;
    if (!file_seek_exact(f, off)) return 0;
    f->ra_fills++;
    fill_started = clock();
    while (done < want) {
        size_t chunk = want - done;
        clock_t read_started;
        unsigned long read_ms;
        if (chunk > g_read_chunk) chunk = g_read_chunk;
        read_started = clock();
#if defined(AMIGA_M68K) || defined(__amigaos__) || defined(__AMIGA__)
        if (f->large_file_mode) {
            if (!n96q_dos_read_exact(f, f->readahead + done, chunk)) {
                f->readahead_len = 0;
                return 0;
            }
        } else
#endif
        {
            if (fread(f->readahead + done, 1, chunk, f->file) != chunk) {
                f->pos_valid = 0;
                f->readahead_len = 0;
                return 0;
            }
            f->pos += chunk;
        }
        read_ms = elapsed_ms(read_started);
        f->ra_chunk_reads++;
        if (read_ms > f->ra_chunk_max_ms) f->ra_chunk_max_ms = read_ms;
        if (read_ms > 10) f->ra_chunk_gt10ms++;
        if (read_ms > 20) f->ra_chunk_gt20ms++;
        if (read_ms > 50) f->ra_chunk_gt50ms++;
        done += chunk;
    }
    fill_ms = elapsed_ms(fill_started);
    if (fill_ms > f->ra_fill_max_ms) f->ra_fill_max_ms = fill_ms;
    file_maybe_write_diag_snapshot(f);
    f->readahead_start = off;
    f->readahead_len = done;
    return 1;
}

static int file_read_at(void *opaque, size_t off, void *dst, size_t len)
{
    file_source *f = (file_source *)opaque;
    unsigned char *out = (unsigned char *)dst;
    if (!f || (!dst && len)) return 0;
    if (f->cached) {
        if (off > f->cached_len || len > f->cached_len - off) return 0;
        if (len) memcpy(dst, f->buffer + off, len);
        return 1;
    }
    if (!len) return 1;
#if defined(AMIGA_M68K) || defined(__amigaos__) || defined(__AMIGA__)
    if (f->large_file_mode) {
        if (!f->dos_fh) return 0;
    } else
#endif
    if (!f->file) return 0;

    /* Large caller ranges bypass the cache but still use <=64-KiB reads. */
    if (!f->readahead || len > g_readahead)
        return file_read_chunked(f, off, out, len);

    /* Serve a complete hit without touching DOS/USB. */
    if (off >= f->readahead_start &&
        off - f->readahead_start <= f->readahead_len &&
        len <= f->readahead_len - (off - f->readahead_start)) {
        f->ra_hits++;
        memcpy(out, f->readahead + (off - f->readahead_start), len);
        return 1;
    }

    /* N97Q boundary stitch: if a request starts inside the current 128-KiB
     * window but crosses its end, keep the still-valid tail and continue the
     * next fill exactly at the old window end.  N96Q discarded that tail and
     * refilled from `off`; in >2-GiB mode that forced Close/Open + Seek for
     * almost every sequential cache boundary and re-read overlapping bytes.
     *
     * The physical file position after a normal fill is already old_end, so
     * the new fill normally needs no reopen and no Seek at all.  Read size is
     * still capped at 64 KiB and the cache remains 128 KiB. */
    if (off >= f->readahead_start &&
        off - f->readahead_start < f->readahead_len) {
        size_t rel = off - f->readahead_start;
        size_t tail = f->readahead_len - rel;
        if (tail < len) {
            size_t remain = len - tail;
            size_t next_off = off + tail;
            memcpy(out, f->readahead + rel, tail);
            f->ra_stitch_hits++;
            f->ra_stitch_bytes += (unsigned long)tail;
            f->ra_misses++;
            if (!file_fill_readahead(f, next_off) || remain > f->readahead_len)
                return 0;
            memcpy(out + tail, f->readahead, remain);
            return 1;
        }
    }

    /* True miss: prefetch 128 KiB beginning at the requested offset as
     * 2 x <=64 KiB. */
    f->ra_misses++;
    if (!file_fill_readahead(f, off) || len > f->readahead_len) return 0;
    memcpy(out, f->readahead, len);
    return 1;
}

static void file_write_io_log(file_source *f, int snapshot)
{
#if MR_DEBUG_LOGS
    FILE *log;
    if (!f) return;
    log = fopen("N68B_IO.TXT", "w");
    if (!log) return;
    fprintf(log, "N97QD SNAPSHOT=%lu\n", snapshot ? 1UL : 0UL);
    fprintf(log, "N68B READ_AHEAD_BYTES=%lu\n", (unsigned long)g_readahead);
    fprintf(log, "N68B READ_CHUNK_MAX_BYTES=%lu\n", (unsigned long)g_read_chunk);
    fprintf(log, "N68B FILE_SIZE=%lu\n", (unsigned long)f->file_len);
    fprintf(log, "N96Q LARGE_FILE_MODE=%lu\n", f->large_file_mode ? 1UL : 0UL);
    fprintf(log, "N96Q DOS_REOPENS=%lu\n", f->lf_reopens);
    fprintf(log, "N96Q DOS_SEEK_LOW=%lu\n", f->lf_seek_low);
    fprintf(log, "N96Q DOS_SEEK_HIGH=%lu\n", f->lf_seek_high);
    fprintf(log, "N96Q DOS_READS=%lu\n", f->lf_direct_reads);
    fprintf(log, "N96Q DOS_BYTES=%lu\n", f->lf_direct_bytes);
    fprintf(log, "N68B READ_AHEAD_ALLOC=%lu\n", f->readahead ? 1UL : 0UL);
    fprintf(log, "N68B CACHE_HITS=%lu\n", f->ra_hits);
    fprintf(log, "N68B CACHE_MISSES=%lu\n", f->ra_misses);
    fprintf(log, "N68B FILLS=%lu\n", f->ra_fills);
    fprintf(log, "N68B CHUNK_READS=%lu\n", f->ra_chunk_reads);
    fprintf(log, "N97Q STITCH_HITS=%lu\n", f->ra_stitch_hits);
    fprintf(log, "N97Q STITCH_BYTES=%lu\n", f->ra_stitch_bytes);
    fprintf(log, "N68B CHUNK_MAX_MS=%lu\n", f->ra_chunk_max_ms);
    fprintf(log, "N68B FILL_MAX_MS=%lu\n", f->ra_fill_max_ms);
    fprintf(log, "N68B CHUNK_GT10MS=%lu\n", f->ra_chunk_gt10ms);
    fprintf(log, "N68B CHUNK_GT20MS=%lu\n", f->ra_chunk_gt20ms);
    fprintf(log, "N68B CHUNK_GT50MS=%lu\n", f->ra_chunk_gt50ms);
    fclose(log);
#else
    (void)f; (void)snapshot;
#endif
}

static void file_maybe_write_diag_snapshot(file_source *f)
{
    if (!f || f->diag_snapshot_written) return;
    if (elapsed_ms(f->diag_started) < 180000UL) return;
    file_write_io_log(f, 1);
    f->diag_snapshot_written = 1;
}

static void file_close(void *opaque)
{
    file_source *f = (file_source *)opaque;
    if (!f) return;
    file_write_io_log(f, 0);
    if (f->file) fclose(f->file);
#if defined(AMIGA_M68K) || defined(__amigaos__) || defined(__AMIGA__)
    if (f->dos_fh) Close(f->dos_fh);
#endif
    mr_free(f->buffer);
    mr_free(f->readahead);
    free(f);
}

/* N95Q: classic stdio SEEK_END/ftell cannot represent a local file above
 * 2 GiB.  For Matroska only, recover the real logical file length from the
 * EBML Segment element without seeking to EOF.  This changes size discovery
 * only; the proven N68B read/cache path below remains untouched. */
static int n94q_mkv_segment_file_len(const char *path, size_t *out_len)
{
    FILE *f;
    unsigned char *buf;
    size_t got, i;
    const size_t probe_bytes = 64u * 1024u;
    if (!path || !out_len) return 0;
    f = fopen(path, "rb");
    if (!f) return 0;
    buf = (unsigned char *)malloc(probe_bytes);
    if (!buf) { fclose(f); return 0; }
    got = fread(buf, 1, probe_bytes, f);
    fclose(f);
    if (got < 8) { free(buf); return 0; }

    for (i = 0; i + 5 < got; ++i) {
        size_t p, width, j;
        unsigned char first, marker;
        unsigned long long value, unknown, total;
        if (buf[i] != 0x18 || buf[i+1] != 0x53 ||
            buf[i+2] != 0x80 || buf[i+3] != 0x67)
            continue;
        p = i + 4;
        first = buf[p];
        if (!first) continue;
        marker = 0x80u;
        width = 1;
        while (width <= 8 && !(first & marker)) {
            marker >>= 1;
            ++width;
        }
        if (width > 8 || p + width > got) continue;
        value = (unsigned long long)(first & (marker - 1u));
        for (j = 1; j < width; ++j)
            value = (value << 8) | (unsigned long long)buf[p + j];
        unknown = (1ULL << (7u * width)) - 1ULL;
        if (value == unknown) continue;
        total = (unsigned long long)(p + width) + value;
        if (total == 0 || total > 0xfffffffeULL) continue;
        *out_len = (size_t)total;
        free(buf);
        return 1;
    }
    free(buf);
    return 0;
}

static mr_source *open_local_file(const char *path, size_t requested_buffer)
{
    file_source *ctx;
    mr_source *source;
    size_t actual_buffer;
    long end = 0;
    size_t file_len = 0;
    int large_file_mode = 0;
    FILE *file = NULL;
#if defined(AMIGA_M68K) || defined(__amigaos__) || defined(__AMIGA__)
    BPTR dos_fh = 0;
#endif

    /* Determine the file size through a throwaway stream, before the real
     * stream performs any operation of its own - setvbuf() below must run
     * before the first operation on *its* stream, so size discovery cannot
     * share it (a seek would already count as an operation). */
    {
        FILE *probe = fopen(path, "rb");
        if (!probe) {
            mr_source_set_error("cannot open local file");
            return NULL;
        }
        if (fseek(probe, 0, SEEK_END) == 0 && (end = ftell(probe)) > 0) {
            file_len = (size_t)end;
            fclose(probe);
        } else {
            fclose(probe);
            if (!n94q_mkv_segment_file_len(path, &file_len)) {
                mr_source_set_error("cannot determine local file size");
                return NULL;
            }
            large_file_mode = file_len > 0x7fffffffUL;
            DBG_PRINTF("[N96Q] >2GiB MKV: logical size from EBML Segment = %lu bytes\n",
                   (unsigned long)file_len);
            if (large_file_mode)
                DBG_PRINTF("[N96Q] direct AmigaDOS large-file reader enabled\n");
        }
    }

#if defined(AMIGA_M68K) || defined(__amigaos__) || defined(__AMIGA__)
    if (large_file_mode) {
        dos_fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
        if (!dos_fh) {
            mr_source_set_error("cannot open large local file with AmigaDOS");
            return NULL;
        }
    } else
#endif
    {
        file = fopen(path, "rb");
        if (!file) {
            mr_source_set_error("cannot open local file");
            return NULL;
        }
    }

    ctx = (file_source *)calloc(1, sizeof *ctx);
    if (!ctx) {
        if (file) fclose(file);
#if defined(AMIGA_M68K) || defined(__amigaos__) || defined(__AMIGA__)
        if (dos_fh) Close(dos_fh);
#endif
        mr_source_set_error("not enough memory for local file");
        return NULL;
    }

    ctx->file_len = file_len;
    ctx->large_file_mode = large_file_mode;
    ctx->diag_started = clock();
    ctx->diag_snapshot_written = 0;
#if defined(AMIGA_M68K) || defined(__amigaos__) || defined(__AMIGA__)
    ctx->dos_fh = dos_fh;
#endif
    if (large_file_mode) {
        size_t n = strlen(path);
        if (n >= sizeof ctx->dos_path) n = sizeof ctx->dos_path - 1;
        memcpy(ctx->dos_path, path, n);
        ctx->dos_path[n] = '\0';
    }

    /* Whole file fits inside the Fast buffer budget: read it once, up
     * front, into a block sized to the file itself, then close the stream
     * entirely. Every later mr_source_read_at() is then served straight
     * from memory - no further disk I/O, which is what a "Fast buffer"
     * actually promises. Merely widening libc's stdio window (below)
     * cannot promise that once a demuxer's reads stop being sequential
     * (index parsing, interleaved chunks that outrun the window) - that
     * gap is what let a small file "spam" the disk with read requests
     * instead of being buffered once. */
    if (!large_file_mode && requested_buffer && file_len <= requested_buffer) {
        unsigned char *buffer = (unsigned char *)mr_alloc_fast(file_len);
        if (buffer) {
            if (fread(buffer, 1, file_len, file) == file_len) {
                fclose(file);
                ctx->buffer = buffer;
                ctx->buffer_size = file_len;
                ctx->cached = 1;
                ctx->cached_len = file_len;
                source = mr_source_create(ctx, file_len, file_read_at,
                                          file_close, path);
                mr_source_set_buffer_capacity(source, file_len);
                mr_source_set_memory_cached(source, 1);
                return source;
            }
            mr_free(buffer);
            fclose(file);
            free(ctx);
            mr_source_set_error("cannot read local file");
            return NULL;
        }
        /* Not enough contiguous Fast RAM for even the exact file size -
         * fall through to the windowed stdio buffer below. */
    }

    /* setvbuf() must run before the first operation on the stream; `file`
     * is freshly opened and untouched at this point in every remaining
     * path, so each branch below is still that first operation. */
    if (!large_file_mode) {
        if (requested_buffer) {
            size_t attempt = requested_buffer;
            while (attempt >= 4u * 1024u * 1024u) {
                unsigned char *buffer = (unsigned char *)mr_alloc_fast(attempt);
                if (buffer) {
                    if (setvbuf(file, (char *)buffer, _IOFBF, attempt) == 0) {
                        ctx->buffer = buffer;
                        ctx->buffer_size = attempt;
                        break;
                    }
                    mr_free(buffer);
                }
                attempt /= 2;
            }
        }
        if (!ctx->buffer)
            (void)setvbuf(file, NULL, _IOFBF,
                          g_read_chunk > MR_LOCAL_FILE_BUFFER_SIZE ?
                          g_read_chunk : MR_LOCAL_FILE_BUFFER_SIZE);
    }

    /* N68: a real read-ahead layer above stdio.  The stdio window remains
     * 64 KiB; the explicit cache is 128 KiB and is filled in two <=64-KiB
     * fread() calls. Allocation failure simply preserves the old path. */
    ctx->readahead = (unsigned char *)mr_alloc_fast(g_readahead);
    if (!ctx->readahead && g_readahead > MR_LOCAL_FILE_READAHEAD) {
        /* No room for the bigger window: fall back to the default sizes. */
        g_read_chunk = MR_LOCAL_FILE_READ_CHUNK;
        g_readahead = MR_LOCAL_FILE_READAHEAD;
        ctx->readahead = (unsigned char *)mr_alloc_fast(g_readahead);
    }
    ctx->readahead_start = 0;
    ctx->readahead_len = 0;

    ctx->file = file;
    ctx->pos = 0;
    ctx->pos_valid = 1;
    actual_buffer = ctx->buffer_size;
    source = mr_source_create(ctx, file_len, file_read_at, file_close, path);
    mr_source_set_buffer_capacity(source, actual_buffer);
    return source;
}

mr_source *mr_source_open_ex(const char *path,
                             const struct mr_http_options *options)
{
    char youtube_media[MR_HTTP_URL_MAX];
    mr_http_options youtube_options;
    mr_youtube_media_kind youtube_kind;
    g_source_error[0] = '\0';
    if (!path || !*path) {
        mr_source_set_error("empty media path");
        return NULL;
    }
    if (mr_source_is_hls(path))
        return mr_hls_source_open_ex(path, options);
    if (mr_youtube_is_url(path)) {
        if (!mr_youtube_http_options_init(&youtube_options, options) ||
            !mr_youtube_resolve_media(path, &youtube_options, youtube_media,
                                      sizeof youtube_media, &youtube_kind) ||
            !mr_youtube_media_http_options_init(&youtube_options, options))
            return NULL;
        if (youtube_kind == MR_YOUTUBE_MEDIA_HLS ||
            youtube_kind == MR_YOUTUBE_MEDIA_HLS_VOD)
            return mr_hls_source_open_ex(youtube_media, &youtube_options);
        return mr_http_source_open_ex(youtube_media, &youtube_options);
    }
    if (mr_source_is_url(path))
        return mr_http_source_open_ex(path, options);
    return open_local_file(path, options ? options->source_buffer_bytes : 0);
}

mr_source *mr_source_open(const char *path)
{
    return mr_source_open_ex(path, NULL);
}

int mr_source_read_at(mr_source *s, size_t off, void *dst, size_t len)
{
    if (!s || (!dst && len)) return 0;
    /* Seekable sources are bounds-checked against their known size. A streaming
     * source has no known end, so the backend reports EOF via a short read. */
    if (s->len != MR_SOURCE_LEN_UNKNOWN &&
        (off > s->len || len > s->len - off))
        return 0;
    if (!g_timing_enabled)
        return s->read_at(s->ctx, off, dst, len);
    {
        clock_t started = clock();
        int ok = s->read_at(s->ctx, off, dst, len);
        unsigned long ms = elapsed_ms(started);
        g_timing.read_ms += ms;
        if (s->network) g_timing.network_ms += ms;
        return ok;
    }
}

size_t mr_source_length(const mr_source *s)
{
    return s ? s->len : 0;
}

int mr_source_is_streaming(const mr_source *s)
{
    return s && s->len == MR_SOURCE_LEN_UNKNOWN;
}

const char *mr_source_final_name(const mr_source *s)
{
    return s ? s->final_name : "";
}

void mr_source_close(mr_source *s)
{
    if (!s) return;
    s->close(s->ctx);
    mr_free(s);
}

size_t mr_source_buffer_capacity(const mr_source *s)
{
    return s ? s->buffer_capacity : 0;
}

void mr_source_set_buffer_capacity(mr_source *s, size_t bytes)
{
    if (s) s->buffer_capacity = bytes;
}

int mr_source_is_memory_cached(const mr_source *s)
{
    return s && s->memory_cached;
}

void mr_source_set_memory_cached(mr_source *s, int cached)
{
    if (s) s->memory_cached = cached != 0;
}
