/* fable: the interpreter front end - REPL and script runner. */
#include "vm.h"
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Reads the whole file. Returns NULL on I/O error; *nul_line is set to the
 * 1-based line of the first NUL byte (source is lexed as a C string, so a
 * NUL would silently truncate the program - reject it instead). */
static char *read_file(const char *path, int *nul_line) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    const char *z = memchr(buf, 0, got);
    if (z) {
        int line = 1;
        for (const char *p = buf; p < z; p++) if (*p == '\n') line++;
        *nul_line = line;
        free(buf);
        return NULL;
    }
    return buf;
}

static void usage(FILE *out) {
    fprintf(out, "usage: fable [script.fable] [-c \"CODE\"] [-disasm]\n"
                 "  no arguments: start the REPL\n"
                 "  -c CODE     run CODE and exit\n"
                 "  -disasm     print bytecode before running\n");
}

int main(int argc, char **argv) {
    const char *path = NULL;
    const char *one = NULL;
    int disasm = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c")) {
            if (i + 1 >= argc) { usage(stderr); return 2; }
            one = argv[++i];
        } else if (!strcmp(argv[i], "-disasm")) disasm = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(stdout); return 0; }
        else if (argv[i][0] == '-') { usage(stderr); return 2; }
        else {
            if (path) { fprintf(stderr, "fable: extra argument '%s' (one script only)\n", argv[i]); usage(stderr); return 2; }
            path = argv[i];
        }
    }

    if (path && one) {
        fprintf(stderr, "fable: give a script or -c, not both\n");
        usage(stderr);
        return 2;
    }

    vm_init(stdout, stderr);
    vm.disasm = disasm;
    int rc = 0;

    if (one) {
        vm.repl = false;
        InterpretResult r = interpret(one, "<cmdline>");
        rc = r == INTERPRET_OK ? 0 : 1;
    } else if (path) {
        int nul_line = 0;
        char *src = read_file(path, &nul_line);
        if (!src) {
            if (nul_line)
                fprintf(stderr, "fable: %s:%d: NUL byte in source file (source must be text)\n",
                        path, nul_line);
            else
                fprintf(stderr, "fable: cannot read %s\n", path);
            vm_free();
            return 1;
        }
        vm.repl = false;
        InterpretResult r = interpret(src, path);
        free(src);
        rc = r == INTERPRET_OK ? 0 : 1;
    } else {
        /* REPL: top-level expression statements echo their value; input that
         * leaves a bracket, string or block comment open continues on the
         * next line with a "... " prompt */
        vm.repl = true;
        int interactive = isatty(0);
        char line[4096];
        char pending[65536];
        pending[0] = '\0';
        for (;;) {
            if (interactive) {
                printf(pending[0] ? "... " : "fable> ");
                fflush(stdout);
            }
            if (!fgets(line, sizeof line, stdin)) {
                if (pending[0]) {
                    fprintf(stderr, "<repl>: unexpected end of input (unclosed bracket, string or comment)\n");
                    rc = 1;
                }
                break;
            }
            if (!strchr(line, '\n') && !feof(stdin)) {
                int ch; while ((ch = getchar()) != '\n' && ch != EOF) {}
                fprintf(stderr, "<repl>: line too long (over %d chars)\n", (int)sizeof line - 2);
                pending[0] = '\0';   /* a dropped line corrupts any pending input */
                rc = 1;
                continue;
            }
            if (strlen(pending) + strlen(line) >= sizeof pending - 1) {
                fprintf(stderr, "<repl>: input too long\n");
                pending[0] = '\0';
                rc = 1;
                continue;
            }
            strcat(pending, line);
            if (!fable_source_complete(pending)) continue;   /* read more lines */
            interpret(pending, "<repl>");
            vm.had_runtime_error = false;
            pending[0] = '\0';
        }
        if (interactive) printf("\n");
    }

    vm_free();
    return rc;
}
