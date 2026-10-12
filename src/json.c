/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cibyp_micro.h"
#include <string.h>
#include <limits.h>
#include <stdlib.h>
#include <errno.h>
static void space(cm_json *j, size_t *p) {while (*p < j->length && strchr(" \t\r\n", j->text[*p])) ++*p;}
static int hex(char c) {return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');}
static int utf8(const unsigned char *s, size_t n) {
  size_t i = 0; while (i < n) {unsigned c = s[i++], count, cp, min;
    if (c < 128) {if (!c) return 0; continue;}
    if (c >= 0xc2 && c <= 0xdf) {count = 1; cp = c & 31; min = 128;}
    else if (c >= 0xe0 && c <= 0xef) {count = 2; cp = c & 15; min = 2048;}
    else if (c >= 0xf0 && c <= 0xf4) {count = 3; cp = c & 7; min = 65536;} else return 0;
    if (i + count > n) return 0;
    while (count--) {c = s[i++]; if ((c & 0xc0) != 0x80) return 0; cp = (cp << 6) | (c & 63);}
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
  } return 1;
}
static int value(cm_json *j, size_t *p, int depth) {
  space(j, p); if (depth > 16 || *p >= j->length || j->count >= CM_TOKENS) return -1;
  int index = j->count++; cm_token *t = &j->tokens[index]; t->start = (int)*p; char c = j->text[(*p)++]; t->type = c;
  if (c == '{' || c == '[') {
    char end = c == '{' ? '}' : ']'; space(j, p);
    if (*p < j->length && j->text[*p] == end) ++*p;
    else for (;;) {
      if (c == '{') {space(j, p); if (*p >= j->length || j->text[*p] != '"' || value(j, p, depth + 1) < 0) return -1; space(j, p); if (*p >= j->length || j->text[(*p)++] != ':') return -1;}
      if (value(j, p, depth + 1) < 0) return -1;
      space(j, p); if (*p >= j->length) return -1;
      char next = j->text[(*p)++]; if (next == end) break; if (next != ',') return -1;
    }
  } else if (c == '"') {
    int closed = 0;
    while (*p < j->length) {unsigned char ch = (unsigned char)j->text[(*p)++]; if (ch == '"') {closed = 1; break;} if (ch < 32) return -1;
      if (ch == '\\') {if (*p >= j->length) return -1; char esc = j->text[(*p)++]; if (esc == 'u') {for (int k = 0; k < 4; ++k) if (*p >= j->length || !hex(j->text[(*p)++])) return -1;} else if (!strchr("\"\\/bfnrt", esc)) return -1;}
    } if (!closed) return -1;
  } else {
    t->type = 'p'; while (*p < j->length && !strchr(" \t\r\n,}]", j->text[*p])) ++*p;
    size_t n = *p - (size_t)t->start; const char *s = j->text + t->start;
    if (!(n == 4 && (!memcmp(s, "true", 4) || !memcmp(s, "null", 4))) && !(n == 5 && !memcmp(s, "false", 5))) {
      size_t a = 0; if (s[a] == '-') ++a; if (a >= n) return -1;
      if (s[a] == '0') ++a; else {if (s[a] < '1' || s[a] > '9') return -1; while (a < n && s[a] >= '0' && s[a] <= '9') ++a;}
      if (a < n && s[a] == '.') {++a; size_t start = a; while (a < n && s[a] >= '0' && s[a] <= '9') ++a; if (a == start) return -1;}
      if (a < n && (s[a] == 'e' || s[a] == 'E')) {++a; if (a < n && (s[a] == '+' || s[a] == '-')) ++a; size_t start = a; while (a < n && s[a] >= '0' && s[a] <= '9') ++a; if (a == start) return -1;}
      if (a != n) return -1;
    }
  }
  t->end = (int)*p; t->next = j->count; return index;
}
int cm_parse(cm_json *j, const char *text, size_t length) {
  if (!j || !text || !length || length > CM_FRAME || !utf8((const unsigned char *)text, length)) return -1;
  j->text = text; j->length = length; j->count = 0; size_t p = 0;
  if (value(j, &p, 0) < 0) return -1;
  space(j, &p); return p == length ? 0 : -1;
}
int cm_string(const cm_json *j, int t, char *out, size_t cap) {
  if (t < 0 || t >= j->count || j->tokens[t].type != '"') return -1;
  size_t n = (size_t)(j->tokens[t].end - j->tokens[t].start - 2);
  if (n + 1 > cap) return -1;
  const char *s = j->text + j->tokens[t].start + 1;
  /* Protocol identifiers never accept escapes; arbitrary JSON string values stay in input. */
  for (size_t i = 0; i < n; ++i) if (s[i] == '\\' || (unsigned char)s[i] < 32) return -1;
  memcpy(out, s, n); out[n] = 0; return 0;
}
int cm_field(const cm_json *j, int object, const char *key) {
  if (object < 0 || object >= j->count || j->tokens[object].type != '{') return -1;
  int found = -1;
  for (int t = object + 1; t < j->tokens[object].next;) {char name[81]; if (cm_string(j, t, name, sizeof name)) return -1; int v = t + 1;
    if (!strcmp(name, key)) {if (found >= 0) return -1; found = v;} t = j->tokens[v].next;
  } return found;
}
int cm_integer(const cm_json *j, int t, int *value_out) {
  if (t < 0 || t >= j->count || j->tokens[t].type != 'p') return -1;
  int n = j->tokens[t].end - j->tokens[t].start; if (n < 1 || n > 11) return -1;
  char s[12]; memcpy(s, j->text + j->tokens[t].start, (size_t)n); s[n] = 0;
  char *end; errno = 0; long v = strtol(s, &end, 10); if (errno == ERANGE || *end || v < INT_MIN || v > INT_MAX) return -1; *value_out = (int)v; return 0;
}
