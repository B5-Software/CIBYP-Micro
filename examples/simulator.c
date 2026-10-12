/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cibyp_micro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct {const char *file; int value;} simulator;
static int load(void *c, cm_journal *j) {simulator *s = c; FILE *f = fopen(s->file, "rb"); if (!f) return 0; size_t n = fread(j, 1, sizeof *j, f); int extra = fgetc(f); fclose(f); return n == sizeof *j && extra == EOF ? 0 : -1;}
static int store(void *c, const cm_journal *j) {simulator *s = c; char tmp[1024]; int n = snprintf(tmp, sizeof tmp, "%s.tmp", s->file); if (n < 0 || (size_t)n >= sizeof tmp) return -1; FILE *f = fopen(tmp, "wb"); if (!f) return -1; int ok = fwrite(j, 1, sizeof *j, f) == sizeof *j; if (fclose(f)) ok = 0; if (!ok) return -1;
#ifdef _WIN32
  /* Host simulator only: NTFS rename cannot replace an open/existing file via ISO C. */
  (void)remove(s->file);
#endif
  return rename(tmp, s->file);
}
static int read_gpio(void *c, int *v) {*v = ((simulator *)c)->value; return 0;}
static int write_gpio(void *c, int v, uint32_t deadline) {((simulator *)c)->value = v; fprintf(stderr, "SIMULATED GPIO=%d fail-safe budget=%u ms\n", v, (unsigned)deadline); fflush(stderr); return 0;}
int main(int argc, char **argv) {
  if (argc != 4) {fprintf(stderr, "usage: micro-simulator DEVICE RESOURCE JOURNAL_FILE\n"); return 2;}
  simulator state = {argv[3], 0}; cm_platform p = {&state, load, store, read_gpio, write_gpio};
  cm_runtime *r = calloc(1, sizeof *r); char frame[CM_FRAME + 2], out[CM_FRAME]; if (!r) return 2;
  if (cm_init(r, &p, argv[1], argv[2])) {fprintf(stderr, "Cannot initialize persistent device journal\n"); free(r); return 1;}
  if (cm_hello(r, "host-simulator", out, sizeof out) > 0) {puts(out); fflush(stdout);}
  while (fgets(frame, sizeof frame, stdin)) {
    size_t n = strlen(frame); if (n && frame[n - 1] == '\n') --n; else if (!feof(stdin)) {fprintf(stderr, "Frame too large\n"); break;}
    int bytes = cm_handle(r, frame, n, out, sizeof out); if (bytes > 0) {puts(out); fflush(stdout);} else if (bytes < 0) fprintf(stderr, "Invalid frame\n");
  } free(r); return 0;
}
