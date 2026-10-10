#ifndef DOTTR_SEMA_H
#define DOTTR_SEMA_H

#include <stdbool.h>

#include "dottr/ast.h"
#include "dottr/diag.h"
#include "dottr/util.h"

/*
 * Checks a parsed manifest against the static rules of the language and
 * resolves the dependencies of every unit (Unit.deps). Reports every error
 * found to diag and returns whether there were none. Does not observe or
 * modify the host.
 */
bool analyzeManifest(Arena *arena, Manifest *manifest, Diag *diag);

/* Returns the top-level unit with the given name, or NULL. */
Unit *findUnit(const Manifest *manifest, const char *name);

/* Whether a multi-component reference path names a built-in value. */
bool isBuiltinReference(const char *path);

#endif
