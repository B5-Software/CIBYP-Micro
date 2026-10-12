/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cibyp_micro.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "failure %s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);}} while (0)
static cm_journal disk; static int value, writes, fail_store;
static int load(void *c, cm_journal *j) {(void)c; if (disk.version) *j = disk; return 0;}
static int store(void *c, const cm_journal *j) {(void)c; if (fail_store) return -1; disk = *j; return 0;}
static int gpio_read(void *c, int *v) {(void)c; *v = value; return 0;}
static int gpio_write(void *c, int v, uint32_t ms) {(void)c; CHECK(ms > 0); value = v; ++writes; return 0;}
static int custom_read(void *c, const cm_json *j, int input, uint32_t ms, char *out, size_t cap) {(void)c; (void)j; (void)input; CHECK(ms); return snprintf(out, cap, "{\"celsius\":23}");}
static int run(cm_runtime *r, const char *operation, int v, int generation, char *out) {char frame[CM_FRAME]; int n = snprintf(frame, sizeof frame, "{\"jsonrpc\":\"2.0\",\"id\":\"request\",\"method\":\"tool.invoke\",\"params\":{\"name\":\"gpio_write\",\"operationId\":\"%s\",\"input\":{\"resource\":\"gpio:17\",\"value\":%d},\"remainingMs\":100,\"gatewayBootId\":\"gateway\",\"gatewayId\":\"gateway-instance\",\"deviceSequence\":%d,\"generation\":%d,\"leaseId\":\"lease\"}}", operation, v, generation, generation); return cm_handle(r, frame, (size_t)n, out, CM_FRAME);}
int main(void) {
  cm_fragments fragments = {0};
  CHECK(cm_fragment(&fragments, "{", 1, 2, 0, 1, 0) == 0);
  CHECK(cm_fragment(&fragments, "\"", 1, 2, 1, 1, 0) == 0);
  CHECK(cm_fragment(&fragments, "x\":1}", 5, 5, 0, 0, 1) == 1);
  CHECK(fragments.length == 7 && !memcmp(fragments.bytes, "{\"x\":1}", 7));
  CHECK(cm_fragment(&fragments, "x", 1, 1, 0, 0, 1) < 0);
  memset(&fragments, 0, sizeof fragments);
  CHECK(cm_fragment(&fragments, "x", 1, CM_FRAME + 1, 0, 1, 1) < 0);
  cm_runtime *r = calloc(1, sizeof *r); CHECK(r); cm_platform p = {NULL, load, store, gpio_read, gpio_write}; char out[CM_FRAME];
  CHECK(!cm_init(r, &p, "board", "gpio:17")); CHECK(cm_hello(r, "boot", out, sizeof out) > 0);
  cm_tool custom = {"temperature", "{\"type\":\"object\",\"properties\":{\"resource\":{\"type\":\"string\"}},\"required\":[\"resource\"]}", 0, NULL, custom_read};
  CHECK(!cm_register_tool(r, &custom)); CHECK(cm_register_tool(r, &custom) < 0);
  const char *custom_request = "{\"jsonrpc\":\"2.0\",\"id\":\"custom\",\"method\":\"tool.invoke\",\"params\":{\"name\":\"temperature\",\"operationId\":\"sensor-read\",\"remainingMs\":100,\"gatewayBootId\":\"gateway\",\"input\":{\"resource\":\"gpio:17\"}}}";
  CHECK(cm_handle(r, custom_request, strlen(custom_request), out, sizeof out) > 0 && strstr(out, "celsius"));
  CHECK(cm_hello(r, "boot", out, sizeof out) > 0);
  cm_json j; CHECK(!cm_parse(&j, out, strlen(out))); CHECK(cm_field(&j, 0, "params") >= 0);
  CHECK(run(r, "one", 1, 1, out) > 0 && strstr(out, "result")); CHECK(writes == 1);
  CHECK(run(r, "one", 1, 1, out) > 0 && strstr(out, "result")); CHECK(writes == 1);
  CHECK(run(r, "one", 0, 1, out) > 0 && strstr(out, "REVISION_CONFLICT"));
  CHECK(run(r, "two", 0, 2, out) > 0 && strstr(out, "result")); CHECK(writes == 2);
  CHECK(run(r, "old", 1, 1, out) > 0 && strstr(out, "FORBIDDEN"));
  CHECK(!cm_init(r, &p, "board", "gpio:17")); CHECK(run(r, "one", 1, 1, out) > 0); CHECK(writes == 2);
  disk.operations[0].state = 1; CHECK(!cm_init(r, &p, "board", "gpio:17")); CHECK(run(r, "one", 1, 1, out) > 0 && strstr(out, "RESULT_UNKNOWN")); CHECK(writes == 2);
  fail_store = 1; CHECK(run(r, "three", 1, 3, out) > 0 && strstr(out, "STORAGE_ERROR")); CHECK(writes == 2);
  const char *invalid[] = {"{", "[1,]", "{\"x\":01}", "{\"x\":truee}", "{\"x\":\"\\q\"}", "{\"x\":1} trailing", "\xc0\xaf", "{\"x\":1e+}"};
  for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) CHECK(cm_parse(&j, invalid[i], strlen(invalid[i])) < 0);
  char big[CM_FRAME + 1]; memset(big, ' ', sizeof big); CHECK(cm_parse(&j, big, sizeof big) < 0);
  CHECK(!cm_parse(&j, "{\"x\":1,\"x\":2}", 13)); CHECK(cm_field(&j, 0, "x") < 0);
  CHECK(!cm_parse(&j, "{\"x\":2147483648}", 16)); int number; CHECK(cm_integer(&j, cm_field(&j, 0, "x"), &number) < 0);
  fail_store = 0; memset(&disk, 0, sizeof disk); CHECK(!cm_init(r, &p, "board", "gpio:17"));
  for (int i = 1; i <= CM_OPERATIONS * 3; ++i) {char op[40]; snprintf(op, sizeof op, "sequence-%d", i); CHECK(run(r, op, i % 2, i, out) > 0 && strstr(out, "result"));}
  int previous_writes = writes;
  CHECK(!cm_init(r, &p, "board", "gpio:17"));
  CHECK(run(r, "sequence-1", 1, 1, out) > 0 && strstr(out, "RESULT_UNKNOWN")); CHECK(writes == previous_writes);
  /* Deterministic malformed-input smoke corpus; ASan/UBSan run this in CI. */
  unsigned random_state = 123456789u;
  for (int trial = 0; trial < 10000; ++trial) {
    char malformed[256]; size_t len = (size_t)(trial % 256);
    for (size_t i = 0; i < len; ++i) {random_state = random_state * 1664525u + 1013904223u; malformed[i] = (char)(random_state >> 24);}
    (void)cm_parse(&j, malformed, len);
    (void)cm_handle(r, malformed, len, out, sizeof out);
  }
  printf("runtime=%lu journal=%lu fragments=%lu bytes\n", (unsigned long)sizeof *r, (unsigned long)sizeof disk, (unsigned long)sizeof fragments);
  free(r); puts("CIBYP Micro contract tests passed"); return 0;
}
