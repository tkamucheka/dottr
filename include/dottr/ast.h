#ifndef DOTTR_AST_H
#define DOTTR_AST_H

#include <stdbool.h>
#include <stddef.h>

#include "dottr/lexer.h"
#include "dottr/package.h"

/* A name with the source position at which it was written. */
typedef struct {
    const char *text;
    int line;
    int column;
} Name;

typedef enum {
    PREDICATE_COMMAND,
    PREDICATE_FILE,
    PREDICATE_DIRECTORY,
} PredicateKind;

/* Statements permitted in the body of apply, apply_if, and apply_unless. */
typedef enum {
    ACTION_SHELL,
} ActionKind;

typedef struct {
    ActionKind kind;
    /* Systems on which the action applies, narrowed by enclosing `on`. */
    SystemSet systems;
    /* Position of the `shell` keyword. */
    int line;
    int column;
    /* Position of the opening fence; its column - 1 is the margin. */
    int fenceLine;
    int fenceColumn;
    ShellDialect dialect;
    Template script;
} Action;

typedef struct {
    Action *items;
    size_t count;
} ActionList;

typedef enum {
    STMT_LOCAL,
    STMT_NEEDS,
    STMT_PKG,
    STMT_CHECK,
    STMT_CREATE,
    STMT_SYMLINK,
    STMT_APPLY,
    STMT_APPLY_IF,
    STMT_APPLY_UNLESS,
} StmtKind;

typedef struct {
    StmtKind kind;
    /* Systems on which the statement applies, narrowed by enclosing `on`. */
    SystemSet systems;
    int line;
    int column;
    union {
        struct {
            Name name;
            Template value;
        } local;
        struct {
            Name *names;
            size_t count;
        } needs;
        struct {
            Name *names;
            size_t count;
        } pkg;
        /* STMT_CHECK and STMT_CREATE. */
        struct {
            PredicateKind kind;
            Name variable;
        } predicate;
        struct {
            Name source;
            Name target;
            Name ignore;
            bool tree;
        } symlink;
        /*
         * STMT_APPLY, STMT_APPLY_IF, and STMT_APPLY_UNLESS. For STMT_APPLY the
         * variable is unused; elseActions is used only by STMT_APPLY_IF.
         */
        struct {
            Name variable;
            ActionList thenActions;
            bool hasElse;
            ActionList elseActions;
        } apply;
    } as;
} Stmt;

typedef struct Unit Unit;

struct Unit {
    Name name;
    /* Systems on which the unit exists, narrowed by enclosing `on`. */
    SystemSet systems;
    bool isBlock;
    /* Position of the closing `end` of a block-form unit; 0 otherwise. */
    int endLine;
    int endColumn;
    Stmt *stmts;
    size_t stmtCount;
    /* Resolved by semantic analysis, in needs order without duplicates. */
    Unit **deps;
    size_t depCount;
};

/*
 * An `on` block. The parser records the systems of its contents in their
 * own systems fields; the block itself is kept for editors.
 */
typedef struct {
    int line;
    int column;
    /* Position of the closing `end`. */
    int endLine;
    int endColumn;
    Name *selectors;
    size_t selectorCount;
    /* The systems of the block's contents, including enclosing blocks. */
    SystemSet systems;
} OnBlock;

typedef struct {
    Unit *units;
    size_t count;
    /* Every `on` block, in source order. */
    OnBlock *onBlocks;
    size_t onCount;
} Manifest;

const char *predicateKindName(PredicateKind kind);

#endif
