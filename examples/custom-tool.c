/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cibyp_micro.h"
#include <stdio.h>
/* Compile into your firmware and call register_temperature(runtime).
 * Replace the deterministic value with a bounded HAL sensor read. */
static int temperature(void *context, const cm_json *json, int input, uint32_t ms, char *out, size_t cap) {
  (void)context; (void)json; (void)input; if (!ms) return -1;
  int n = snprintf(out, cap, "{\"celsius\":23.5,\"unit\":\"C\"}"); return n < 0 || (size_t)n >= cap ? -1 : n;
}
int register_temperature(cm_runtime *runtime) {
  cm_tool tool = {"temperature", "{\"type\":\"object\",\"properties\":{\"resource\":{\"type\":\"string\"}},\"required\":[\"resource\"],\"additionalProperties\":false}", 0, NULL, temperature};
  return cm_register_tool(runtime, &tool);
}
