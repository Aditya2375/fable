/* fable: the interpreter front end - REPL and script runner. */
#include "vm.h"
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *read_file(const char *path) {
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

    vm_init(stdout, stderr);
    vm.disasm = disasm;
    int rc = 0;

    if (one) {
        vm.repl = false;
        InterpretResult r = interpret(one, "<cmdline>");
        rc = r == INTERPRET_OK ? 0 : 1;
    } else if (path) {
        char *src = read_file(path);
        if (!src) { fprintf(stderr, "fable: cannot read %s\n", path); vm_free(); return 1; }
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
            if (!fgets(line, sizeof line, stdin)) break;
            if (!strchr(line, '\n') && !feof(stdin)) {
                int ch; while ((ch = getchar()) != '\n' && ch != EOF) {}
                fprintf(stderr, "line too long\n");
                continue;
            }
            if (strlen(pending) + strlen(line) >= sizeof pending - 1) {
                fprintf(stderr, "input too long\n");
                pending[0] = '\0';
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
