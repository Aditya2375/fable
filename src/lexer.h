/* Fable lexer. */
#ifndef FABLE_LEXER_H
#define FABLE_LEXER_H

#include <stdbool.h>

typedef enum {
    /* punctuation */
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
    T_COMMA, T_DOT, T_SEMICOLON, T_COLON,
    /* operators */
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_BANG, T_BANGEQ, T_EQ, T_EQEQ,
    T_LT, T_LTE, T_GT, T_GTE,
    /* literals */
    T_IDENT, T_STRING, T_NUMBER,
    /* keywords */
    T_LET, T_IF, T_ELSE, T_WHILE, T_FOR, T_FN, T_RETURN,
    T_AND, T_OR, T_NOT, T_TRUE, T_FALSE, T_NIL,
    T_ERROR, T_EOF
} TokenType;

typedef struct {
    TokenType type;
    const char *start;
    int len;
    int line;
} Token;

typedef struct {
    const char *start;
    const char *cur;
    int line;
    int last_token_line;   /* line of the last real token: anchors EOF errors */
    Token error;      /* first lex error, sticky */
    bool failed;
    bool unterminated_comment;
} Lexer;

void lexer_init(Lexer *lx, const char *src);
bool fable_source_complete(const char *src);
Token lexer_next(Lexer *lx);
const char *token_name(TokenType t);

#endif
