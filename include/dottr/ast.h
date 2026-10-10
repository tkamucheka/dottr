#ifndef DOTTR_AST_H
#define DOTTR_AST_H

#include <stdbool.h>
#include <stddef.h>

#include "dottr/lexer.h"

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
    int line;
    int column;
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
    STMT_CHILD_PKG,
    STMT_CHECK,
    STMT_CREATE,
    STMT_APPLY,
    STMT_APPLY_IF,
    STMT_APPLY_UNLESS,
} StmtKind;

typedef struct {
    StmtKind kind;
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
            Name package;
            /* Registered child unit name, PARENT-child-PACKAGE. */
            const char *childName;
        } childPkg;
        /* STMT_CHECK and STMT_CREATE. */
        struct {
            PredicateKind kind;
            Name variable;
        } predicate;
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
    bool isBlock;
    Stmt *stmts;
    size_t stmtCount;
    /* Resolved by semantic analysis, in needs order without duplicates. */
    Unit **deps;
    size_t depCount;
};

typedef struct {
    Unit *units;
    size_t count;
} Manifest;

const char *predicateKindName(PredicateKind kind);

#endif
