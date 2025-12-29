#ifndef JSONL_H
#define JSONL_H
// Minimal helpers to build/parse tiny JSON objects used in this project.
// For MVP: we only need (op, strings, ints). Use naive parsing by keys.
char *jsonl_build(const char *fmt, ...);
// Example: jsonl_build("{\"op\":\"%s\",\"user\":\"%s\"}", op, user);

// Very small helpers to extract a string/int by key from a single-line JSON.
int json_get_str(const char *json, const char *key, char *out, int outlen);
int json_get_int(const char *json, const char *key, int *out);
#endif

