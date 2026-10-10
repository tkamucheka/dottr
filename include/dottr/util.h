#ifndef DOTTR_UTIL_H
#define DOTTR_UTIL_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Region allocator. Every allocation lives until arenaFree. Memory returned
 * by arenaAlloc is zero-initialized and aligned for any object type.
 */
typedef struct ArenaBlock ArenaBlock;

typedef struct {
    ArenaBlock *head;
} Arena;

void arenaInit(Arena *arena);
void arenaFree(Arena *arena);
void *arenaAlloc(Arena *arena, size_t size);
char *arenaStrndup(Arena *arena, const char *text, size_t length);
char *arenaStrdup(Arena *arena, const char *text);

/*
 * Growth helper for arena-backed arrays. Returns items unchanged while
 * count < *capacity; otherwise returns a larger copy and updates *capacity.
 * Typical use:
 *
 *     items = arenaGrow(arena, items, count, &capacity, sizeof *items);
 *     items[count++] = value;
 */
void *arenaGrow(Arena *arena, void *items, size_t count, size_t *capacity,
                size_t itemSize);

/* Growable, always NUL-terminated byte string on the heap. */
typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} StrBuf;

void strBufInit(StrBuf *buf);
void strBufFree(StrBuf *buf);
void strBufClear(StrBuf *buf);
void strBufAppend(StrBuf *buf, const char *text, size_t length);
void strBufAppendStr(StrBuf *buf, const char *text);
void strBufAppendChar(StrBuf *buf, char c);

/*
 * Reads a whole file into a NUL-terminated heap buffer. On failure returns
 * false and leaves errno set.
 */
bool readFile(const char *path, char **data, size_t *length);

void *xmalloc(size_t size);
void *xrealloc(void *pointer, size_t size);

#endif
