#ifndef DOTTR_SEMA_H
#define DOTTR_SEMA_H

#include <stdbool.h>

#include "dottr/ast.h"
#include "dottr/diag.h"
#include "dottr/util.h"

/*
 * Checks a parsed manifest against the static rules of the language on
 * every system, and reports every error found to diag, once, naming the
 * systems on which it occurs unless it occurs on all. Returns whether there
 * were none. A manifest without `on` blocks is checked once, and the
 * dependencies of its units (Unit.deps) are resolved. Does not observe or
 * modify the host.
 */
bool analyzeManifest(Arena *arena, Manifest *manifest, Diag *diag);

/*
 * Builds the view of a manifest on systems: the units, statements, and
 * actions that apply on every one of them, with the dependencies of the
 * units resolved. Returns false if the view breaks a static rule, which
 * cannot happen for a single system of a manifest that analyzeManifest
 * accepted.
 */
bool selectManifest(Arena *arena, const Manifest *manifest,
                    SystemSet systems, Manifest *view);

/*
 * Whether every system of systems has the same view of the manifest: no
 * `on` block applies on some of them but not all.
 */
bool viewsAgree(const Manifest *manifest, SystemSet systems);

/* Returns the top-level unit with the given name, or NULL. */
Unit *findUnit(const Manifest *manifest, const char *name);

/* Whether a multi-component reference path names a built-in value. */
bool isBuiltinReference(const char *path);

/*
 * The environment variable that a variable name would replace, if that
 * variable controls how a shell starts or finds commands, and NULL
 * otherwise. Names are compared without regard to case, as Windows compares
 * environment variable names.
 */
const char *protectedVariable(const char *name);

#endif
