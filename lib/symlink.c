#include "dottr/symlink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dottr/host.h"
#include "dottr/ignore.h"
#include "dottr/util.h"

typedef struct {
    const char *source;
    const char *target;
    bool directory;
    bool unchanged;
} LinkOperation;

typedef struct {
    Arena arena;
    LinkOperation *items;
    size_t count;
    size_t capacity;
    char *message;
    size_t messageSize;
    const char *sourceBase;
    const char *sourceRoot;
    IgnoreList ignored;
} LinkPlan;

static char *parentPath(const char *path)
{
    size_t length = strlen(path);
    size_t root = hostPathRootLength(path);
    while (length > root && !hostPathSeparator(path[length - 1])) {
        length--;
    }
    while (length > root && hostPathSeparator(path[length - 1])) {
        length--;
    }
    char *parent = xmalloc(length + 1);
    memcpy(parent, path, length);
    parent[length] = '\0';
    return parent;
}

static const char *joinPath(LinkPlan *plan, const char *parent,
                            const char *name)
{
    size_t length = strlen(parent);
    bool separator = length > 0 && !hostPathSeparator(parent[length - 1]);
    size_t size = length + (separator ? 1 : 0) + strlen(name) + 1;
    char *result = arenaAlloc(&plan->arena, size);
    snprintf(result, size, "%s%s%s", parent, separator ? "/" : "", name);
    return result;
}

static bool pathWithin(const char *parent, const char *path)
{
    size_t length = strlen(parent);
    if (strlen(path) < length) {
        return false;
    }
    char *prefix = xmalloc(length + 1);
    memcpy(prefix, path, length);
    prefix[length] = '\0';
    bool equal = hostPathsEqual(parent, prefix);
    free(prefix);
    return equal &&
           (path[length] == '\0' || hostPathSeparator(parent[length - 1]) ||
            hostPathSeparator(path[length]));
}

/* Refuse traversal through target directory links, including dangling links. */
static bool inspectParents(LinkPlan *plan, const char *target, bool tree)
{
    char *parent = parentPath(target);
    size_t root = hostPathRootLength(parent);
    size_t length = strlen(parent);
    bool ok = true;
    for (size_t i = root; i <= length && ok; i++) {
        if (i != length && !hostPathSeparator(parent[i])) {
            continue;
        }
        char saved = parent[i];
        parent[i] = '\0';
        HostPathKind kind;
        ok = hostPathKind(parent, &kind, plan->message, plan->messageSize);
        if (ok && kind != HOST_PATH_DIRECTORY &&
            !(tree && kind == HOST_PATH_MISSING)) {
            snprintf(plan->message, plan->messageSize,
                     "target parent %s is not a real directory; "
                     "create missing parents explicitly and resolve conflicts",
                     parent);
            ok = false;
        }
        parent[i] = saved;
    }
    free(parent);
    return ok;
}

static bool addOperation(LinkPlan *plan, const char *source, const char *target,
                         bool directory)
{
    if (plan->sourceRoot && pathWithin(plan->sourceRoot, target)) {
        snprintf(plan->message, plan->messageSize,
                 "tree mapping enters its own source at %s", target);
        return false;
    }
    HostPathKind kind;
    if (!hostPathKind(target, &kind, plan->message, plan->messageSize)) {
        return false;
    }
    bool unchanged = false;
    if (directory && kind == HOST_PATH_DIRECTORY) {
        unchanged = true;
    } else if (!directory && kind == HOST_PATH_SYMLINK) {
        if (!hostSymlinkMatches(source, target, &unchanged, plan->message,
                                plan->messageSize)) {
            return false;
        }
    }
    if (kind != HOST_PATH_MISSING && !unchanged) {
        snprintf(plan->message, plan->messageSize,
                 "symlink target %s conflicts with an existing entry; "
                 "nothing was replaced",
                 target);
        return false;
    }
    plan->items = arenaGrow(&plan->arena, plan->items, plan->count,
                            &plan->capacity, sizeof *plan->items);
    plan->items[plan->count++] =
        (LinkOperation){source, target, directory, unchanged};
    return true;
}

static bool collectTree(LinkPlan *plan, const char *source, const char *target)
{
    HostPathKind targetKind;
    if (!hostPathKind(target, &targetKind, plan->message, plan->messageSize)) {
        return false;
    }
    /* Retain a directory link already pointing to this complete subtree. */
    if (targetKind == HOST_PATH_SYMLINK) {
        return addOperation(plan, source, target, false);
    }
    if (!addOperation(plan, source, target, true)) {
        return false;
    }
    char **names;
    size_t count;
    if (!hostDirectoryEntries(source, &names, &count, plan->message,
                              plan->messageSize)) {
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < count; i++) {
        if (ok) {
            const char *from = joinPath(plan, source, names[i]);
            const char *to = joinPath(plan, target, names[i]);
            const char *relative = from + strlen(plan->sourceBase) + 1;
            if ((plan->ignored.file && hostPathsEqual(from, plan->ignored.file)) ||
                ignoresPath(&plan->ignored, relative)) {
                free(names[i]);
                continue;
            }
            HostPathKind kind;
            ok = hostPathKind(from, &kind, plan->message, plan->messageSize);
            if (ok && kind == HOST_PATH_DIRECTORY) {
                ok = collectTree(plan, from, to);
            } else if (ok &&
                       (kind == HOST_PATH_FILE || kind == HOST_PATH_SYMLINK)) {
                ok = addOperation(plan, from, to, false);
            } else if (ok) {
                snprintf(
                    plan->message, plan->messageSize,
                    "symlink source %s is not a file, directory, or symlink",
                    from);
                ok = false;
            }
        }
        free(names[i]);
    }
    free(names);
    return ok;
}

/* Tree mode may create the missing ancestors of the target root. */
static bool collectParents(LinkPlan *plan, const char *target)
{
    char *parent = parentPath(target);
    HostPathKind kind;
    bool ok = hostPathKind(parent, &kind, plan->message, plan->messageSize);
    if (ok && kind == HOST_PATH_MISSING) {
        ok = collectParents(plan, parent);
        if (ok) {
            const char *stored =
                arenaStrndup(&plan->arena, parent, strlen(parent));
            ok = addOperation(plan, NULL, stored, true);
        }
    }
    free(parent);
    return ok;
}

bool deploySymlink(const char *source, const char *target, bool tree,
                   const char *ignoreFile, bool planOnly, char *message,
                   size_t messageSize)
{
    LinkPlan plan = {0};
    arenaInit(&plan.arena);
    plan.message = message;
    plan.messageSize = messageSize;
    char *from = hostAbsolutePath(source, message, messageSize);
    char *to = hostAbsolutePath(target, message, messageSize);
    bool ok = from && to;
    if (ok && ignoreFile && !tree) {
        snprintf(message, messageSize, "an ignore file requires tree mode");
        ok = false;
    }
    HostPathKind kind = HOST_PATH_MISSING;
    if (ok) {
        ok = hostPathKind(from, &kind, message, messageSize);
    }
    if (ok && kind == HOST_PATH_MISSING && planOnly) {
        printf("  link %s%s -> %s (source currently missing)\n",
               tree ? "tree " : "", to, from);
        free(from);
        free(to);
        arenaFree(&plan.arena);
        return true;
    }
    if (ok && (kind == HOST_PATH_MISSING || kind == HOST_PATH_OTHER ||
               (tree && kind != HOST_PATH_DIRECTORY))) {
        snprintf(message, messageSize, "symlink source %s must be %s", from,
                 tree ? "a real directory in tree mode"
                      : "a file, directory, or symlink");
        ok = false;
    }
    if (ok && hostPathsEqual(from, to)) {
        snprintf(message, messageSize,
                 "symlink source and target are the same path: %s", from);
        ok = false;
    }
    if (ok) {
        ok = inspectParents(&plan, to, tree || planOnly);
    }
    if (ok && tree) {
        char *canonical = hostCanonicalPath(from, message, messageSize);
        ok = canonical != NULL;
        if (ok && pathWithin(canonical, to)) {
            snprintf(message, messageSize,
                     "symlink source and target trees overlap: %s and %s", from,
                     to);
            ok = false;
        }
        if (ok) {
            plan.sourceBase = from;
            plan.sourceRoot = arenaStrdup(&plan.arena, canonical);
        }
        free(canonical);
        if (ok && ignoreFile) {
            ok = loadIgnoreList(&plan.arena, ignoreFile, &plan.ignored,
                                 message, messageSize);
        }
        if (ok) {
            ok = collectParents(&plan, to) && collectTree(&plan, from, to);
        }
    } else if (ok) {
        ok = addOperation(&plan, from, to, false);
    }
    /* All conflicts are checked before the first filesystem write. */
    for (size_t i = 0; i < plan.count && ok; i++) {
        const LinkOperation *operation = &plan.items[i];
        printf("  %s%s %s", planOnly ? "" : "-> ",
               operation->directory ? "directory" : "link",
               operation->target);
        if (!operation->directory) {
            printf(" -> %s", operation->source);
        }
        printf(" (%s)\n", operation->unchanged ? "unchanged"
                          : planOnly           ? "would create"
                                               : "create");
        if (!planOnly && !operation->unchanged) {
            /* Recheck ancestors immediately before writing as well. */
            ok = inspectParents(&plan, operation->target, false);
            if (ok) {
                ok = operation->directory
                         ? hostCreateDirectory(operation->target, message,
                                               messageSize)
                         : hostCreateSymlink(operation->source,
                                             operation->target, message,
                                             messageSize);
            }
        }
    }
    free(from);
    free(to);
    arenaFree(&plan.arena);
    return ok;
}
