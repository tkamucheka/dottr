#ifndef DOTTR_SYMLINK_H
#define DOTTR_SYMLINK_H

#include <stdbool.h>
#include <stddef.h>

/* Plans or applies a link statement; planning performs no filesystem writes. */
bool deploySymlink(const char *source, const char *target, bool tree,
                   bool planOnly, char *message, size_t messageSize);

#endif
