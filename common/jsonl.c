#include "jsonl.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

char *jsonl_build(const char *fmt, ...) {
  static char buf[8192];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return buf;
}

static const char *find_key(const char *json, const char *key) {
  static char pat[256];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  return strstr(json, pat);
}

int json_get_str(const char *json, const char *key, char *out, int outlen) {
  const char *p = find_key(json, key);
  if (!p) return -1;
  p = strchr(p, ':'); if (!p) return -1; p++;
  while (*p==' '){p++;}
  if (*p!='\"') return -1; p++;
  int i=0;
  while (*p && *p!='\"' && i<outlen-1){ out[i++]=*p++; }
  out[i]=0;
  return (*p=='\"') ? 0 : -1;
}

int json_get_int(const char *json, const char *key, int *out) {
  const char *p = find_key(json, key);
  if (!p) return -1;
  p = strchr(p, ':'); if (!p) return -1; p++;
  while (*p==' '){p++;}
  int val=0; int sign=1;
  if (*p=='-'){ sign=-1; p++; }
  if (*p<'0' || *p>'9') return -1;
  while (*p>='0' && *p<='9'){ val = val*10 + (*p-'0'); p++; }
  *out = sign*val;
  return 0;
}

