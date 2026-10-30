#ifndef DOTTR_LEXER_H
#define DOTTR_LEXER_H

#include <stdbool.h>
#include <stddef.h>

#include "dottr/diag.h"
#include "dottr/util.h"

typedef enum {
    TOK_EOF,
    TOK_NEWLINE,
    TOK_IDENT,
    TOK_STRING,
    TOK_RAW_BLOCK,
    TOK_EQUALS,
    TOK_COMMA,

    /* Keywords. */
    TOK_PKG,
    TOK_UNIT,
    TOK_SYSTEMS,
    TOK_DO,
    TOK_END,
    TOK_LOCAL,
    TOK_NEEDS,
    TOK_CHECK,
    TOK_CREATE,
    TOK_APPLY,
    TOK_APPLY_IF,
    TOK_APPLY_UNLESS,
    TOK_ELSE,
    TOK_SHELL,
    TOK_COMMAND,
    TOK_FILE,
    TOK_DIRECTORY,
    TOK_ON,
    TOK_SYMLINK,
} TokenKind;

/*
 * Shell named by the info string of a raw block. SHELL_DEFAULT, a block
 * without an info string, runs with sh, or with powershell on Windows.
 */
typedef enum {
    SHELL_DEFAULT,
    SHELL_SH,
    SHELL_BASH,
    SHELL_ZSH,
    SHELL_PWSH,
    SHELL_POWERSHELL,
} ShellDialect;

typedef enum {
    TEMPLATE_TEXT,
    TEMPLATE_REF,
} TemplatePartKind;

/*
 * One part of a string literal or raw block. For TEMPLATE_TEXT, text is the
 * literal text after escape processing and margin removal. For TEMPLATE_REF,
 * text is the dotted reference path without the surrounding "@{" and "}",
 * and line and column locate the '@'. References occur only in string
 * literals; a raw block consists of TEMPLATE_TEXT parts.
 */
typedef struct {
    TemplatePartKind kind;
    const char *text;
    size_t length;
    int line;
    int column;
} TemplatePart;

typedef struct {
    TemplatePart *parts;
    size_t count;
} Template;

typedef struct {
    TokenKind kind;
    int line;
    int column;
    /* Source lexeme; for TOK_STRING and TOK_RAW_BLOCK, see value instead. */
    const char *text;
    size_t length;
    /* Contents of TOK_STRING and TOK_RAW_BLOCK. */
    Template value;
    /* Info string of TOK_RAW_BLOCK. */
    ShellDialect dialect;
} Token;

/* A comment: text from '#' to the end of the line, without the line end. */
typedef struct {
    int line;
    int column;
    const char *text;
    size_t length;
} Comment;

typedef struct {
    Token *items;
    size_t count;
    /* Comments in source order; not part of the token sequence. */
    Comment *comments;
    size_t commentCount;
} TokenList;

/*
 * Tokenizes source. All token data is allocated in arena. Returns false
 * after reporting the first lexical error to diag.
 */
bool lexSource(Arena *arena, const char *source, size_t length,
               TokenList *tokens, Diag *diag);

/* Human-readable token kind, for example "`pkg`" or "identifier". */
const char *tokenKindName(TokenKind kind);

/*
 * Interpreter name for a dialect: "sh", "bash", "zsh", "pwsh", or
 * "powershell". The interpreter of SHELL_DEFAULT is "sh" unless windows.
 */
const char *shellDialectInterpreter(ShellDialect dialect, bool windows);

/* Whether a dialect runs with PowerShell. */
bool shellDialectIsPowerShell(ShellDialect dialect, bool windows);

#endif
