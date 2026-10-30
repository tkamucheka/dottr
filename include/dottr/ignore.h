#ifndef DOTTR_IGNORE_H
#define DOTTR_IGNORE_H

#include <stdbool.h>
#include <stddef.h>

#include "dottr/util.h"

typedef struct IgnoreRule IgnoreRule;

typedef struct {
    IgnoreRule *rules;
    size_t count;
    const char *file;
} IgnoreList;

bool loadIgnoreList(Arena *arena, const char *file, IgnoreList *list,
                    char *message, size_t messageSize);
bool ignoresPath(const IgnoreList *list, const char *relative);

#endif
