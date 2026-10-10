#include "dottr/json.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 256

/* True if number is an integer of magnitude below 2^53. (No libm needed.) */
static bool isSmallInteger(double number)
{
    return number > -9007199254740992.0 && number < 9007199254740992.0 &&
           (double)(long long)number == number;
}

typedef struct {
    Arena *arena;
    const char *text;
    size_t length;
    size_t pos;
    int depth;
} JsonParser;

static JsonValue *parseValue(JsonParser *p);

static void skipWhitespace(JsonParser *p)
{
    while (p->pos < p->length) {
        char c = p->text[p->pos];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        p->pos++;
    }
}

static bool consumeLiteral(JsonParser *p, const char *literal)
{
    size_t length = strlen(literal);
    if (p->length - p->pos < length ||
        memcmp(&p->text[p->pos], literal, length) != 0) {
        return false;
    }
    p->pos += length;
    return true;
}

static JsonValue *newValue(JsonParser *p, JsonKind kind)
{
    JsonValue *value = arenaAlloc(p->arena, sizeof *value);
    value->kind = kind;
    return value;
}

static int hexDigit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool parseHex4(JsonParser *p, unsigned *code)
{
    if (p->length - p->pos < 4) {
        return false;
    }
    unsigned value = 0;
    for (int i = 0; i < 4; i++) {
        int digit = hexDigit(p->text[p->pos + (size_t)i]);
        if (digit < 0) {
            return false;
        }
        value = value * 16 + (unsigned)digit;
    }
    p->pos += 4;
    *code = value;
    return true;
}

static void appendUtf8(StrBuf *buf, unsigned code)
{
    if (code < 0x80) {
        strBufAppendChar(buf, (char)code);
    } else if (code < 0x800) {
        strBufAppendChar(buf, (char)(0xC0 | (code >> 6)));
        strBufAppendChar(buf, (char)(0x80 | (code & 0x3F)));
    } else if (code < 0x10000) {
        strBufAppendChar(buf, (char)(0xE0 | (code >> 12)));
        strBufAppendChar(buf, (char)(0x80 | ((code >> 6) & 0x3F)));
        strBufAppendChar(buf, (char)(0x80 | (code & 0x3F)));
    } else {
        strBufAppendChar(buf, (char)(0xF0 | (code >> 18)));
        strBufAppendChar(buf, (char)(0x80 | ((code >> 12) & 0x3F)));
        strBufAppendChar(buf, (char)(0x80 | ((code >> 6) & 0x3F)));
        strBufAppendChar(buf, (char)(0x80 | (code & 0x3F)));
    }
}

/* Parses a string literal whose opening quote is at p->pos. */
static bool parseStringText(JsonParser *p, const char **out, size_t *outLength)
{
    p->pos++;
    StrBuf buf;
    strBufInit(&buf);
    for (;;) {
        if (p->pos >= p->length) {
            strBufFree(&buf);
            return false;
        }
        unsigned char c = (unsigned char)p->text[p->pos++];
        if (c == '"') {
            break;
        }
        if (c < 0x20) {
            strBufFree(&buf);
            return false;
        }
        if (c != '\\') {
            strBufAppendChar(&buf, (char)c);
            continue;
        }
        if (p->pos >= p->length) {
            strBufFree(&buf);
            return false;
        }
        char escape = p->text[p->pos++];
        unsigned code;
        switch (escape) {
        case '"': strBufAppendChar(&buf, '"'); break;
        case '\\': strBufAppendChar(&buf, '\\'); break;
        case '/': strBufAppendChar(&buf, '/'); break;
        case 'b': strBufAppendChar(&buf, '\b'); break;
        case 'f': strBufAppendChar(&buf, '\f'); break;
        case 'n': strBufAppendChar(&buf, '\n'); break;
        case 'r': strBufAppendChar(&buf, '\r'); break;
        case 't': strBufAppendChar(&buf, '\t'); break;
        case 'u':
            if (!parseHex4(p, &code)) {
                strBufFree(&buf);
                return false;
            }
            if (code >= 0xD800 && code <= 0xDBFF) {
                unsigned low;
                if (p->length - p->pos >= 6 && p->text[p->pos] == '\\' &&
                    p->text[p->pos + 1] == 'u') {
                    size_t saved = p->pos;
                    p->pos += 2;
                    if (parseHex4(p, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                        code = 0x10000 + ((code - 0xD800) << 10) +
                               (low - 0xDC00);
                    } else {
                        p->pos = saved;
                        code = 0xFFFD;
                    }
                } else {
                    code = 0xFFFD;
                }
            } else if (code >= 0xDC00 && code <= 0xDFFF) {
                code = 0xFFFD;
            }
            appendUtf8(&buf, code);
            break;
        default:
            strBufFree(&buf);
            return false;
        }
    }
    *out = arenaStrndup(p->arena, buf.data ? buf.data : "", buf.length);
    *outLength = buf.length;
    strBufFree(&buf);
    return true;
}

static JsonValue *parseNumber(JsonParser *p)
{
    size_t start = p->pos;
    if (p->pos < p->length && p->text[p->pos] == '-') {
        p->pos++;
    }
    if (p->pos >= p->length) {
        return NULL;
    }
    if (p->text[p->pos] == '0') {
        p->pos++;
    } else if (p->text[p->pos] >= '1' && p->text[p->pos] <= '9') {
        while (p->pos < p->length && p->text[p->pos] >= '0' &&
               p->text[p->pos] <= '9') {
            p->pos++;
        }
    } else {
        return NULL;
    }
    if (p->pos < p->length && p->text[p->pos] == '.') {
        p->pos++;
        size_t digits = p->pos;
        while (p->pos < p->length && p->text[p->pos] >= '0' &&
               p->text[p->pos] <= '9') {
            p->pos++;
        }
        if (p->pos == digits) {
            return NULL;
        }
    }
    if (p->pos < p->length &&
        (p->text[p->pos] == 'e' || p->text[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->length &&
            (p->text[p->pos] == '+' || p->text[p->pos] == '-')) {
            p->pos++;
        }
        size_t digits = p->pos;
        while (p->pos < p->length && p->text[p->pos] >= '0' &&
               p->text[p->pos] <= '9') {
            p->pos++;
        }
        if (p->pos == digits) {
            return NULL;
        }
    }
    char *copy = arenaStrndup(p->arena, &p->text[start], p->pos - start);
    JsonValue *value = newValue(p, JSON_NUMBER);
    value->number = strtod(copy, NULL);
    return value;
}

static JsonValue *parseArray(JsonParser *p)
{
    p->pos++;
    JsonValue *value = newValue(p, JSON_ARRAY);
    size_t capacity = 0;
    skipWhitespace(p);
    if (p->pos < p->length && p->text[p->pos] == ']') {
        p->pos++;
        return value;
    }
    for (;;) {
        JsonValue *item = parseValue(p);
        if (!item) {
            return NULL;
        }
        value->items = arenaGrow(p->arena, value->items, value->count,
                                 &capacity, sizeof *value->items);
        value->items[value->count++] = item;
        skipWhitespace(p);
        if (p->pos >= p->length) {
            return NULL;
        }
        char c = p->text[p->pos++];
        if (c == ']') {
            return value;
        }
        if (c != ',') {
            return NULL;
        }
    }
}

static JsonValue *parseObject(JsonParser *p)
{
    p->pos++;
    JsonValue *value = newValue(p, JSON_OBJECT);
    size_t capacity = 0;
    skipWhitespace(p);
    if (p->pos < p->length && p->text[p->pos] == '}') {
        p->pos++;
        return value;
    }
    for (;;) {
        skipWhitespace(p);
        if (p->pos >= p->length || p->text[p->pos] != '"') {
            return NULL;
        }
        JsonMember member;
        if (!parseStringText(p, &member.key, &member.keyLength)) {
            return NULL;
        }
        skipWhitespace(p);
        if (p->pos >= p->length || p->text[p->pos] != ':') {
            return NULL;
        }
        p->pos++;
        member.value = parseValue(p);
        if (!member.value) {
            return NULL;
        }
        value->members = arenaGrow(p->arena, value->members, value->count,
                                   &capacity, sizeof *value->members);
        value->members[value->count++] = member;
        skipWhitespace(p);
        if (p->pos >= p->length) {
            return NULL;
        }
        char c = p->text[p->pos++];
        if (c == '}') {
            return value;
        }
        if (c != ',') {
            return NULL;
        }
    }
}

static JsonValue *parseValue(JsonParser *p)
{
    skipWhitespace(p);
    if (p->pos >= p->length) {
        return NULL;
    }
    if (++p->depth > MAX_DEPTH) {
        return NULL;
    }

    JsonValue *value = NULL;
    char c = p->text[p->pos];
    if (c == '{') {
        value = parseObject(p);
    } else if (c == '[') {
        value = parseArray(p);
    } else if (c == '"') {
        value = newValue(p, JSON_STRING);
        if (!parseStringText(p, &value->string, &value->length)) {
            value = NULL;
        }
    } else if (consumeLiteral(p, "true")) {
        value = newValue(p, JSON_BOOL);
        value->boolean = true;
    } else if (consumeLiteral(p, "false")) {
        value = newValue(p, JSON_BOOL);
    } else if (consumeLiteral(p, "null")) {
        value = newValue(p, JSON_NULL);
    } else {
        value = parseNumber(p);
    }

    p->depth--;
    return value;
}

JsonValue *jsonParse(Arena *arena, const char *text, size_t length)
{
    JsonParser p = {arena, text, length, 0, 0};
    JsonValue *value = parseValue(&p);
    if (!value) {
        return NULL;
    }
    skipWhitespace(&p);
    return p.pos == length ? value : NULL;
}

const JsonValue *jsonGet(const JsonValue *object, const char *key)
{
    if (!object || object->kind != JSON_OBJECT) {
        return NULL;
    }
    size_t keyLength = strlen(key);
    for (size_t i = 0; i < object->count; i++) {
        const JsonMember *member = &object->members[i];
        if (member->keyLength == keyLength &&
            memcmp(member->key, key, keyLength) == 0) {
            return member->value;
        }
    }
    return NULL;
}

const char *jsonString(const JsonValue *value)
{
    return value && value->kind == JSON_STRING ? value->string : NULL;
}

int jsonInt(const JsonValue *value, int fallback)
{
    if (!value || value->kind != JSON_NUMBER ||
        !isSmallInteger(value->number) || value->number < INT_MIN ||
        value->number > INT_MAX) {
        return fallback;
    }
    return (int)value->number;
}

bool jsonBool(const JsonValue *value, bool fallback)
{
    return value && value->kind == JSON_BOOL ? value->boolean : fallback;
}

/* Length of the valid UTF-8 sequence at text, or 0 if it is invalid. */
static size_t utf8SequenceLength(const unsigned char *text, size_t available)
{
    unsigned char c = text[0];
    size_t length;
    unsigned code;
    if (c < 0x80) {
        return 1;
    } else if ((c & 0xE0) == 0xC0) {
        length = 2;
        code = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        length = 3;
        code = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        length = 4;
        code = c & 0x07;
    } else {
        return 0;
    }
    if (available < length) {
        return 0;
    }
    for (size_t i = 1; i < length; i++) {
        if ((text[i] & 0xC0) != 0x80) {
            return 0;
        }
        code = (code << 6) | (text[i] & 0x3F);
    }
    static const unsigned minimum[] = {0, 0, 0x80, 0x800, 0x10000};
    if (code < minimum[length] || code > 0x10FFFF ||
        (code >= 0xD800 && code <= 0xDFFF)) {
        return 0;
    }
    return length;
}

void jsonWriteString(StrBuf *out, const char *text, size_t length)
{
    const unsigned char *bytes = (const unsigned char *)text;
    strBufAppendChar(out, '"');
    size_t i = 0;
    while (i < length) {
        unsigned char c = bytes[i];
        if (c == '"' || c == '\\') {
            strBufAppendChar(out, '\\');
            strBufAppendChar(out, (char)c);
            i++;
        } else if (c == '\n') {
            strBufAppendStr(out, "\\n");
            i++;
        } else if (c == '\r') {
            strBufAppendStr(out, "\\r");
            i++;
        } else if (c == '\t') {
            strBufAppendStr(out, "\\t");
            i++;
        } else if (c < 0x20 || c == 0x7F) {
            char escape[8];
            snprintf(escape, sizeof escape, "\\u%04x", c);
            strBufAppendStr(out, escape);
            i++;
        } else if (c < 0x80) {
            strBufAppendChar(out, (char)c);
            i++;
        } else {
            size_t sequence = utf8SequenceLength(&bytes[i], length - i);
            if (sequence == 0) {
                strBufAppendStr(out, "\xEF\xBF\xBD");
                i++;
            } else {
                strBufAppend(out, &text[i], sequence);
                i += sequence;
            }
        }
    }
    strBufAppendChar(out, '"');
}

void jsonWriteValue(StrBuf *out, const JsonValue *value)
{
    char number[64];
    switch (value->kind) {
    case JSON_NULL:
        strBufAppendStr(out, "null");
        break;
    case JSON_BOOL:
        strBufAppendStr(out, value->boolean ? "true" : "false");
        break;
    case JSON_NUMBER:
        if (isSmallInteger(value->number)) {
            snprintf(number, sizeof number, "%.0f", value->number);
        } else {
            snprintf(number, sizeof number, "%.17g", value->number);
        }
        strBufAppendStr(out, number);
        break;
    case JSON_STRING:
        jsonWriteString(out, value->string, value->length);
        break;
    case JSON_ARRAY:
        strBufAppendChar(out, '[');
        for (size_t i = 0; i < value->count; i++) {
            if (i > 0) {
                strBufAppendChar(out, ',');
            }
            jsonWriteValue(out, value->items[i]);
        }
        strBufAppendChar(out, ']');
        break;
    case JSON_OBJECT:
        strBufAppendChar(out, '{');
        for (size_t i = 0; i < value->count; i++) {
            if (i > 0) {
                strBufAppendChar(out, ',');
            }
            jsonWriteString(out, value->members[i].key,
                            value->members[i].keyLength);
            strBufAppendChar(out, ':');
            jsonWriteValue(out, value->members[i].value);
        }
        strBufAppendChar(out, '}');
        break;
    }
}
