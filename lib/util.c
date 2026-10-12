#include "dottr/util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARENA_BLOCK_SIZE ((size_t)64 * 1024)

/*
 * A type with the strictest alignment of the basic types, as max_align_t,
 * which MSVC provides only in C++.
 */
typedef union {
    long double floating;
    long long integer;
    void *object;
    void (*function)(void);
} MaxAlign;

struct ArenaBlock {
    ArenaBlock *next;
    size_t size;
    size_t used;
    MaxAlign data[];
};

static void fatalOutOfMemory(void)
{
    fputs("dottr: fatal: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t size)
{
    void *pointer = malloc(size ? size : 1);
    if (!pointer) {
        fatalOutOfMemory();
    }
    return pointer;
}

void *xrealloc(void *pointer, size_t size)
{
    void *result = realloc(pointer, size ? size : 1);
    if (!result) {
        fatalOutOfMemory();
    }
    return result;
}

void arenaInit(Arena *arena)
{
    arena->head = NULL;
}

void arenaFree(Arena *arena)
{
    ArenaBlock *block = arena->head;
    while (block) {
        ArenaBlock *next = block->next;
        free(block);
        block = next;
    }
    arena->head = NULL;
}

void *arenaAlloc(Arena *arena, size_t size)
{
    const size_t align = _Alignof(MaxAlign);
    size = (size + align - 1) / align * align;
    if (size == 0) {
        size = align;
    }

    ArenaBlock *block = arena->head;
    if (!block || block->size - block->used < size) {
        size_t blockSize = size > ARENA_BLOCK_SIZE ? size : ARENA_BLOCK_SIZE;
        block = xmalloc(sizeof *block + blockSize);
        block->size = blockSize;
        block->used = 0;
        block->next = arena->head;
        arena->head = block;
    }

    unsigned char *memory = (unsigned char *)block->data + block->used;
    block->used += size;
    memset(memory, 0, size);
    return memory;
}

char *arenaStrndup(Arena *arena, const char *text, size_t length)
{
    char *copy = arenaAlloc(arena, length + 1);
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

char *arenaStrdup(Arena *arena, const char *text)
{
    return arenaStrndup(arena, text, strlen(text));
}

void *arenaGrow(Arena *arena, void *items, size_t count, size_t *capacity,
                size_t itemSize)
{
    if (count < *capacity) {
        return items;
    }
    size_t newCapacity = *capacity ? *capacity * 2 : 8;
    void *grown = arenaAlloc(arena, newCapacity * itemSize);
    if (count > 0) {
        memcpy(grown, items, count * itemSize);
    }
    *capacity = newCapacity;
    return grown;
}

void strBufInit(StrBuf *buf)
{
    buf->capacity = 64;
    buf->length = 0;
    buf->data = xmalloc(buf->capacity);
    buf->data[0] = '\0';
}

void strBufFree(StrBuf *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->length = 0;
    buf->capacity = 0;
}

void strBufClear(StrBuf *buf)
{
    buf->length = 0;
    buf->data[0] = '\0';
}

void strBufAppend(StrBuf *buf, const char *text, size_t length)
{
    if (buf->length + length + 1 > buf->capacity) {
        size_t capacity = buf->capacity;
        while (buf->length + length + 1 > capacity) {
            capacity *= 2;
        }
        buf->data = xrealloc(buf->data, capacity);
        buf->capacity = capacity;
    }
    memcpy(buf->data + buf->length, text, length);
    buf->length += length;
    buf->data[buf->length] = '\0';
}

void strBufAppendStr(StrBuf *buf, const char *text)
{
    strBufAppend(buf, text, strlen(text));
}

void strBufAppendChar(StrBuf *buf, char c)
{
    strBufAppend(buf, &c, 1);
}

bool readFile(const char *path, char **data, size_t *length)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    bool ok = readStream(file, data, length);
    int savedErrno = errno;
    fclose(file);
    errno = savedErrno;
    return ok;
}

bool readStream(FILE *file, char **data, size_t *length)
{
    StrBuf buf;
    strBufInit(&buf);
    char chunk[8192];
    size_t count;
    while ((count = fread(chunk, 1, sizeof chunk, file)) > 0) {
        strBufAppend(&buf, chunk, count);
    }

    if (ferror(file)) {
        int savedErrno = errno;
        strBufFree(&buf);
        errno = savedErrno ? savedErrno : EIO;
        return false;
    }

    *data = buf.data;
    *length = buf.length;
    return true;
}
