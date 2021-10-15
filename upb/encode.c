/*
 * Copyright (c) 2009-2021, Google LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Google LLC nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Google LLC BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* We encode backwards, to avoid pre-computing lengths (one-pass encode). */

#include "upb/encode.h"

#include <setjmp.h>
#include <string.h>

#include "upb/msg_internal.h"
#include "upb/upb.h"

/* Must be last. */
#include "upb/port_def.inc"

#define UPB_PB_VARINT_MAX_LEN 10

static char *encode_varint64(uint64_t val, char *buf)
{
  while (val > 127)
  {
    *buf++ = val | 0x80;
    val >>= 7;
  }
  *buf++ = val;
  return buf;
}

static uint32_t encode_zz32(int32_t n) { return ((uint32_t)n << 1) ^ (n >> 31); }
static uint64_t encode_zz64(int64_t n) { return ((uint64_t)n << 1) ^ (n >> 63); }

typedef struct
{
  jmp_buf err;
  upb_alloc *alloc;
  char *buf, *limit;
  int options;
  int depth;
  _upb_mapsorter sorter;
} upb_context;

typedef struct
{
  char *ptr;
  upb_context *ctx;
} upb_encstate;

static size_t upb_roundup_pow2(size_t bytes)
{
  size_t ret = 128;
  while (ret < bytes)
  {
    ret *= 2;
  }
  return ret;
}

UPB_FORCEINLINE
UPB_NORETURN static void encode_err(upb_encstate *e)
{
  UPB_LONGJMP(e->ctx->err, 1);
}

UPB_NOINLINE
static char *encode_growbuffer(upb_encstate e2, size_t bytes)
{
  char *ptr = e2.ptr;
  upb_context *e = e2.ctx;
  size_t cur_size = e->limit - ptr;
  size_t new_size = upb_roundup_pow2(bytes + cur_size);
  char *new_buf = upb_malloc(e->alloc, new_size);

  if (!new_buf)
    encode_err(&e2);

  /* We want previous data at the end, realloc() put it at the beginning. */
  if (cur_size > 0)
  {
    memcpy(new_buf + new_size - cur_size, ptr, cur_size);
  }

  ptr = new_buf + new_size - cur_size;
  e->limit = new_buf + new_size;
  e->buf = new_buf;

  return ptr - bytes;
}

/* Call to ensure that at least "bytes" bytes are available for writing at
 * e->ptr.  Returns false if the bytes could not be allocated. */
UPB_FORCEINLINE
static void encode_reserve(upb_encstate *e, size_t bytes)
{
  if ((size_t)(e->ptr - e->ctx->buf) < bytes)
  {
    e->ptr = encode_growbuffer(*e, bytes);
    return;
  }

  e->ptr -= bytes;
}

/* Writes the given bytes to the buffer, handling reserve/advance. */
UPB_FORCEINLINE
static void encode_bytes(upb_encstate *e, const void *data, size_t len)
{
  if (len == 0)
    return; /* memcpy() with zero size is UB */
  encode_reserve(e, len);
  memcpy(e->ptr, data, len);
}

UPB_FORCEINLINE
static void encode_fixed64(upb_encstate *e, uint64_t val)
{
  val = _upb_be_swap64(val);
  encode_bytes(e, &val, sizeof(uint64_t));
}

UPB_FORCEINLINE
static void encode_fixed32(upb_encstate *e, uint32_t val)
{
  val = _upb_be_swap32(val);
  encode_bytes(e, &val, sizeof(uint32_t));
}

UPB_NOINLINE
static char *encode_longvarint(upb_encstate e, uint64_t val)
{
  char buffer[32];

  char *start = buffer + 16;
  char *end = encode_varint64(val, start);

  encode_reserve(&e, 16);
  memcpy(e.ptr, end - 16, 16);
  return e.ptr + 16 - (end - start);
}

UPB_FORCEINLINE
static void encode_varint(upb_encstate *e, uint64_t val)
{
  if (val < 128 && e->ptr != e->ctx->buf)
  {
    --e->ptr;
    *e->ptr = val;
  }
  else
  {
    e->ptr = encode_longvarint(*e, val);
  }
}

UPB_FORCEINLINE
static void encode_double(upb_encstate *e, double d)
{
  uint64_t u64;
  UPB_ASSERT(sizeof(double) == sizeof(uint64_t));
  memcpy(&u64, &d, sizeof(uint64_t));
  encode_fixed64(e, u64);
}

UPB_FORCEINLINE
static void encode_float(upb_encstate *e, float d)
{
  uint32_t u32;
  UPB_ASSERT(sizeof(float) == sizeof(uint32_t));
  memcpy(&u32, &d, sizeof(uint32_t));
  encode_fixed32(e, u32);
}

UPB_FORCEINLINE
static void encode_tag(upb_encstate *e, uint32_t field_number,
                       uint8_t wire_type)
{
  encode_varint(e, (field_number << 3) | wire_type);
}

static char *encode_fixedarray_impl(upb_encstate e, const upb_array *arr,
                                    size_t elem_size, uint32_t tag)
{
  // ENDIAN??
  size_t bytes = arr->len * elem_size;
  const char *data = _upb_array_constptr(arr);
  const char *ptr = data + bytes - elem_size;
  if (tag)
  {
    while (true)
    {
      encode_bytes(&e, ptr, elem_size);
      encode_varint(&e, tag);
      if (ptr == data)
        break;
      ptr -= elem_size;
    }
  }
  else
  {
    encode_bytes(&e, data, bytes);
  }
  return e.ptr;
}

UPB_FORCEINLINE
static void encode_fixedarray(upb_encstate *e, const upb_array *arr,
                              size_t elem_size, uint32_t tag)
{
  e->ptr = encode_fixedarray_impl(*e, arr, elem_size, tag);
}

static char *encode_message_impl(upb_encstate e, const upb_msg *msg,
                                 const upb_msglayout *m, size_t *size);

UPB_FORCEINLINE
static void encode_message(upb_encstate *e, const upb_msg *msg,
                           const upb_msglayout *m, size_t *size)
{
  e->ptr = encode_message_impl(*e, msg, m, size);
}

static char *encode_scalar_impl(upb_encstate e, const void *_field_mem,
                                const upb_msglayout_sub *subs,
                                const upb_msglayout_field *f)
{
  const char *field_mem = _field_mem;
  int wire_type;

#define CASE(ctype, type, wtype, encodeval) \
  {                                         \
    ctype val = *(ctype *)field_mem;        \
    encode_##type(&e, encodeval);           \
    wire_type = wtype;                      \
    break;                                  \
  }

  switch (f->descriptortype)
  {
  case UPB_DESCRIPTOR_TYPE_DOUBLE:
    CASE(double, double, UPB_WIRE_TYPE_64BIT, val);
  case UPB_DESCRIPTOR_TYPE_FLOAT:
    CASE(float, float, UPB_WIRE_TYPE_32BIT, val);
  case UPB_DESCRIPTOR_TYPE_INT64:
  case UPB_DESCRIPTOR_TYPE_UINT64:
    CASE(uint64_t, varint, UPB_WIRE_TYPE_VARINT, val);
  case UPB_DESCRIPTOR_TYPE_UINT32:
    CASE(uint32_t, varint, UPB_WIRE_TYPE_VARINT, val);
  case UPB_DESCRIPTOR_TYPE_INT32:
  case UPB_DESCRIPTOR_TYPE_ENUM:
    CASE(int32_t, varint, UPB_WIRE_TYPE_VARINT, (int64_t)val);
  case UPB_DESCRIPTOR_TYPE_SFIXED64:
  case UPB_DESCRIPTOR_TYPE_FIXED64:
    CASE(uint64_t, fixed64, UPB_WIRE_TYPE_64BIT, val);
  case UPB_DESCRIPTOR_TYPE_FIXED32:
  case UPB_DESCRIPTOR_TYPE_SFIXED32:
    CASE(uint32_t, fixed32, UPB_WIRE_TYPE_32BIT, val);
  case UPB_DESCRIPTOR_TYPE_BOOL:
    CASE(bool, varint, UPB_WIRE_TYPE_VARINT, val);
  case UPB_DESCRIPTOR_TYPE_SINT32:
    CASE(int32_t, varint, UPB_WIRE_TYPE_VARINT, encode_zz32(val));
  case UPB_DESCRIPTOR_TYPE_SINT64:
    CASE(int64_t, varint, UPB_WIRE_TYPE_VARINT, encode_zz64(val));
  case UPB_DESCRIPTOR_TYPE_STRING:
  case UPB_DESCRIPTOR_TYPE_BYTES:
  {
    upb_strview view = *(upb_strview *)field_mem;
    encode_bytes(&e, view.data, view.size);
    encode_varint(&e, view.size);
    wire_type = UPB_WIRE_TYPE_DELIMITED;
    break;
  }
  case UPB_DESCRIPTOR_TYPE_GROUP:
  {
    size_t size;
    void *submsg = *(void **)field_mem;
    const upb_msglayout *subm = subs[f->submsg_index].submsg;
    if (submsg == NULL)
    {
      return e.ptr;
    }
    if (--e.ctx->depth == 0)
      encode_err(&e);
    encode_tag(&e, f->number, UPB_WIRE_TYPE_END_GROUP);
    encode_message(&e, submsg, subm, &size);
    wire_type = UPB_WIRE_TYPE_START_GROUP;
    e.ctx->depth++;
    break;
  }
  case UPB_DESCRIPTOR_TYPE_MESSAGE:
  {
    size_t size;
    void *submsg = *(void **)field_mem;
    const upb_msglayout *subm = subs[f->submsg_index].submsg;
    if (submsg == NULL)
    {
      return e.ptr;
    }
    if (--e.ctx->depth == 0)
      encode_err(&e);
    encode_message(&e, submsg, subm, &size);
    encode_varint(&e, size);
    wire_type = UPB_WIRE_TYPE_DELIMITED;
    e.ctx->depth++;
    break;
  }
  default:
    UPB_UNREACHABLE();
  }
#undef CASE

  encode_tag(&e, f->number, wire_type);
  return e.ptr;
}

UPB_FORCEINLINE
static void encode_scalar(upb_encstate *e, const void *_field_mem,
                          const upb_msglayout_sub *subs,
                          const upb_msglayout_field *f)
{
  e->ptr = encode_scalar_impl(*e, _field_mem, subs, f);
}

static char *encode_array_impl(upb_encstate e, const upb_msg *msg,
                               const upb_msglayout_sub *subs,
                               const upb_msglayout_field *f)
{
  const upb_array *arr = *UPB_PTR_AT(msg, f->offset, upb_array *);
  bool packed = f->mode & _UPB_MODE_IS_PACKED;
  size_t pre_len = e.ctx->limit - e.ptr;

  if (arr == NULL || arr->len == 0)
  {
    return e.ptr;
  }

#define VARINT_CASE(ctype, encode)                                       \
  {                                                                      \
    const ctype *start = _upb_array_constptr(arr);                       \
    const ctype *ptr = start + arr->len;                                 \
    uint32_t tag = packed ? 0 : (f->number << 3) | UPB_WIRE_TYPE_VARINT; \
    do                                                                   \
    {                                                                    \
      ptr--;                                                             \
      encode_varint(&e, encode);                                         \
      if (tag)                                                           \
        encode_varint(&e, tag);                                          \
    } while (ptr != start);                                              \
  }                                                                      \
  break;

#define TAG(wire_type) (packed ? 0 : (f->number << 3 | wire_type))

  switch (f->descriptortype)
  {
  case UPB_DESCRIPTOR_TYPE_DOUBLE:
    encode_fixedarray(&e, arr, sizeof(double), TAG(UPB_WIRE_TYPE_64BIT));
    break;
  case UPB_DESCRIPTOR_TYPE_FLOAT:
    encode_fixedarray(&e, arr, sizeof(float), TAG(UPB_WIRE_TYPE_32BIT));
    break;
  case UPB_DESCRIPTOR_TYPE_SFIXED64:
  case UPB_DESCRIPTOR_TYPE_FIXED64:
    encode_fixedarray(&e, arr, sizeof(uint64_t), TAG(UPB_WIRE_TYPE_64BIT));
    break;
  case UPB_DESCRIPTOR_TYPE_FIXED32:
  case UPB_DESCRIPTOR_TYPE_SFIXED32:
    encode_fixedarray(&e, arr, sizeof(uint32_t), TAG(UPB_WIRE_TYPE_32BIT));
    break;
  case UPB_DESCRIPTOR_TYPE_INT64:
  case UPB_DESCRIPTOR_TYPE_UINT64:
    VARINT_CASE(uint64_t, *ptr);
  case UPB_DESCRIPTOR_TYPE_UINT32:
    VARINT_CASE(uint32_t, *ptr);
  case UPB_DESCRIPTOR_TYPE_INT32:
  case UPB_DESCRIPTOR_TYPE_ENUM:
    VARINT_CASE(int32_t, (int64_t)*ptr);
  case UPB_DESCRIPTOR_TYPE_BOOL:
    VARINT_CASE(bool, *ptr);
  case UPB_DESCRIPTOR_TYPE_SINT32:
    VARINT_CASE(int32_t, encode_zz32(*ptr));
  case UPB_DESCRIPTOR_TYPE_SINT64:
    VARINT_CASE(int64_t, encode_zz64(*ptr));
  case UPB_DESCRIPTOR_TYPE_STRING:
  case UPB_DESCRIPTOR_TYPE_BYTES:
  {
    const upb_strview *start = _upb_array_constptr(arr);
    const upb_strview *ptr = start + arr->len;
    do
    {
      ptr--;
      encode_bytes(&e, ptr->data, ptr->size);
      encode_varint(&e, ptr->size);
      encode_tag(&e, f->number, UPB_WIRE_TYPE_DELIMITED);
    } while (ptr != start);
    return e.ptr;
  }
  case UPB_DESCRIPTOR_TYPE_GROUP:
  {
    const void *const *start = _upb_array_constptr(arr);
    const void *const *ptr = start + arr->len;
    const upb_msglayout *subm = subs[f->submsg_index].submsg;
    if (--e.ctx->depth == 0)
      encode_err(&e);
    do
    {
      size_t size;
      ptr--;
      encode_tag(&e, f->number, UPB_WIRE_TYPE_END_GROUP);
      encode_message(&e, *ptr, subm, &size);
      encode_tag(&e, f->number, UPB_WIRE_TYPE_START_GROUP);
    } while (ptr != start);
    e.ctx->depth++;
    return e.ptr;
  }
  case UPB_DESCRIPTOR_TYPE_MESSAGE:
  {
    const void *const *start = _upb_array_constptr(arr);
    const void *const *ptr = start + arr->len;
    const upb_msglayout *subm = subs[f->submsg_index].submsg;
    if (--e.ctx->depth == 0)
      encode_err(&e);
    do
    {
      size_t size;
      ptr--;
      encode_message(&e, *ptr, subm, &size);
      encode_varint(&e, size);
      encode_tag(&e, f->number, UPB_WIRE_TYPE_DELIMITED);
    } while (ptr != start);
    e.ctx->depth++;
    return e.ptr;
  }
  }
#undef VARINT_CASE

  if (packed)
  {
    encode_varint(&e, e.ctx->limit - e.ptr - pre_len);
    encode_tag(&e, f->number, UPB_WIRE_TYPE_DELIMITED);
  }
  return e.ptr;
}

UPB_FORCEINLINE
static void encode_array(upb_encstate *e, const upb_msg *msg,
                         const upb_msglayout_sub *subs,
                         const upb_msglayout_field *f)
{
  e->ptr = encode_array_impl(*e, msg, subs, f);
}

static char *encode_mapentry_impl(upb_encstate e, uint32_t number,
                                  const upb_msglayout *layout,
                                  const upb_map_entry *ent)
{
  const upb_msglayout_field *key_field = &layout->fields[0];
  const upb_msglayout_field *val_field = &layout->fields[1];
  size_t pre_len = e.ctx->limit - e.ptr;
  size_t size;
  encode_scalar(&e, &ent->v, layout->subs, val_field);
  encode_scalar(&e, &ent->k, layout->subs, key_field);
  size = (e.ctx->limit - e.ptr) - pre_len;
  encode_varint(&e, size);
  encode_tag(&e, number, UPB_WIRE_TYPE_DELIMITED);
  return e.ptr;
}

UPB_FORCEINLINE
static void encode_mapentry(upb_encstate *e, uint32_t number,
                            const upb_msglayout *layout,
                            const upb_map_entry *ent)
{
  e->ptr = encode_mapentry_impl(*e, number, layout, ent);
}

static char *encode_map_impl(upb_encstate e, const upb_msg *msg,
                             const upb_msglayout_sub *subs,
                             const upb_msglayout_field *f)
{
  const upb_map *map = *UPB_PTR_AT(msg, f->offset, const upb_map *);
  const upb_msglayout *layout = subs[f->submsg_index].submsg;
  UPB_ASSERT(layout->field_count == 2);

  if (map == NULL)
    return e.ptr;

  if (e.ctx->options & UPB_ENCODE_DETERMINISTIC)
  {
    _upb_sortedmap sorted;
    _upb_mapsorter_pushmap(&e.ctx->sorter, layout->fields[0].descriptortype, map,
                           &sorted);
    upb_map_entry ent;
    while (_upb_sortedmap_next(&e.ctx->sorter, map, &sorted, &ent))
    {
      encode_mapentry(&e, f->number, layout, &ent);
    }
    _upb_mapsorter_popmap(&e.ctx->sorter, &sorted);
  }
  else
  {
    upb_strtable_iter i;
    upb_strtable_begin(&i, &map->table);
    for (; !upb_strtable_done(&i); upb_strtable_next(&i))
    {
      upb_strview key = upb_strtable_iter_key(&i);
      const upb_value val = upb_strtable_iter_value(&i);
      upb_map_entry ent;
      _upb_map_fromkey(key, &ent.k, map->key_size);
      _upb_map_fromvalue(val, &ent.v, map->val_size);
      encode_mapentry(&e, f->number, layout, &ent);
    }
  }
  return e.ptr;
}

UPB_FORCEINLINE
static void encode_map(upb_encstate *e, const upb_msg *msg,
                       const upb_msglayout_sub *subs,
                       const upb_msglayout_field *f)
{
  e->ptr = encode_map_impl(*e, msg, subs, f);
}

static bool encode_shouldencode(const upb_msg *msg,
                                const upb_msglayout_sub *subs,
                                const upb_msglayout_field *f)
{
  if (f->presence == 0)
  {
    /* Proto3 presence or map/array. */
    const void *mem = UPB_PTR_AT(msg, f->offset, void);
    switch (f->mode >> _UPB_REP_SHIFT)
    {
    case _UPB_REP_1BYTE:
    {
      char ch;
      memcpy(&ch, mem, 1);
      return ch != 0;
    }
    case _UPB_REP_4BYTE:
    {
      uint32_t u32;
      memcpy(&u32, mem, 4);
      return u32 != 0;
    }
    case _UPB_REP_8BYTE:
    {
      uint64_t u64;
      memcpy(&u64, mem, 8);
      return u64 != 0;
    }
    case _UPB_REP_STRVIEW:
    {
      const upb_strview *str = (const upb_strview *)mem;
      return str->size != 0;
    }
    default:
      UPB_UNREACHABLE();
    }
  }
  else if (f->presence > 0)
  {
    /* Proto2 presence: hasbit. */
    return _upb_hasbit_field(msg, f);
  }
  else
  {
    /* Field is in a oneof. */
    return _upb_getoneofcase_field(msg, f) == f->number;
  }
}

static char *encode_field_impl(upb_encstate e, const upb_msg *msg,
                               const upb_msglayout_sub *subs,
                               const upb_msglayout_field *field)
{
  switch (_upb_getmode(field))
  {
  case _UPB_MODE_ARRAY:
    encode_array(&e, msg, subs, field);
    break;
  case _UPB_MODE_MAP:
    encode_map(&e, msg, subs, field);
    break;
  case _UPB_MODE_SCALAR:
    encode_scalar(&e, UPB_PTR_AT(msg, field->offset, void), subs, field);
    break;
  default:
    UPB_UNREACHABLE();
  }
  return e.ptr;
}

UPB_FORCEINLINE
static void encode_field(upb_encstate *e, const upb_msg *msg,
                         const upb_msglayout_sub *subs,
                         const upb_msglayout_field *field)
{
  e->ptr = encode_field_impl(*e, msg, subs, field);
}

/* message MessageSet {
 *   repeated group Item = 1 {
 *     required int32 type_id = 2;
 *     required string message = 3;
 *   }
 * } */
UPB_FORCEINLINE
static void encode_msgset_item(upb_encstate *e, const upb_msg_ext *ext)
{
  size_t size;
  encode_tag(e, 1, UPB_WIRE_TYPE_END_GROUP);
  encode_message(e, ext->data.ptr, ext->ext->sub.submsg, &size);
  encode_varint(e, size);
  encode_tag(e, 3, UPB_WIRE_TYPE_DELIMITED);
  encode_varint(e, ext->ext->field.number);
  encode_tag(e, 2, UPB_WIRE_TYPE_VARINT);
  encode_tag(e, 1, UPB_WIRE_TYPE_START_GROUP);
}

static char *encode_message_impl(upb_encstate e, const upb_msg *msg,
                                 const upb_msglayout *m, size_t *size)
{
  size_t pre_len = e.ctx->limit - e.ptr;

  if ((e.ctx->options & UPB_ENCODE_SKIPUNKNOWN) == 0)
  {
    size_t unknown_size;
    const char *unknown = upb_msg_getunknown(msg, &unknown_size);

    if (unknown)
    {
      encode_bytes(&e, unknown, unknown_size);
    }
  }

  if (m->ext != _UPB_MSGEXT_NONE)
  {
    /* Encode all extensions together. Unlike C++, we do not attempt to keep
     * these in field number order relative to normal fields or even to each
     * other. */
    size_t ext_count;
    const upb_msg_ext *ext = _upb_msg_getexts(msg, &ext_count);
    const upb_msg_ext *end = ext + ext_count;
    if (ext_count)
    {
      for (; ext != end; ext++)
      {
        if (UPB_UNLIKELY(m->ext == _UPB_MSGEXT_MSGSET))
        {
          encode_msgset_item(&e, ext);
        }
        else
        {
          encode_field(&e, &ext->data, &ext->ext->sub, &ext->ext->field);
        }
      }
    }
  }

  const upb_msglayout_field *f = &m->fields[m->field_count];
  const upb_msglayout_field *first = &m->fields[0];
  while (f != first)
  {
    f--;
    if (encode_shouldencode(msg, m->subs, f))
    {
      encode_field(&e, msg, m->subs, f);
    }
  }

  *size = (e.ctx->limit - e.ptr) - pre_len;
  return e.ptr;
}

char *upb_encode_ex(const void *msg, const upb_msglayout *l, int options,
                    upb_arena *arena, size_t *size)
{
  upb_context ctx;
  unsigned depth = (unsigned)options >> 16;

  ctx.alloc = upb_arena_alloc(arena);
  ctx.buf = NULL;
  ctx.limit = NULL;
  ctx.depth = depth ? depth : 64;
  ctx.options = options;
  _upb_mapsorter_init(&ctx.sorter);
  char *ret = NULL;

  upb_encstate e = {NULL, &ctx};
  if (UPB_SETJMP(ctx.err))
  {
    *size = 0;
    ret = NULL;
  }
  else
  {
    encode_message(&e, msg, l, size);
    *size = ctx.limit - e.ptr;
    if (*size == 0)
    {
      static char ch;
      ret = &ch;
    }
    else
    {
      UPB_ASSERT(e.ptr);
      ret = e.ptr;
    }
  }

  _upb_mapsorter_destroy(&ctx.sorter);
  return ret;
}
