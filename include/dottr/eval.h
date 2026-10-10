#ifndef DOTTR_EVAL_H
#define DOTTR_EVAL_H

#include <stddef.h>

#include "dottr/ast.h"
#include "dottr/diag.h"

typedef enum {
    /* Print the actions apply would perform without performing them. */
    EVAL_PLAN,
    /* Perform the actions. */
    EVAL_APPLY,
} EvalMode;

/*
 * Evaluates a manifest that has passed semantic analysis. Evaluates the
 * selected units and their transitive dependencies, or every unit if
 * selectedCount is 0, in dependency order. Progress goes to standard output
 * and errors to diag. Returns 0 if every evaluated unit was satisfied (or, in
 * plan mode, if the plan was produced), and 1 otherwise.
 */
int evaluateManifest(const Manifest *manifest, EvalMode mode,
                     Unit *const *selected, size_t selectedCount, Diag *diag);

#endif
