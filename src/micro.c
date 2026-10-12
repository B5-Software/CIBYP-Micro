/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cibyp_micro.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
static int identifier(const char *s, size_t max) {size_t n = strlen(s); if (!n || n > max) return 0; for (size_t i = 0; i < n; ++i) if (!isalnum((unsigned char)s[i]) && !strchr("_.:@/-", s[i])) return 0; return 1;}
static int answer(char *out, size_t cap, const char *id, const char *result, const char *reason) {
  int code = -32603;
  if (reason) {
    if (!strcmp(reason, "INVALID_PARAMS")) code = -32602;
    else if (!strcmp(reason, "METHOD_NOT_FOUND")) code = -32601;
    else if (!strcmp(reason, "FORBIDDEN")) code = 1003;
    else if (!strcmp(reason, "CAPABILITY_UNAVAILABLE")) code = 1004;
    else if (!strcmp(reason, "REVISION_CONFLICT")) code = 1009;
    else if (!strcmp(reason, "RESOURCE_BUSY")) code = 1010;
    else if (!strcmp(reason, "RESULT_UNKNOWN")) code = 1013;
    else if (!strcmp(reason, "PAYLOAD_TOO_LARGE")) code = 1014;
  }
  int n = reason ? snprintf(out, cap, "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"error\":{\"code\":%d,\"message\":\"%s\",\"data\":{\"reason\":\"%s\",\"retryable\":false}}}", id, code, reason, reason) : snprintf(out, cap, "{\"jsonrpc\":\"2.0\",\"id\":\"%s\",\"result\":%s}", id, result);
  return n < 0 || (size_t)n >= cap ? -1 : n;
}
static const char *gpio_schema = "{\"type\":\"object\",\"properties\":{\"resource\":{\"type\":\"string\"},\"value\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":1}},\"required\":[\"resource\"],\"additionalProperties\":false}";
static int gpio_read_tool(void *context, const cm_json *j, int input, uint32_t ms, char *result, size_t capacity) {
  (void)j; (void)input; (void)ms; cm_runtime *r = context; int value;
  if (r->platform.gpio_read(r->platform.context, &value)) return -1;
  int n = snprintf(result, capacity, "{\"ok\":true,\"value\":%d}", value); return n < 0 || (size_t)n >= capacity ? -1 : n;
}
static int gpio_write_tool(void *context, const cm_json *j, int input, uint32_t ms, char *result, size_t capacity) {
  cm_runtime *r = context; int value;
  if (cm_integer(j, cm_field(j, input, "value"), &value) || (value != 0 && value != 1) || r->platform.gpio_write(r->platform.context, value, ms)) return -1;
  int n = snprintf(result, capacity, "{\"ok\":true,\"value\":%d}", value); return n < 0 || (size_t)n >= capacity ? -1 : n;
}
int cm_register_tool(cm_runtime *r, const cm_tool *tool) {
  if (!r || !tool || !tool->execute || !tool->input_schema || !identifier(tool->name, 80) || r->tool_count >= CM_TOOLS) return -1;
  cm_json json; if (cm_parse(&json, tool->input_schema, strlen(tool->input_schema)) || json.tokens[0].type != '{') return -1;
  for (int i = 0; i < r->tool_count; ++i) if (!strcmp(r->tools[i].name, tool->name)) return -1;
  r->tools[r->tool_count++] = *tool; return 0;
}
int cm_init(cm_runtime *r, const cm_platform *p, const char *device, const char *resource) {
  if (!r || !p || !p->store || !p->gpio_read || !p->gpio_write || !identifier(device, 80) || !identifier(resource, 80)) return -1;
  memset(r, 0, sizeof *r); r->platform = *p; strcpy(r->device, device); strcpy(r->resource, resource); r->journal.version = 2;
  if (p->load && p->load(p->context, &r->journal)) return -1;
  if (r->journal.version != 2 || !memchr(r->journal.gateway_id, 0, sizeof r->journal.gateway_id)) return -1;
  for (int i = 0; i < CM_OPERATIONS; ++i) {
    cm_operation *o = &r->journal.operations[i];
    if (o->state > 3 || !memchr(o->operation, 0, sizeof o->operation) || !memchr(o->name, 0, sizeof o->name) || !memchr(o->input, 0, sizeof o->input) || !memchr(o->result, 0, sizeof o->result)) return -1;
    if (o->state == 1) o->state = 3; /* interrupted physical action: unknown */
  }
  if (p->store(p->context, &r->journal)) return -1;
  cm_tool read_tool = {"gpio_read", gpio_schema, 0, r, gpio_read_tool}, write_tool = {"gpio_write", gpio_schema, 1, r, gpio_write_tool};
  if (cm_register_tool(r, &read_tool) || cm_register_tool(r, &write_tool)) return -1;
  r->ready = 1; return 0;
}
int cm_hello(const cm_runtime *r, const char *boot, char *out, size_t cap) {
  if (!r->ready || !identifier(boot, 80)) return -1;
  int n = snprintf(out, cap, "{\"jsonrpc\":\"2.0\",\"id\":\"hello\",\"method\":\"device.hello\",\"params\":{\"deviceId\":\"%s\",\"bootId\":\"%s\",\"tools\":[", r->device, boot);
  if (n < 0 || (size_t)n >= cap) return -1;
  size_t total = (size_t)n;
  for (int i = 0; i < r->tool_count; ++i) {n = snprintf(out + total, cap - total, "%s{\"name\":\"%s\",\"inputSchema\":%s}", i ? "," : "", r->tools[i].name, r->tools[i].input_schema); if (n < 0 || (size_t)n >= cap - total) return -1; total += (size_t)n;}
  n = snprintf(out + total, cap - total, "]}}"); return n < 0 || (size_t)n >= cap - total ? -1 : (int)total + n;
}
int cm_handle(cm_runtime *r, const char *input, size_t length, char *out, size_t cap) {
  if (!r->ready) return -1;
  cm_json j; if (cm_parse(&j, input, length)) return -1;
  char version[8], id[161], method[81];
  if (cm_string(&j, cm_field(&j, 0, "jsonrpc"), version, sizeof version) || strcmp(version, "2.0") || cm_string(&j, cm_field(&j, 0, "id"), id, sizeof id) || !identifier(id, 160)) return -1;
  int m = cm_field(&j, 0, "method"); if (m < 0) return 0; /* gateway hello acknowledgement */
  if (cm_string(&j, m, method, sizeof method) || strcmp(method, "tool.invoke")) return answer(out, cap, id, NULL, "METHOD_NOT_FOUND");
  int p = cm_field(&j, 0, "params"), a = cm_field(&j, p, "input");
  char name[81], operation[161], resource[81], gateway[161], gateway_id[161], lease[161]; int remaining, generation, sequence = 0;
  if (a < 0 || j.tokens[a].type != '{' || cm_string(&j, cm_field(&j, p, "name"), name, sizeof name) || cm_string(&j, cm_field(&j, p, "operationId"), operation, sizeof operation) || !identifier(operation, 160) || cm_string(&j, cm_field(&j, a, "resource"), resource, sizeof resource) || strcmp(resource, r->resource) || cm_string(&j, cm_field(&j, p, "gatewayBootId"), gateway, sizeof gateway) || !identifier(gateway, 160) || cm_integer(&j, cm_field(&j, p, "remainingMs"), &remaining) || remaining <= 0 || remaining > 60000) return answer(out, cap, id, NULL, "INVALID_PARAMS");
  const cm_tool *tool = NULL; for (int i = 0; i < r->tool_count; ++i) if (!strcmp(name, r->tools[i].name)) tool = &r->tools[i];
  if (!tool) return answer(out, cap, id, NULL, "CAPABILITY_UNAVAILABLE");
  int write = tool->requires_lease;
  if (write) {
    if (cm_string(&j, cm_field(&j, p, "gatewayId"), gateway_id, sizeof gateway_id) || !identifier(gateway_id, 160) || cm_integer(&j, cm_field(&j, p, "deviceSequence"), &sequence) || sequence <= 0) return answer(out, cap, id, NULL, "INVALID_PARAMS");
    if (r->journal.gateway_id[0] && strcmp(gateway_id, r->journal.gateway_id)) return answer(out, cap, id, NULL, "FORBIDDEN");
    if (cm_string(&j, cm_field(&j, p, "leaseId"), lease, sizeof lease) || !identifier(lease, 160) || cm_integer(&j, cm_field(&j, p, "generation"), &generation) || generation <= 0) return answer(out, cap, id, NULL, "FORBIDDEN");
    if (!strcmp(gateway, r->gateway) && ((uint32_t)generation < r->generation || ((uint32_t)generation == r->generation && strcmp(lease, r->lease)))) return answer(out, cap, id, NULL, "FORBIDDEN");
  }
  char raw[CM_INPUT]; int n = j.tokens[a].end - j.tokens[a].start; if (n < 0 || (size_t)n >= sizeof raw) return answer(out, cap, id, NULL, "PAYLOAD_TOO_LARGE"); memcpy(raw, input + j.tokens[a].start, (size_t)n); raw[n] = 0;
  cm_operation *slot = NULL;
  for (int i = 0; i < CM_OPERATIONS; ++i) {cm_operation *o = &r->journal.operations[i];
    if (o->state && !strcmp(o->operation, operation)) {
      if (strcmp(o->name, name) || strcmp(o->input, raw)) return answer(out, cap, id, NULL, "REVISION_CONFLICT");
      return answer(out, cap, id, o->result, o->state == 2 ? NULL : "RESULT_UNKNOWN");
    } if (!o->state && !slot) slot = o;
  }
  if (write) {
    if ((uint32_t)sequence <= r->journal.accepted_sequence) return answer(out, cap, id, NULL, "RESULT_UNKNOWN");
    if (!slot) {
      /* Only confirmed outcomes can retire. A durable high-water mark rejects
       * old frames after their detailed result is evicted, including on reboot. */
      for (int i = 0; i < CM_OPERATIONS; ++i) {cm_operation *o = &r->journal.operations[i]; if (o->state == 2 && (!slot || o->sequence < slot->sequence)) slot = o;}
      if (slot) memset(slot, 0, sizeof *slot);
    }
    if (!slot) return answer(out, cap, id, NULL, "RESOURCE_BUSY"); /* never forget and repeat a physical write */
    strcpy(r->journal.gateway_id, gateway_id);
    r->journal.accepted_sequence = (uint32_t)sequence;
    strcpy(slot->operation, operation); strcpy(slot->name, name); strcpy(slot->input, raw); slot->state = 1; slot->sequence = (uint32_t)sequence;
    if (r->platform.store(r->platform.context, &r->journal)) {slot->state = 3; r->ready = 0; return answer(out, cap, id, NULL, "STORAGE_ERROR");}
    strcpy(r->gateway, gateway); strcpy(r->lease, lease); r->generation = (uint32_t)generation;
    int bytes = tool->execute(tool->context, &j, a, (uint32_t)remaining, slot->result, sizeof slot->result);
    cm_json result_json;
    if (bytes < 0 || (size_t)bytes >= sizeof slot->result || cm_parse(&result_json, slot->result, (size_t)bytes)) {slot->state = 3; (void)r->platform.store(r->platform.context, &r->journal); return answer(out, cap, id, NULL, "RESULT_UNKNOWN");}
    slot->result[bytes] = 0;
    slot->state = 2;
    if (r->platform.store(r->platform.context, &r->journal)) {slot->state = 3; r->ready = 0; return answer(out, cap, id, NULL, "RESULT_UNKNOWN");}
    return answer(out, cap, id, slot->result, NULL);
  }
  char result[256]; int bytes = tool->execute(tool->context, &j, a, (uint32_t)remaining, result, sizeof result);
  cm_json result_json;
  if (bytes < 0 || (size_t)bytes >= sizeof result || cm_parse(&result_json, result, (size_t)bytes)) return answer(out, cap, id, NULL, "DEVICE_ERROR");
  result[bytes] = 0;
  return answer(out, cap, id, result, NULL);
}
