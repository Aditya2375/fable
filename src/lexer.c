#include "lexer.h"

#include <string.h>
#include <ctype.h>
#include <stdbool.h>

void lexer_init(Lexer *lx, const char *src, int first_line) {
    lx->start = src;
    lx->cur = src;
    /* first_line lets the REPL report absolute input line numbers even
     * when the pending buffer holds only the current statement */
    lx->line = first_line;
    lx->last_token_line = first_line;
    lx->unterminated_comment_line = 0;
}

static bool at_end(Lexer *lx) { return *lx->cur == '\0'; }
static char advance(Lexer *lx) { lx->cur++; return lx->cur[-1]; }
static char peek(Lexer *lx) { return *lx->cur; }
static char peek_next(Lexer *lx) { return at_end(lx) ? '\0' : lx->cur[1]; }
static bool match(Lexer *lx, char want) {
    if (at_end(lx) || *lx->cur != want) return false;
    lx->cur++;
    return true;
}

static Token make(Lexer *lx, TokenType t) {
    Token tok;
    tok.type = t;
    tok.start = lx->start;
    tok.len = (int)(lx->cur - lx->start);
    tok.line = lx->line;
    return tok;
}

static Token error_at(int line, const char *msg) {
    Token tok;
    tok.type = T_ERROR;
    tok.start = msg;
    tok.len = (int)strlen(msg);
    tok.line = line;
    return tok;
}

static void skip_space(Lexer *lx) {
    for (;;) {
        char c = peek(lx);
        switch (c) {
            case ' ': case '\r': case '\t': advance(lx); break;
            case '\n': lx->line++; advance(lx); break;
            case '#':
                while (peek(lx) != '\n' && !at_end(lx)) advance(lx);
                break;
            case '/':
                if (peek_next(lx) == '/') {
                    while (peek(lx) != '\n' && !at_end(lx)) advance(lx);
                } else if (peek_next(lx) == '*') {
                    int comment_line = lx->line;   /* anchor errors where the comment opens */
                    advance(lx); advance(lx);
                    int depth = 1;
                    while (depth > 0 && !at_end(lx)) {
                        if (peek(lx) == '\n') lx->line++;
                        if (peek(lx) == '/' && peek_next(lx) == '*') { depth++; advance(lx); advance(lx); }
                        else if (peek(lx) == '*' && peek_next(lx) == '/') { depth--; advance(lx); advance(lx); }
                        else advance(lx);
                    }
                    if (depth > 0) lx->unterminated_comment_line = comment_line;
                } else return;
                break;
            default: return;
        }
    }
}

static TokenType check_kw(Lexer *lx, int skip, int len, const char *rest, TokenType t) {
    if (lx->cur - lx->start == skip + len && memcmp(lx->start + skip, rest, (size_t)len) == 0)
        return t;
    return T_IDENT;
}

static TokenType ident_type(Lexer *lx) {
    switch (lx->start[0]) {
        case 'a': return check_kw(lx, 1, 2, "nd", T_AND);
        case 'e': return check_kw(lx, 1, 3, "lse", T_ELSE);
        case 'f':
            if (lx->cur - lx->start > 1) {
                switch (lx->start[1]) {
                    case 'a': return check_kw(lx, 2, 3, "lse", T_FALSE);
                    case 'n': return check_kw(lx, 2, 0, "", T_FN);
                    case 'o': return check_kw(lx, 2, 1, "r", T_FOR);
                }
            }
            break;
        case 'i': return check_kw(lx, 1, 1, "f", T_IF);
        case 'l': return check_kw(lx, 1, 2, "et", T_LET);
        case 'n':
            if (lx->cur - lx->start > 1) {
                switch (lx->start[1]) {
                    case 'i': return check_kw(lx, 2, 1, "l", T_NIL);
                    case 'o': return check_kw(lx, 2, 1, "t", T_NOT);
                }
            }
            break;
        case 'o': return check_kw(lx, 1, 1, "r", T_OR);
        case 'r': return check_kw(lx, 1, 5, "eturn", T_RETURN);
        case 't': return check_kw(lx, 1, 3, "rue", T_TRUE);
        case 'w': return check_kw(lx, 1, 4, "hile", T_WHILE);
    }
    return T_IDENT;
}

static Token string_lit(Lexer *lx) {
    int start_line = lx->line;   /* the line of the opening quote */
    while (peek(lx) != '"' && !at_end(lx)) {
        if (peek(lx) == '\n') lx->line++;
        if (peek(lx) == '\\' && peek_next(lx) != '\0') { advance(lx); advance(lx); continue; }
        advance(lx);
    }
    if (at_end(lx)) return error_at(start_line, "unterminated string");
    advance(lx);   /* closing quote */
    return make(lx, T_STRING);
}

static Token number_lit(Lexer *lx) {
    while (isdigit((unsigned char)peek(lx))) advance(lx);
    if (peek(lx) == '.' && isdigit((unsigned char)peek_next(lx))) {
        advance(lx);
        while (isdigit((unsigned char)peek(lx))) advance(lx);
    }
    if (peek(lx) == 'e' || peek(lx) == 'E') {
        const char *save = lx->cur;
        advance(lx);
        if (peek(lx) == '+' || peek(lx) == '-') advance(lx);
        if (isdigit((unsigned char)peek(lx)))
            while (isdigit((unsigned char)peek(lx))) advance(lx);
        else
            lx->cur = save;   /* "1e" is 1 then ident e */
    }
    return make(lx, T_NUMBER);
}

Token lexer_next(Lexer *lx) {
    skip_space(lx);
    lx->start = lx->cur;
    if (lx->unterminated_comment_line) {
        int line = lx->unterminated_comment_line;
        lx->unterminated_comment_line = 0;
        return error_at(line, "unterminated block comment");
    }
    if (at_end(lx)) {
        /* "at end" errors point at the last real token's line, not past a
         * trailing newline - terminal REPL, -c and the wasm demo agree. */
        Token tok = make(lx, T_EOF);
        tok.line = lx->last_token_line;
        return tok;
    }
    lx->last_token_line = lx->line;
    char c = advance(lx);
    if (isalpha((unsigned char)c) || c == '_') {
        while (isalnum((unsigned char)peek(lx)) || peek(lx) == '_') advance(lx);
        return make(lx, ident_type(lx));
    }
    if (isdigit((unsigned char)c)) return number_lit(lx);
    switch (c) {
        case '(': return make(lx, T_LPAREN);
        case ')': return make(lx, T_RPAREN);
        case '{': return make(lx, T_LBRACE);
        case '}': return make(lx, T_RBRACE);
        case '[': return make(lx, T_LBRACKET);
        case ']': return make(lx, T_RBRACKET);
        case ',': return make(lx, T_COMMA);
        case '.': return make(lx, T_DOT);
        case ';': return make(lx, T_SEMICOLON);
        case ':': return make(lx, T_COLON);
        case '+': return make(lx, T_PLUS);
        case '-': return make(lx, T_MINUS);
        case '*': return make(lx, T_STAR);
        case '/': return make(lx, T_SLASH);
        case '%': return make(lx, T_PERCENT);
        case '!': return make(lx, match(lx, '=') ? T_BANGEQ : T_BANG);
        case '=': return make(lx, match(lx, '=') ? T_EQEQ : T_EQ);
        case '<': return make(lx, match(lx, '=') ? T_LTE : T_LT);
        case '>': return make(lx, match(lx, '=') ? T_GTE : T_GT);
        case '"': return string_lit(lx);
    }
    return error_at(lx->line, "unexpected character");
}

const char *token_name(TokenType t) {
    switch (t) {
        case T_LPAREN: return "'('"; case T_RPAREN: return "')'";
        case T_LBRACE: return "'{'"; case T_RBRACE: return "'}'";
        case T_LBRACKET: return "'['"; case T_RBRACKET: return "']'";
        case T_COMMA: return "','"; case T_DOT: return "'.'";
        case T_SEMICOLON: return "';'"; case T_COLON: return "':'";
        case T_PLUS: return "'+'"; case T_MINUS: return "'-'";
        case T_STAR: return "'*'"; case T_SLASH: return "'/'";
        case T_PERCENT: return "'%'";
        case T_BANG: return "'!'"; case T_BANGEQ: return "'!='";
        case T_EQ: return "'='"; case T_EQEQ: return "'=='";
        case T_LT: return "'<'"; case T_LTE: return "'<='";
        case T_GT: return "'>'"; case T_GTE: return "'>='";
        case T_IDENT: return "identifier"; case T_STRING: return "string";
        case T_NUMBER: return "number";
        case T_LET: return "'let'"; case T_IF: return "'if'";
        case T_ELSE: return "'else'"; case T_WHILE: return "'while'";
        case T_FOR: return "'for'"; case T_FN: return "'fn'";
        case T_RETURN: return "'return'"; case T_AND: return "'and'";
        case T_OR: return "'or'"; case T_NOT: return "'not'";
        case T_TRUE: return "'true'"; case T_FALSE: return "'false'";
        case T_NIL: return "'nil'";
        case T_ERROR: return "error"; case T_EOF: return "end of input";
    }
    return "?";
}

/* Is this chunk of source syntactically complete - every (, [, { closed and
 * no open string or block comment? The REPL and browser session use it to
 * keep reading continuation lines instead of erroring on half a program. */
bool fable_source_complete(const char *src) {
    int parens = 0, brackets = 0, braces = 0;
    for (const char *p = src; *p; p++) {
        switch (*p) {
            case '"':
                p++;
                while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
                if (!*p) return false;   /* open string */
                break;
            case '#':
                while (*p && *p != '\n') p++;
                if (!*p) goto counts;   /* comment runs to end of source */
                break;
            case '/':
                if (p[1] == '/') { while (*p && *p != '\n') p++; if (!*p) goto counts; }
                else if (p[1] == '*') {
                    int depth = 1;
                    p += 2;
                    while (*p && depth > 0) {
                        if (p[0] == '/' && p[1] == '*') { depth++; p += 2; }
                        else if (p[0] == '*' && p[1] == '/') { depth--; p += 2; }
                        else p++;
                    }
                    if (depth > 0) return false;   /* open block comment */
                    p--;
                }
                break;
            case '(': parens++; break;
            case ')': parens--; break;
            case '[': brackets++; break;
            case ']': brackets--; break;
            case '{': braces++; break;
            case '}': braces--; break;
        }
    }
counts:
    return parens <= 0 && brackets <= 0 && braces <= 0;
}
