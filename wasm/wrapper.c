/* emscripten wrapper: paste the CLI into this translation unit and expose a
 * tiny session API to JavaScript. The VM persists between fable_exec calls
 * so definitions carry over; fable_boot builds a fresh VM. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE   /* open_memstream + fopencookie prototypes (also native tests) */
#endif
#define main fable_cli_main
#include "../src/main.c"
#undef main
#include <emscripten.h>
#include <stdint.h>
#include <sys/types.h>

static void pending_clear(void);

/* Session line numbering, matching the terminal REPL: web_lines counts every
 * input line received this session (accepted or dropped), and pending_start
 * is the 1-based session line where the current pending statement began, so
 * every error - compile, runtime or NUL - names the absolute session line
 * like the CLI's interpret_at(pending, "<repl>", pending_start). */
static int web_lines = 0;
static int pending_start = 1;

/* The VM writes to one combined byte stream, not emscripten's
 * stdout/stderr: the glue's print path flushes on NUL (splitting a printed
 * line in two), decodes invalid UTF-8 into invented glyphs, and two
 * separate streams would lose the stdout/stderr interleave order within
 * one fable_exec_len call. So vm.out and vm.err are fopencookie streams
 * over a single memstream; every write appends one record -
 *   [1 byte tag: 0 = stdout, 1 = stderr][4 bytes little-endian length][payload]
 * and the page walks the records in write order, merging adjacent same-tag
 * runs (round-20 byte-faithfulness, round-21 F2 order). fable_out_ptr/len
 * hand the page the record buffer; fable_out_drain starts a fresh one. */
static FILE *wbuf = NULL;              /* the combined record memstream */
static char *wbuf_bytes = NULL;
static size_t wbuf_len = 0;
static FILE *wout = NULL, *werr = NULL;   /* unbuffered cookie streams over wbuf */

static ssize_t wbuf_write(void *cookie, const char *data, size_t n) {
    unsigned char hdr[5];
    if (n == 0) return 0;   /* an fflush pokes a zero-length write: no record */
    hdr[0] = (unsigned char)(uintptr_t)cookie;   /* 0 = stdout, 1 = stderr */
    hdr[1] = (unsigned char)(n & 0xff);
    hdr[2] = (unsigned char)((n >> 8) & 0xff);
    hdr[3] = (unsigned char)((n >> 16) & 0xff);
    hdr[4] = (unsigned char)((n >> 24) & 0xff);
    if (fwrite(hdr, 1, sizeof hdr, wbuf) < sizeof hdr) return -1;
    if (fwrite(data, 1, n, wbuf) < n) return -1;
    return (ssize_t)n;
}

/* Returns 1 on the byte-faithful tagged path, 0 on the glue-stream
 * fallback. A partial failure cleans up whatever did open before falling
 * back - no leaked stream or buffer (round-21 F3). */
static int streams_open(void) {
    static const cookie_io_functions_t io = { NULL, wbuf_write, NULL, NULL };
    wbuf = open_memstream(&wbuf_bytes, &wbuf_len);
    wout = wbuf ? fopencookie((void *)(uintptr_t)0, "w", io) : NULL;
    werr = wbuf ? fopencookie((void *)(uintptr_t)1, "w", io) : NULL;
    if (wout) setvbuf(wout, NULL, _IONBF, 0);   /* per-write records keep the interleave order */
    if (werr) setvbuf(werr, NULL, _IONBF, 0);
    if (wbuf && wout && werr) return 1;
    if (werr) fclose(werr);
    if (wout) fclose(wout);
    if (wbuf) { fclose(wbuf); free(wbuf_bytes); }
    wbuf = NULL; wbuf_bytes = NULL; wbuf_len = 0;
    wout = stdout;   /* memstream/cookie OOM: fall back to the glue's streams */
    werr = stderr;
    return 0;
}

EMSCRIPTEN_KEEPALIVE
int fable_boot(void) {
    static int booted = 0;
    if (booted) {
        vm_free();
        if (wbuf) {
            fclose(werr);
            fclose(wout);
            fclose(wbuf);
            free(wbuf_bytes);
        }
    }
    pending_clear();
    web_lines = 0;
    pending_start = 1;
    booted = 1;
    if (!streams_open())
        fprintf(stderr, "fable: warning: output capture unavailable (out of memory); output decoding is approximate\n");
    vm_init(wout, werr);
    vm.repl = true;   /* expression statements echo, like the terminal REPL */
    return 0;
}

/* The pointer is only valid until the next VM write (the memstream may
 * reallocate), so the page copies the bytes and then drains. On the
 * fallback path there is no buffer: NULL/0, and the glue's print/printErr
 * carry output instead. */
EMSCRIPTEN_KEEPALIVE
const char *fable_out_ptr(void) { if (wbuf) fflush(wbuf); return wbuf_bytes; }
EMSCRIPTEN_KEEPALIVE
int fable_out_len(void) { if (wbuf) fflush(wbuf); return wbuf_bytes ? (int)wbuf_len : 0; }

EMSCRIPTEN_KEEPALIVE
void fable_out_drain(void) {
    if (!wbuf) return;   /* fallback streams: nothing buffered */
    fclose(werr);
    fclose(wout);
    fclose(wbuf);
    free(wbuf_bytes);
    streams_open();
    vm.out = wout;
    vm.err = werr;
}

/* Incomplete input (an open bracket, string or block comment) accumulates
 * until it completes, like the terminal REPL's "... " continuation. */
static char *pending = NULL;
static size_t pending_len = 0, pending_cap = 0;

static void pending_clear(void) {
    free(pending);
    pending = NULL;
    pending_len = pending_cap = 0;
}

/* Whole programs (the demo's example buttons) run without REPL echo;
 * typed lines keep it. */
EMSCRIPTEN_KEEPALIVE
void fable_set_echo(int on) { vm.repl = on != 0; }

/* Drop any half-typed multi-line input. Returns 1 if something was pending,
 * so the page can say what happened. Example buttons call this first:
 * running a program must not wedge the session on an old incomplete line. */
EMSCRIPTEN_KEEPALIVE
int fable_discard_pending(void) {
    int had = pending != NULL;
    pending_clear();
    return had;
}

/* One input line, exactly the CLI's getline-loop semantics: NUL check with
 * the absolute session line, per-line 64 KB pending cap, append, then run
 * when the accumulated input completes. Returns 1 on error; the session
 * continues either way. `terminated` marks a line that ended with '\n':
 * getline's byte count includes that newline, so the cap counts it - an
 * unterminated tail (the demo's typed lines, or EOF on stdin) has no
 * newline to count (round-19). The synthetic '\n' is still appended, so
 * pending lines stay separated like the CLI's. */
static int exec_line(const char *line, size_t n, int terminated) {
    size_t line_bytes = n + (terminated ? 1 : 0);   /* what getline would report */
    if (memchr(line, 0, n)) {
        fprintf(werr, "<web>:%d: NUL byte in source (source must be text)\n", web_lines + 1);
        fflush(werr);
        pending_clear();   /* a dropped line corrupts any pending input, like the CLI */
        web_lines++;
        return 1;
    }
    if (pending_len + line_bytes >= 65536) {
        fprintf(werr, "<web>: input too long\n");
        fflush(werr);
        pending_clear();
        web_lines++;
        return 1;
    }
    if (pending_len + n + 2 > pending_cap) {
        size_t new_cap = (pending_len + n + 2) * 2;
        char *np = realloc(pending, new_cap);   /* temp: a failed realloc must not leak the old buffer */
        if (!np) {
            fprintf(werr, "<web>: out of memory receiving input\n");
            fflush(werr);
            pending_clear();   /* the half-received statement cannot complete */
            web_lines++;
            return 1;
        }
        pending = np;
        pending_cap = new_cap;
    }
    if (pending_len == 0) pending_start = web_lines + 1;
    memcpy(pending + pending_len, line, n);
    pending_len += n;
    pending[pending_len++] = '\n';
    pending[pending_len] = '\0';
    web_lines++;
    if (!fable_source_complete(pending)) {
        if (vm.repl) { fprintf(wout, "...\n"); fflush(wout); }   /* typed-line continuation echo */
        return 0;   /* waiting for more lines */
    }
    InterpretResult r = interpret_at(pending, "<web>", pending_start);
    vm.had_runtime_error = false;   /* one bad line must not kill the session */
    pending_clear();
    return r == INTERPRET_OK ? 0 : 1;
}

EMSCRIPTEN_KEEPALIVE
int fable_exec_len(const char *src, int len) {
    /* The JS side marshals an explicit length, so a NUL byte is a loud
     * error, never a silent truncation (round-13 F1). The call is processed
     * line by line with exactly the CLI's getline-loop semantics: a
     * trailing newline terminates the last line instead of adding a blank
     * one (round-18 F1), the 64 KB pending cap is checked per line
     * (round-18 F2), and a dropped line (NUL, over-cap, OOM) drops any
     * pending input while the lines around it still run. */
    int had_error = 0;
    int start = 0, i = 0;
    while (i < len) {
        if (src[i] != '\n') { i++; continue; }
        if (exec_line(src + start, (size_t)(i - start), 1)) had_error = 1;
        start = ++i;
    }
    if (start < len || len == 0)   /* unterminated tail; an empty call is one empty line */
        if (exec_line(src + start, (size_t)(len - start), 0)) had_error = 1;
    fflush(wout);
    fflush(werr);
    return had_error ? 1 : (pending_len ? 2 : 0);
}
