#define _GNU_SOURCE
#include "mjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MJ_MAX_DEPTH 64
#define MJ_MAX_TEXT (64u << 20)

struct MJ {
  int type;
  int b;
  double num;
  char *str;
  MJ **kid;
  char **key;
  size_t n, cap;
};

typedef struct { const char *p, *end; char *err; size_t errcap; int failed; } Parser;

static void fail(Parser *ps, const char *msg) {
  if (!ps->failed && ps->err && ps->errcap)
    snprintf(ps->err, ps->errcap, "JSON: %s", msg);
  ps->failed = 1;
}

static void skip_ws(Parser *ps) {
  while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')) ps->p++;
}

static MJ *node(int type) {
  MJ *j = calloc(1, sizeof *j);
  if (j) j->type = type;
  return j;
}

void mj_free(MJ *j) {
  if (!j) return;
  free(j->str);
  for (size_t i = 0; i < j->n; i++) {
    mj_free(j->kid[i]);
    if (j->key) free(j->key[i]);
  }
  free(j->kid);
  free(j->key);
  free(j);
}

static int add_kid(MJ *j, const char *key, MJ *kid) {
  if (j->n == j->cap) {
    size_t nc = j->cap ? j->cap * 2 : 8;
    MJ **nk = realloc(j->kid, nc * sizeof *nk);
    if (!nk) return -1;
    j->kid = nk;
    if (j->type == MJ_OBJ) {
      char **nkey = realloc(j->key, nc * sizeof *nkey);
      if (!nkey) return -1;
      j->key = nkey;
    }
    j->cap = nc;
  }
  if (j->type == MJ_OBJ) {
    j->key[j->n] = strdup(key ? key : "");
    if (!j->key[j->n]) return -1;
  }
  j->kid[j->n++] = kid;
  return 0;
}

static int hex4(const char *p, unsigned *out) {
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    else return -1;
  }
  *out = v;
  return 0;
}

static void put_utf8(char **o, unsigned cp) {
  char *p = *o;
  if (cp < 0x80) *p++ = (char)cp;
  else if (cp < 0x800) { *p++ = (char)(0xC0 | (cp >> 6)); *p++ = (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { *p++ = (char)(0xE0 | (cp >> 12)); *p++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *p++ = (char)(0x80 | (cp & 0x3F)); }
  else { *p++ = (char)(0xF0 | (cp >> 18)); *p++ = (char)(0x80 | ((cp >> 12) & 0x3F)); *p++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *p++ = (char)(0x80 | (cp & 0x3F)); }
  *o = p;
}

// Parses a string starting at the opening quote; returns a malloc'd, unescaped copy.
static char *parse_string(Parser *ps) {
  ps->p++;  // opening quote
  size_t maxlen = (size_t)(ps->end - ps->p);
  char *out = malloc(maxlen + 1);
  if (!out) { fail(ps, "out of memory"); return NULL; }
  char *o = out;
  while (ps->p < ps->end && *ps->p != '"') {
    unsigned char c = (unsigned char)*ps->p++;
    if (c < 0x20) { free(out); fail(ps, "control character in string"); return NULL; }
    if (c != '\\') { *o++ = (char)c; continue; }
    if (ps->p >= ps->end) break;
    char e = *ps->p++;
    switch (e) {
      case '"': *o++ = '"'; break;
      case '\\': *o++ = '\\'; break;
      case '/': *o++ = '/'; break;
      case 'b': *o++ = '\b'; break;
      case 'f': *o++ = '\f'; break;
      case 'n': *o++ = '\n'; break;
      case 'r': *o++ = '\r'; break;
      case 't': *o++ = '\t'; break;
      case 'u': {
        unsigned cp;
        if (ps->end - ps->p < 4 || hex4(ps->p, &cp) != 0) { free(out); fail(ps, "bad \\u escape"); return NULL; }
        ps->p += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && ps->end - ps->p >= 6 && ps->p[0] == '\\' && ps->p[1] == 'u') {
          unsigned lo;
          if (hex4(ps->p + 2, &lo) == 0 && lo >= 0xDC00 && lo < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            ps->p += 6;
          }
        }
        if (cp == 0) cp = 0xFFFD;  // an embedded NUL would truncate the string
        put_utf8(&o, cp);
        break;
      }
      default: free(out); fail(ps, "bad escape"); return NULL;
    }
  }
  if (ps->p >= ps->end) { free(out); fail(ps, "unterminated string"); return NULL; }
  ps->p++;  // closing quote
  *o = 0;
  return out;
}

static MJ *parse_value(Parser *ps, int depth) {
  if (depth > MJ_MAX_DEPTH) { fail(ps, "nesting too deep"); return NULL; }
  skip_ws(ps);
  if (ps->p >= ps->end) { fail(ps, "unexpected end"); return NULL; }
  char c = *ps->p;
  if (c == '{' || c == '[') {
    int obj = c == '{';
    char close = obj ? '}' : ']';
    MJ *j = node(obj ? MJ_OBJ : MJ_ARR);
    if (!j) { fail(ps, "out of memory"); return NULL; }
    ps->p++;
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == close) { ps->p++; return j; }
    for (;;) {
      char *key = NULL;
      skip_ws(ps);
      if (obj) {
        if (ps->p >= ps->end || *ps->p != '"') { fail(ps, "expected a member name"); mj_free(j); return NULL; }
        key = parse_string(ps);
        if (!key) { mj_free(j); return NULL; }
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') { free(key); fail(ps, "expected ':'"); mj_free(j); return NULL; }
        ps->p++;
      }
      MJ *kid = parse_value(ps, depth + 1);
      if (!kid) { free(key); mj_free(j); return NULL; }
      if (add_kid(j, key, kid) != 0) { free(key); mj_free(kid); mj_free(j); fail(ps, "out of memory"); return NULL; }
      free(key);
      skip_ws(ps);
      if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
      if (ps->p < ps->end && *ps->p == close) { ps->p++; return j; }
      fail(ps, obj ? "expected ',' or '}'" : "expected ',' or ']'");
      mj_free(j);
      return NULL;
    }
  }
  if (c == '"') {
    char *s = parse_string(ps);
    if (!s) return NULL;
    MJ *j = node(MJ_STR);
    if (!j) { free(s); fail(ps, "out of memory"); return NULL; }
    j->str = s;
    return j;
  }
  if (!strncmp(ps->p, "true", 4)) { ps->p += 4; MJ *j = node(MJ_BOOL); if (j) j->b = 1; return j; }
  if (!strncmp(ps->p, "false", 5)) { ps->p += 5; return node(MJ_BOOL); }
  if (!strncmp(ps->p, "null", 4)) { ps->p += 4; return node(MJ_NULL); }
  if (c == '-' || (c >= '0' && c <= '9')) {
    char *e = NULL;
    double d = strtod(ps->p, &e);
    if (e == ps->p || e > ps->end) { fail(ps, "bad number"); return NULL; }
    ps->p = e;
    MJ *j = node(MJ_NUM);
    if (!j) { fail(ps, "out of memory"); return NULL; }
    j->num = d;
    return j;
  }
  fail(ps, "unexpected character");
  return NULL;
}

MJ *mj_parse(const char *text, char *err, size_t errcap) {
  if (err && errcap) err[0] = 0;
  if (!text) { if (err && errcap) snprintf(err, errcap, "JSON: no input"); return NULL; }
  size_t len = strlen(text);
  if (len > MJ_MAX_TEXT) { if (err && errcap) snprintf(err, errcap, "JSON: document too large"); return NULL; }
  // `text` is NUL-terminated, so the literal compares below cannot run past the end.
  Parser ps = {text, text + len, err, errcap, 0};
  // tolerate a UTF-8 byte order mark and leading junk-free whitespace
  if (len >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) ps.p += 3;
  MJ *j = parse_value(&ps, 0);
  if (!j) {
    if (err && errcap && !err[0]) snprintf(err, errcap, "JSON: parse error");
    return NULL;
  }
  skip_ws(&ps);
  if (ps.p != ps.end) {
    mj_free(j);
    if (err && errcap) snprintf(err, errcap, "JSON: trailing data after the document");
    return NULL;
  }
  return j;
}

int mj_type(const MJ *j) { return j ? j->type : MJ_NULL; }
size_t mj_len(const MJ *j) { return (j && (j->type == MJ_ARR || j->type == MJ_OBJ)) ? j->n : 0; }
const MJ *mj_at(const MJ *j, size_t i) { return (j && (j->type == MJ_ARR || j->type == MJ_OBJ) && i < j->n) ? j->kid[i] : NULL; }
const char *mj_key_at(const MJ *j, size_t i) { return (j && j->type == MJ_OBJ && i < j->n) ? j->key[i] : NULL; }
const char *mj_str(const MJ *j) { return (j && j->type == MJ_STR) ? j->str : NULL; }
double mj_num(const MJ *j) { return (j && j->type == MJ_NUM) ? j->num : 0; }
int mj_bool(const MJ *j) { return j && j->type == MJ_BOOL && j->b; }

const MJ *mj_get(const MJ *obj, const char *key) {
  if (!obj || obj->type != MJ_OBJ) return NULL;
  for (size_t i = 0; i < obj->n; i++)
    if (!strcmp(obj->key[i], key)) return obj->kid[i];
  return NULL;
}
const char *mj_get_str(const MJ *obj, const char *key, const char *def) {
  const char *s = mj_str(mj_get(obj, key));
  return s ? s : def;
}
double mj_get_num(const MJ *obj, const char *key, double def) {
  const MJ *n = mj_get(obj, key);
  return (n && n->type == MJ_NUM) ? n->num : def;
}
