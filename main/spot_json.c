// main/spot_json.c - see spot_json.h.
#include "spot_json.h"

#include <string.h>

enum {
    ST_VALUE = 0,   // expecting a value
    ST_OBJ_FIRST,   // after '{': a key or '}'
    ST_OBJ_KEY,     // after ',' in an object: a key
    ST_COLON,       // after a key
    ST_AFTER,       // after a value: ',' or a closing bracket
    ST_ARR_FIRST,   // after '[': a value or ']'
    ST_STRING,
    ST_ESC,         // after a backslash
    ST_UHEX,        // inside \uXXXX
    ST_LITERAL,     // number, true, false, null
    ST_DONE,
};

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void fail(spot_json_t *s) {
    s->error = true;
}

void spot_json_init(spot_json_t *s, spot_json_cb_t cb, void *ctx) {
    memset(s, 0, sizeof(*s));
    s->cb = cb;
    s->ctx = ctx;
    s->state = ST_VALUE;
}

// ---------------------------------------------------------------------------
// Path
// ---------------------------------------------------------------------------

// Replaces the last path segment (below the innermost container) with `seg`.
static void set_segment(spot_json_t *s, const char *seg, size_t seg_len) {
    uint16_t base = s->depth ? s->stack[s->depth - 1].path_len : 0;
    size_t sep = base ? 1 : 0;
    if (base + sep + seg_len >= SPOT_JSON_PATH_MAX) {
        // Does not fit: use "?" so the path can never equal a wanted one.
        seg = "?";
        seg_len = 1;
    }
    if (base + sep + seg_len >= SPOT_JSON_PATH_MAX) {
        s->path[SPOT_JSON_PATH_MAX - 2] = '?';
        s->path[SPOT_JSON_PATH_MAX - 1] = '\0';
        s->path_len = SPOT_JSON_PATH_MAX - 1;
        return;
    }
    size_t len = base;
    if (sep) s->path[len++] = '.';
    memcpy(s->path + len, seg, seg_len);
    len += seg_len;
    s->path[len] = '\0';
    s->path_len = (uint16_t)len;
}

static void set_index_segment(spot_json_t *s) {
    char num[8];
    unsigned v = s->stack[s->depth - 1].index;
    if (s->stack[s->depth - 1].index < UINT16_MAX) s->stack[s->depth - 1].index++;
    size_t n = 0;
    char rev[8];
    do {
        rev[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < sizeof(rev));
    for (size_t i = 0; i < n; i++) num[i] = rev[n - 1 - i];
    set_segment(s, num, n);
}

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------

static void value_reset(spot_json_t *s) {
    s->value_len = 0;
    s->value[0] = '\0';
    s->value_truncated = false;
}

static void value_put(spot_json_t *s, const char *bytes, size_t n) {
    if (s->value_truncated) return;
    if (s->value_len + n >= SPOT_JSON_VALUE_MAX) {
        s->value_truncated = true;
        return;
    }
    memcpy(s->value + s->value_len, bytes, n);
    s->value_len = (uint16_t)(s->value_len + n);
}

static void key_put(spot_json_t *s, char c) {
    if (s->key_len + 1 >= SPOT_JSON_KEY_MAX) {
        s->key_overflow = true;
        return;
    }
    s->key[s->key_len++] = c;
}

static void string_put(spot_json_t *s, const char *bytes, size_t n) {
    if (s->in_key) {
        for (size_t i = 0; i < n; i++) key_put(s, bytes[i]);
    } else {
        value_put(s, bytes, n);
    }
}

static size_t utf8_encode(uint32_t cp, char buf[4]) {
    if (cp < 0x80) {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static void put_codepoint(spot_json_t *s, uint32_t cp) {
    char buf[4];
    string_put(s, buf, utf8_encode(cp, buf));
}

// An unpaired high surrogate becomes '?'.
static void flush_surrogate(spot_json_t *s) {
    if (s->high_surrogate) {
        s->high_surrogate = 0;
        put_codepoint(s, '?');
    }
}

// Drops a multi-byte sequence cut off by truncation at the end of the value.
static void trim_partial_utf8(spot_json_t *s) {
    size_t len = s->value_len;
    size_t i = len;
    while (i > 0 && ((unsigned char)s->value[i - 1] & 0xC0) == 0x80) i--;
    if (i == 0) {
        s->value_len = 0;
        return;
    }
    unsigned char lead = (unsigned char)s->value[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (len - (i - 1) < need) s->value_len = (uint16_t)(i - 1);
}

static void emit(spot_json_t *s, spot_json_type_t type) {
    s->value[s->value_len] = '\0';
    if (s->cb) s->cb(s->ctx, s->path, type, s->value, s->value_truncated);
}

static void value_complete(spot_json_t *s) {
    s->state = s->depth == 0 ? ST_DONE : ST_AFTER;
    if (s->depth == 0) s->done = true;
}

static bool is_number(const char *p) {
    if (*p == '-') p++;
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (*p >= '0' && *p <= '9') p++;
    } else {
        return false;
    }
    if (*p == '.') {
        p++;
        if (!(*p >= '0' && *p <= '9')) return false;
        while (*p >= '0' && *p <= '9') p++;
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (!(*p >= '0' && *p <= '9')) return false;
        while (*p >= '0' && *p <= '9') p++;
    }
    return *p == '\0';
}

static void finish_literal(spot_json_t *s) {
    s->value[s->value_len] = '\0';
    spot_json_type_t type;
    if (s->value_truncated) {
        fail(s);  // no valid literal is this long
        return;
    }
    if (strcmp(s->value, "true") == 0) {
        type = SPOT_JSON_TRUE;
    } else if (strcmp(s->value, "false") == 0) {
        type = SPOT_JSON_FALSE;
    } else if (strcmp(s->value, "null") == 0) {
        type = SPOT_JSON_NULL;
    } else if (is_number(s->value)) {
        type = SPOT_JSON_NUMBER;
    } else {
        fail(s);
        return;
    }
    emit(s, type);
    value_complete(s);
}

static bool is_literal_char(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           c == '-' || c == '+' || c == '.';
}

// ---------------------------------------------------------------------------
// Containers
// ---------------------------------------------------------------------------

static void push(spot_json_t *s, bool is_array) {
    if (s->depth == SPOT_JSON_DEPTH_MAX) {
        fail(s);
        return;
    }
    s->stack[s->depth].is_array = is_array;
    s->stack[s->depth].path_len = s->path_len;
    s->stack[s->depth].index = 0;
    s->depth++;
    s->state = is_array ? ST_ARR_FIRST : ST_OBJ_FIRST;
}

static void pop(spot_json_t *s) {
    s->depth--;
    s->path_len = s->stack[s->depth].path_len;
    s->path[s->path_len] = '\0';
    value_complete(s);
}

static void start_string(spot_json_t *s, bool key) {
    s->in_key = key;
    s->high_surrogate = 0;
    if (key) {
        s->key_len = 0;
        s->key_overflow = false;
    } else {
        value_reset(s);
    }
    s->state = ST_STRING;
}

static void end_string(spot_json_t *s) {
    flush_surrogate(s);
    if (s->in_key) {
        s->key[s->key_len] = '\0';
        s->state = ST_COLON;
        return;
    }
    if (s->value_truncated) trim_partial_utf8(s);
    emit(s, SPOT_JSON_STRING);
    value_complete(s);
}

static void start_value(spot_json_t *s, char c) {
    if (s->depth > 0 && s->stack[s->depth - 1].is_array) set_index_segment(s);
    if (c == '{') {
        push(s, false);
    } else if (c == '[') {
        push(s, true);
    } else if (c == '"') {
        start_string(s, false);
    } else if (is_literal_char(c)) {
        value_reset(s);
        value_put(s, &c, 1);
        s->state = ST_LITERAL;
    } else {
        fail(s);
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void finish_escape_cp(spot_json_t *s, uint32_t cp) {
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        flush_surrogate(s);
        s->high_surrogate = cp;
        return;
    }
    if (cp >= 0xDC00 && cp <= 0xDFFF) {
        if (s->high_surrogate) {
            cp = 0x10000 + ((s->high_surrogate - 0xD800) << 10) + (cp - 0xDC00);
            s->high_surrogate = 0;
        } else {
            cp = '?';
        }
    } else {
        flush_surrogate(s);
    }
    put_codepoint(s, cp);
}

// ---------------------------------------------------------------------------
// Feed
// ---------------------------------------------------------------------------

static void step(spot_json_t *s, char c) {
    switch (s->state) {
    case ST_VALUE:
        if (!is_ws(c)) start_value(s, c);
        break;
    case ST_ARR_FIRST:
        if (is_ws(c)) break;
        if (c == ']') {
            pop(s);
        } else {
            start_value(s, c);
        }
        break;
    case ST_OBJ_FIRST:
        if (is_ws(c)) break;
        if (c == '}') {
            pop(s);
        } else if (c == '"') {
            start_string(s, true);
        } else {
            fail(s);
        }
        break;
    case ST_OBJ_KEY:
        if (is_ws(c)) break;
        if (c == '"') {
            start_string(s, true);
        } else {
            fail(s);
        }
        break;
    case ST_COLON:
        if (is_ws(c)) break;
        if (c != ':') {
            fail(s);
            break;
        }
        if (s->key_overflow) {
            set_segment(s, "?", 1);
        } else {
            set_segment(s, s->key, s->key_len);
        }
        s->state = ST_VALUE;
        break;
    case ST_AFTER: {
        if (is_ws(c)) break;
        bool is_array = s->stack[s->depth - 1].is_array;
        if (c == ',') {
            s->state = is_array ? ST_VALUE : ST_OBJ_KEY;
        } else if ((c == ']' && is_array) || (c == '}' && !is_array)) {
            pop(s);
        } else {
            fail(s);
        }
        break;
    }
    case ST_STRING:
        if (c == '"') {
            end_string(s);
        } else if (c == '\\') {
            s->state = ST_ESC;
        } else if ((unsigned char)c < 0x20) {
            fail(s);
        } else {
            flush_surrogate(s);
            string_put(s, &c, 1);
        }
        break;
    case ST_ESC: {
        char out = 0;
        switch (c) {
        case '"': out = '"'; break;
        case '\\': out = '\\'; break;
        case '/': out = '/'; break;
        case 'b': out = '\b'; break;
        case 'f': out = '\f'; break;
        case 'n': out = '\n'; break;
        case 'r': out = '\r'; break;
        case 't': out = '\t'; break;
        case 'u':
            s->esc_cp = 0;
            s->esc_hex_left = 4;
            s->state = ST_UHEX;
            return;
        default:
            fail(s);
            return;
        }
        flush_surrogate(s);
        string_put(s, &out, 1);
        s->state = ST_STRING;
        break;
    }
    case ST_UHEX: {
        int h = hex_value(c);
        if (h < 0) {
            fail(s);
            break;
        }
        s->esc_cp = (s->esc_cp << 4) | (uint32_t)h;
        if (--s->esc_hex_left == 0) {
            finish_escape_cp(s, s->esc_cp);
            s->state = ST_STRING;
        }
        break;
    }
    case ST_LITERAL:
        if (is_literal_char(c)) {
            value_put(s, &c, 1);
            break;
        }
        finish_literal(s);
        if (!s->error) step(s, c);  // the terminator belongs to the next state
        break;
    case ST_DONE:
        if (!is_ws(c)) fail(s);
        break;
    default:
        fail(s);
        break;
    }
}

bool spot_json_feed(spot_json_t *s, const char *data, size_t len) {
    for (size_t i = 0; i < len && !s->error; i++) step(s, data[i]);
    return !s->error;
}

bool spot_json_finish(spot_json_t *s) {
    if (s->error) return false;
    if (s->state == ST_LITERAL && s->depth == 0) finish_literal(s);
    return !s->error && s->done;
}
