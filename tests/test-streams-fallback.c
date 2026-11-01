/* Native test for the wasm wrapper's output-capture fallback (round-21 F3):
 * inject failures into open_memstream/fopencookie and check that boot falls
 * back to the glue streams, closes and frees whatever did open, and the
 * session still runs. Run under ASan/LSan (make sanitize) so a missed
 * cleanup is a leak failure, not a silent boot leak. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

static int fail_open_memstream;   /* how many upcoming open_memstream calls fail */
static int fail_fopencookie_after = -1;   /* allow this many cookies, then fail */

static FILE *test_open_memstream(char **b, size_t *s);
static FILE *test_fopencookie(void *c, const char *m, cookie_io_functions_t io);

#define open_memstream(b, s) test_open_memstream(b, s)
#define fopencookie(c, m, io) test_fopencookie(c, m, io)

#include "../../wasm/wrapper.c"

#undef open_memstream
#undef fopencookie

static FILE *test_open_memstream(char **b, size_t *s) {
    if (fail_open_memstream > 0) { fail_open_memstream--; errno = ENOMEM; return NULL; }
    return open_memstream(b, s);
}
static FILE *test_fopencookie(void *c, const char *mo, cookie_io_functions_t io) {
    if (fail_fopencookie_after == 0) { errno = ENOMEM; return NULL; }
    if (fail_fopencookie_after > 0) fail_fopencookie_after--;
    return fopencookie(c, mo, io);
}

static int fails = 0;
#define CHECK(cond, label) do { \
    if (cond) printf("ok: %s\n", label); \
    else { printf("FAIL: %s\n", label); fails = 1; } } while (0)

int main(void) {
    /* Case A: the memstream itself fails - all-opens-fail lands on the glue
     * streams and the session still runs. */
    fail_open_memstream = 1;
    CHECK(fable_boot() == 0, "fallback: boot succeeds when open_memstream fails");
    CHECK(wbuf == NULL && wout == stdout && werr == stderr,
          "fallback: all-opens-fail falls back to the glue streams");
    CHECK(fable_exec_len("print(40 + 2);", 14) == 0, "fallback: exec works on glue streams");

    /* Case B: the memstream opens, BOTH cookie streams fail - the opened
     * memstream must be closed and freed before the fallback (LSan). */
    fail_fopencookie_after = 0;
    CHECK(fable_boot() == 0, "fallback: re-boot succeeds when both cookies fail");
    CHECK(wbuf == NULL && wout == stdout && werr == stderr,
          "fallback: memstream-only partial failure falls back cleanly");

    /* Case C: memstream + first cookie open, second cookie fails - both
     * opened streams must be cleaned up before the fallback (LSan). */
    fail_fopencookie_after = 1;
    CHECK(fable_boot() == 0, "fallback: re-boot succeeds when the second cookie fails");
    CHECK(wbuf == NULL && wout == stdout && werr == stderr,
          "fallback: second-cookie partial failure falls back cleanly");

    /* Case D: no failures - the tagged byte-faithful path is live. */
    fail_fopencookie_after = -1;
    CHECK(fable_boot() == 0, "fallback: boot healthy with no failures");
    CHECK(wbuf != NULL && wout != stdout && werr != stderr,
          "fallback: no failures - the tagged path is live");
    CHECK(fable_exec_len("print(1);", 9) == 0 && fable_out_len() > 0,
          "fallback: the tagged path buffers output records");
    fable_out_drain();
    CHECK(fable_out_len() == 0, "fallback: drain resets the record buffer");
    printf("%s\n", fails ? "FAILURES" : "all stream-fallback checks passed");
    return fails;
}
