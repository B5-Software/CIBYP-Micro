/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CIBYP_MICRO_H
#define CIBYP_MICRO_H
#include <stddef.h>
#include <stdint.h>
#define CM_FRAME 4096
#define CM_TOKENS 192
#define CM_OPERATIONS 16
#define CM_INPUT 768
#define CM_TOOLS 16
typedef struct {int start, end, next; char type;} cm_token;
typedef struct {const char *text; size_t length; cm_token tokens[CM_TOKENS]; int count;} cm_json;
int cm_parse(cm_json *json, const char *text, size_t length);
int cm_field(const cm_json *json, int object, const char *key);
int cm_string(const cm_json *json, int token, char *out, size_t capacity);
int cm_integer(const cm_json *json, int token, int *value);
typedef struct {char bytes[CM_FRAME]; size_t length, frame_length, frame_offset; int message_active, frame_active;} cm_fragments;
/* One WebSocket frame may arrive in several reads; a message may span frames.
 * Returns 1 for a complete text message, 0 for more data, -1 on any violation. */
int cm_fragment(cm_fragments *, const char *, size_t bytes, size_t frame_length, size_t frame_offset, int opcode, int fin);
typedef struct {char operation[161], name[81], input[CM_INPUT], result[256]; uint32_t sequence; uint8_t state;} cm_operation;
typedef struct {uint32_t version, accepted_sequence; char gateway_id[161]; cm_operation operations[CM_OPERATIONS];} cm_journal;
typedef struct {
  const char *name;
  const char *input_schema;
  int requires_lease;
  void *context;
  /* Return JSON result bytes, or -1. Validate every device-specific parameter. */
  int (*execute)(void *, const cm_json *, int input_token, uint32_t remaining_ms, char *result, size_t capacity);
} cm_tool;
/* Callbacks must fail on I/O errors. Store commits the complete journal atomically.
 * gpio_write(value, remaining_ms) MUST install a hardware/platform fail-safe timer. */
typedef struct {
  void *context;
  int (*load)(void *, cm_journal *);
  int (*store)(void *, const cm_journal *);
  int (*gpio_read)(void *, int *);
  int (*gpio_write)(void *, int, uint32_t);
} cm_platform;
typedef struct {cm_platform platform; cm_journal journal; cm_tool tools[CM_TOOLS]; int tool_count; char device[81], resource[81], gateway[161], lease[161]; uint32_t generation; int ready;} cm_runtime;
int cm_register_tool(cm_runtime *runtime, const cm_tool *tool);
int cm_init(cm_runtime *runtime, const cm_platform *platform, const char *device, const char *resource);
int cm_hello(const cm_runtime *runtime, const char *boot_id, char *output, size_t capacity);
/* Complete UTF-8 frames only. Returns bytes written, 0 for a handshake response,
 * -1 for invalid/oversized input. Never truncates a JSON response. */
int cm_handle(cm_runtime *runtime, const char *input, size_t length, char *output, size_t capacity);
#endif
