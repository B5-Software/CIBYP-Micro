/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cibyp_micro.h"
#include <string.h>
int cm_fragment(cm_fragments *s, const char *data, size_t n, size_t frame_length, size_t offset, int opcode, int fin) {
  if (!s || (!data && n) || (opcode != 0 && opcode != 1) || frame_length > CM_FRAME || offset > frame_length || n > frame_length - offset) return -1;
  if (!offset) {
    if (s->frame_active || (opcode == 1 && s->message_active) || (opcode == 0 && !s->message_active)) goto invalid;
    if (opcode == 1) s->length = 0;
    s->message_active = s->frame_active = 1; s->frame_length = frame_length; s->frame_offset = 0;
  }
  if (!s->frame_active || frame_length != s->frame_length || offset != s->frame_offset || n > CM_FRAME - s->length) goto invalid;
  if (n) memcpy(s->bytes + s->length, data, n);
  s->length += n; s->frame_offset += n;
  if (s->frame_offset < frame_length) return 0;
  s->frame_active = 0;
  if (!fin) return 0;
  s->message_active = 0; return 1;
invalid:
  memset(s, 0, sizeof *s); return -1;
}
