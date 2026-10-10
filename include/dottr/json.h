#ifndef DOTTR_JSON_H
#define DOTTR_JSON_H

#include <stdbool.h>
#include <stddef.h>

#include "dottr/util.h"

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT,
} JsonKind;

typedef struct JsonValue JsonValue;

typedef struct {
    const char *key;
    size_t keyLength;
    JsonValue *value;
} JsonMember;

struct JsonValue {
    JsonKind kind;
    bool boolean;
    double number;
    /* JSON_STRING: decoded UTF-8, NUL-terminated; may contain NUL bytes. */
    const char *string;
    size_t length;
    /* JSON_ARRAY: items; JSON_OBJECT: members. */
    JsonValue **items;
    JsonMember *members;
    size_t count;
};

/*
 * Parses one JSON value, surrounded by optional whitespace, into a tree
 * allocated in arena. Returns NULL if the text is not valid JSON or nests
 * more deeply than 256 levels.
 */
JsonValue *jsonParse(Arena *arena, const char *text, size_t length);

/* Member of an object, or NULL if value is not an object or has no member. */
const JsonValue *jsonGet(const JsonValue *object, const char *key);

/* String of a string value, or NULL. */
const char *jsonString(const JsonValue *value);

/* Number of a number value that is an integer in int range, or fallback. */
int jsonInt(const JsonValue *value, int fallback);

bool jsonBool(const JsonValue *value, bool fallback);

/*
 * Appends text as a JSON string literal. Invalid UTF-8 is replaced by
 * U+FFFD, so the output is always valid JSON.
 */
void jsonWriteString(StrBuf *out, const char *text, size_t length);

/* Appends a value in compact JSON. */
void jsonWriteValue(StrBuf *out, const JsonValue *value);

#endif
