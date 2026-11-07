// main/home_json.c - see home_json.h.
#include "home_json.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 16

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

// p points at the opening quote. Returns the position after the closing quote.
static const char *skip_string(const char *p) {
    if (*p != '"') return NULL;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            if (!*p) return NULL;
        }
        p++;
    }
    return *p == '"' ? p + 1 : NULL;
}

static const char *skip_value(const char *p, int depth);

static const char *skip_container(const char *p, int depth, char close, bool object) {
    p = skip_ws(p + 1);
    if (*p == close) return p + 1;
    for (;;) {
        if (object) {
            p = skip_string(p);
            if (!p) return NULL;
            p = skip_ws(p);
            if (*p != ':') return NULL;
            p = skip_ws(p + 1);
        }
        p = skip_value(p, depth + 1);
        if (!p) return NULL;
        p = skip_ws(p);
        if (*p == ',') {
            p = skip_ws(p + 1);
            continue;
        }
        return *p == close ? p + 1 : NULL;
    }
}

// p points at the first character of a value (whitespace already skipped).
static const char *skip_value(const char *p, int depth) {
    if (depth > JSON_MAX_DEPTH) return NULL;
    switch (*p) {
    case '"':
        return skip_string(p);
    case '{':
        return skip_container(p, depth, '}', true);
    case '[':
        return skip_container(p, depth, ']', false);
    default: {
        // Number or literal: consume up to the next structural character.
        const char *start = p;
        while (*p && !strchr(",}] \t\r\n", *p)) p++;
        return p > start ? p : NULL;
    }
    }
}

static bool segment_is_index(const char *seg, size_t len, size_t *index) {
    if (len == 0 || len > 4) return false;
    size_t value = 0;
    for (size_t i = 0; i < len; i++) {
        if (seg[i] < '0' || seg[i] > '9') return false;
        value = value * 10 + (size_t)(seg[i] - '0');
    }
    *index = value;
    return true;
}

// Returns the start of the value at `path`, or NULL.
static const char *find_value(const char *json, const char *path) {
    if (!json || !path) return NULL;
    const char *v = skip_ws(json);
    const char *seg = path;
    while (*seg) {
        const char *dot = strchr(seg, '.');
        size_t seg_len = dot ? (size_t)(dot - seg) : strlen(seg);
        size_t index;
        const char *found = NULL;

        if (*v == '{') {
            const char *p = skip_ws(v + 1);
            while (*p == '"') {
                const char *key = p + 1;
                const char *after_key = skip_string(p);
                if (!after_key) return NULL;
                size_t key_len = (size_t)(after_key - 1 - key);
                p = skip_ws(after_key);
                if (*p != ':') return NULL;
                p = skip_ws(p + 1);
                if (key_len == seg_len && memcmp(key, seg, seg_len) == 0) {
                    found = p;
                    break;
                }
                p = skip_value(p, 1);
                if (!p) return NULL;
                p = skip_ws(p);
                if (*p != ',') break;
                p = skip_ws(p + 1);
            }
        } else if (*v == '[' && segment_is_index(seg, seg_len, &index)) {
            const char *p = skip_ws(v + 1);
            for (size_t i = 0; *p && *p != ']'; i++) {
                if (i == index) {
                    found = p;
                    break;
                }
                p = skip_value(p, 1);
                if (!p) return NULL;
                p = skip_ws(p);
                if (*p != ',') break;
                p = skip_ws(p + 1);
            }
        }

        if (!found) return NULL;
        v = found;
        seg = dot ? dot + 1 : seg + seg_len;
    }
    return v;
}

bool home_json_number(const char *json, const char *path, double *out) {
    const char *v = find_value(json, path);
    if (!v || !(*v == '-' || (*v >= '0' && *v <= '9'))) return false;
    char *end = NULL;
    double value = strtod(v, &end);
    if (end == v) return false;
    *out = value;
    return true;
}

bool home_json_bool(const char *json, const char *path, bool *out) {
    const char *v = find_value(json, path);
    if (!v) return false;
    if (strncmp(v, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(v, "false", 5) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool read_hex4(const char *p, uint32_t *out) {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
        int h = hex_value(p[i]);
        if (h < 0) return false;
        value = (value << 4) | (uint32_t)h;
    }
    *out = value;
    return true;
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

// Length of the UTF-8 sequence that starts with lead byte c (1 for invalid bytes).
static size_t utf8_seq_len(unsigned char c) {
    if (c >= 0xF0) return 4;
    if (c >= 0xE0) return 3;
    if (c >= 0xC0) return 2;
    return 1;
}

bool home_json_string(const char *json, const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    const char *v = find_value(json, path);
    if (!v || *v != '"') return false;
    if (!skip_string(v)) return false;

    size_t len = 0;
    bool full = false;
    const char *p = v + 1;
    while (*p != '"') {
        char buf[4];
        size_t n;
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"': buf[0] = '"'; n = 1; break;
            case '\\': buf[0] = '\\'; n = 1; break;
            case '/': buf[0] = '/'; n = 1; break;
            case 'b': buf[0] = '\b'; n = 1; break;
            case 'f': buf[0] = '\f'; n = 1; break;
            case 'n': buf[0] = '\n'; n = 1; break;
            case 'r': buf[0] = '\r'; n = 1; break;
            case 't': buf[0] = '\t'; n = 1; break;
            case 'u': {
                uint32_t cp;
                if (!read_hex4(p + 1, &cp)) return false;
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[1] == '\\' && p[2] == 'u') {
                    uint32_t low;
                    if (read_hex4(p + 3, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        p += 6;
                    }
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) cp = '?';  // unpaired surrogate
                n = utf8_encode(cp, buf);
                break;
            }
            default:
                return false;
            }
            p++;
        } else {
            n = utf8_seq_len((unsigned char)*p);
            for (size_t i = 0; i < n; i++) {
                if (p[i] == '\0' || (i > 0 && p[i] == '"')) {
                    n = 1;  // truncated sequence: copy the lead byte only
                    break;
                }
                buf[i] = p[i];
            }
            p += n;
        }
        if (!full && len + n < out_size) {
            memcpy(out + len, buf, n);
            len += n;
        } else {
            full = true;  // keep scanning so malformed input is still rejected
        }
    }
    out[len] = '\0';
    return true;
}
