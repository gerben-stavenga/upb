// Copyright (c) 2009-2021, Google LLC
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of Google LLC nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL Google LLC BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <memory>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/ascii.h"
#include "absl/strings/substitute.h"
#include "google/protobuf/compiler/code_generator.h"
#include "google/protobuf/compiler/plugin.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/wire_format.h"
#include "upbc/common.h"
#include "upbc/message_layout.h"

namespace upbc {
namespace {

namespace protoc = ::google::protobuf::compiler;
namespace protobuf = ::google::protobuf;

std::string SourceFilename(const google::protobuf::FileDescriptor* file) {
  return StripExtension(file->name()) + ".upb.c";
}

std::string MessageInit(const protobuf::Descriptor* descriptor) {
  return MessageName(descriptor) + "_msginit";
}

std::string ExtensionIdentBase(const protobuf::FieldDescriptor* ext) {
  assert(ext->is_extension());
  std::string ext_scope;
  if (ext->extension_scope()) {
    return MessageName(ext->extension_scope());
  } else {
    return ToCIdent(ext->file()->package());
  }
}

std::string ExtensionLayout(const google::protobuf::FieldDescriptor* ext) {
  return absl::StrCat(ExtensionIdentBase(ext), "_", ext->name(), "_ext");
}

const char *kMessagesInit = "messages_layout";
const char *kExtensionsInit = "extensions_layout";

void AddEnums(const protobuf::Descriptor* message,
              std::vector<const protobuf::EnumDescriptor*>* enums) {
  for (int i = 0; i < message->enum_type_count(); i++) {
    enums->push_back(message->enum_type(i));
  }
  for (int i = 0; i < message->nested_type_count(); i++) {
    AddEnums(message->nested_type(i), enums);
  }
}

template <class T>
void SortDefs(std::vector<T>* defs) {
  std::sort(defs->begin(), defs->end(),
            [](T a, T b) { return a->full_name() < b->full_name(); });
}

std::vector<const protobuf::EnumDescriptor*> SortedEnums(
    const protobuf::FileDescriptor* file) {
  std::vector<const protobuf::EnumDescriptor*> enums;
  for (int i = 0; i < file->enum_type_count(); i++) {
    enums.push_back(file->enum_type(i));
  }
  for (int i = 0; i < file->message_type_count(); i++) {
    AddEnums(file->message_type(i), &enums);
  }
  SortDefs(&enums);
  return enums;
}

void AddMessages(const protobuf::Descriptor* message,
                 std::vector<const protobuf::Descriptor*>* messages) {
  messages->push_back(message);
  for (int i = 0; i < message->nested_type_count(); i++) {
    AddMessages(message->nested_type(i), messages);
  }
}

// Ordering must match upb/def.c!
//
// The ordering is significant because each upb_msgdef* will point at the
// corresponding upb_msglayout and we just iterate through the list without
// any search or lookup.
std::vector<const protobuf::Descriptor*> SortedMessages(
    const protobuf::FileDescriptor* file) {
  std::vector<const protobuf::Descriptor*> messages;
  for (int i = 0; i < file->message_type_count(); i++) {
    AddMessages(file->message_type(i), &messages);
  }
  return messages;
}

void AddExtensionsFromMessage(
    const protobuf::Descriptor* message,
    std::vector<const protobuf::FieldDescriptor*>* exts) {
  for (int i = 0; i < message->extension_count(); i++) {
    exts->push_back(message->extension(i));
  }
  for (int i = 0; i < message->nested_type_count(); i++) {
    AddExtensionsFromMessage(message->nested_type(i), exts);
  }
}

// Ordering must match upb/def.c!
//
// The ordering is significant because each upb_fielddef* will point at the
// corresponding upb_msglayout_ext and we just iterate through the list without
// any search or lookup.
std::vector<const protobuf::FieldDescriptor*> SortedExtensions(
    const protobuf::FileDescriptor* file) {
  std::vector<const protobuf::FieldDescriptor*> ret;
  for (int i = 0; i < file->extension_count(); i++) {
    ret.push_back(file->extension(i));
  }
  for (int i = 0; i < file->message_type_count(); i++) {
    AddExtensionsFromMessage(file->message_type(i), &ret);
  }
  return ret;
}

std::vector<const protobuf::FieldDescriptor*> FieldNumberOrder(
    const protobuf::Descriptor* message) {
  std::vector<const protobuf::FieldDescriptor*> fields;
  for (int i = 0; i < message->field_count(); i++) {
    fields.push_back(message->field(i));
  }
  std::sort(fields.begin(), fields.end(),
            [](const protobuf::FieldDescriptor* a,
               const protobuf::FieldDescriptor* b) {
              return a->number() < b->number();
            });
  return fields;
}

std::vector<const protobuf::FieldDescriptor*> SortedSubmessages(
    const protobuf::Descriptor* message) {
  std::vector<const protobuf::FieldDescriptor*> ret;
  for (int i = 0; i < message->field_count(); i++) {
    if (message->field(i)->cpp_type() ==
        protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      ret.push_back(message->field(i));
    }
  }
  std::sort(ret.begin(), ret.end(),
            [](const protobuf::FieldDescriptor* a,
               const protobuf::FieldDescriptor* b) {
              return a->message_type()->full_name() <
                     b->message_type()->full_name();
            });
  return ret;
}

std::string EnumValueSymbol(const protobuf::EnumValueDescriptor* value) {
  return ToCIdent(value->full_name());
}

std::string GetSizeInit(const MessageLayout::Size& size) {
  return absl::Substitute("UPB_SIZE($0, $1)", size.size32, size.size64);
}

std::string CTypeInternal(const protobuf::FieldDescriptor* field,
                          bool is_const) {
  std::string maybe_const = is_const ? "const " : "";
  switch (field->cpp_type()) {
    case protobuf::FieldDescriptor::CPPTYPE_MESSAGE: {
      std::string maybe_struct =
          field->file() != field->message_type()->file() ? "struct " : "";
      return maybe_const + maybe_struct + MessageName(field->message_type()) +
             "*";
    }
    case protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return "bool";
    case protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      return "float";
    case protobuf::FieldDescriptor::CPPTYPE_INT32:
    case protobuf::FieldDescriptor::CPPTYPE_ENUM:
      return "int32_t";
    case protobuf::FieldDescriptor::CPPTYPE_UINT32:
      return "uint32_t";
    case protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
      return "double";
    case protobuf::FieldDescriptor::CPPTYPE_INT64:
      return "int64_t";
    case protobuf::FieldDescriptor::CPPTYPE_UINT64:
      return "uint64_t";
    case protobuf::FieldDescriptor::CPPTYPE_STRING:
      return "upb_strview";
    default:
      fprintf(stderr, "Unexpected type");
      abort();
  }
}

std::string SizeLg2(const protobuf::FieldDescriptor* field) {
  switch (field->cpp_type()) {
    case protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
      return "UPB_SIZE(2, 3)";
    case protobuf::FieldDescriptor::CPPTYPE_ENUM:
      return std::to_string(2);
    case protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return std::to_string(1);
    case protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      return std::to_string(2);
    case protobuf::FieldDescriptor::CPPTYPE_INT32:
      return std::to_string(2);
    case protobuf::FieldDescriptor::CPPTYPE_UINT32:
      return std::to_string(2);
    case protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
      return std::to_string(3);
    case protobuf::FieldDescriptor::CPPTYPE_INT64:
      return std::to_string(3);
    case protobuf::FieldDescriptor::CPPTYPE_UINT64:
      return std::to_string(3);
    case protobuf::FieldDescriptor::CPPTYPE_STRING:
      return "UPB_SIZE(3, 4)";
    default:
      fprintf(stderr, "Unexpected type");
      abort();
  }
}

std::string SizeRep(const protobuf::FieldDescriptor* field) {
  switch (field->cpp_type()) {
    case protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
      return "_UPB_REP_PTR";
    case protobuf::FieldDescriptor::CPPTYPE_ENUM:
    case protobuf::FieldDescriptor::CPPTYPE_FLOAT:
    case protobuf::FieldDescriptor::CPPTYPE_INT32:
    case protobuf::FieldDescriptor::CPPTYPE_UINT32:
      return "_UPB_REP_4BYTE";
    case protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return "_UPB_REP_1BYTE";
    case protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
    case protobuf::FieldDescriptor::CPPTYPE_INT64:
    case protobuf::FieldDescriptor::CPPTYPE_UINT64:
      return "_UPB_REP_8BYTE";
    case protobuf::FieldDescriptor::CPPTYPE_STRING:
      return "_UPB_REP_STRVIEW";
    default:
      fprintf(stderr, "Unexpected type");
      abort();
  }
}

std::string FieldDefault(const protobuf::FieldDescriptor* field) {
  switch (field->cpp_type()) {
    case protobuf::FieldDescriptor::CPPTYPE_MESSAGE:
      return "NULL";
    case protobuf::FieldDescriptor::CPPTYPE_STRING:
      return absl::Substitute("upb_strview_make(\"$0\", strlen(\"$0\"))",
                              absl::CEscape(field->default_value_string()));
    case protobuf::FieldDescriptor::CPPTYPE_INT32:
      return absl::StrCat(field->default_value_int32());
    case protobuf::FieldDescriptor::CPPTYPE_INT64:
      return absl::StrCat(field->default_value_int64());
    case protobuf::FieldDescriptor::CPPTYPE_UINT32:
      return absl::StrCat(field->default_value_uint32());
    case protobuf::FieldDescriptor::CPPTYPE_UINT64:
      return absl::StrCat(field->default_value_uint64());
    case protobuf::FieldDescriptor::CPPTYPE_FLOAT:
      return absl::StrCat(field->default_value_float());
    case protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
      return absl::StrCat(field->default_value_double());
    case protobuf::FieldDescriptor::CPPTYPE_BOOL:
      return field->default_value_bool() ? "true" : "false";
    case protobuf::FieldDescriptor::CPPTYPE_ENUM:
      // Use a number instead of a symbolic name so that we don't require
      // this enum's header to be included.
      return absl::StrCat(field->default_value_enum()->number());
  }
  ABSL_ASSERT(false);
  return "XXX";
}

std::string CType(const protobuf::FieldDescriptor* field) {
  return CTypeInternal(field, false);
}

std::string CTypeConst(const protobuf::FieldDescriptor* field) {
  return CTypeInternal(field, true);
}

void DumpEnumValues(const protobuf::EnumDescriptor* desc, Output& output) {
  std::vector<const protobuf::EnumValueDescriptor*> values;
  for (int i = 0; i < desc->value_count(); i++) {
    values.push_back(desc->value(i));
  }
  std::sort(values.begin(), values.end(),
            [](const protobuf::EnumValueDescriptor* a,
               const protobuf::EnumValueDescriptor* b) {
              return a->number() < b->number();
            });

  for (size_t i = 0; i < values.size(); i++) {
    auto value = values[i];
    output("  $0 = $1", EnumValueSymbol(value), value->number());
    if (i != values.size() - 1) {
      output(",");
    }
    output("\n");
  }
}

void GenerateExtensionInHeader(const protobuf::FieldDescriptor* ext,
                               Output& output) {
  output(
      "UPB_INLINE bool $0_has_$1(const struct $2 *msg) { "
      "return _upb_msg_getext(msg, &$3) != NULL; }\n",
      ExtensionIdentBase(ext), ext->name(), MessageName(ext->containing_type()),
      ExtensionLayout(ext));

  if (ext->is_repeated()) {
  } else if (ext->message_type()) {
    output(
        "UPB_INLINE $0 $1_$2(const struct $3 *msg) { "
        "const upb_msg_ext *ext = _upb_msg_getext(msg, &$4); "
        "UPB_ASSERT(ext); return *UPB_PTR_AT(&ext->data, 0, $0); }\n",
        CTypeConst(ext), ExtensionIdentBase(ext), ext->name(),
        MessageName(ext->containing_type()), ExtensionLayout(ext),
        FieldDefault(ext));
  } else {
    output(
        "UPB_INLINE $0 $1_$2(const struct $3 *msg) { "
        "const upb_msg_ext *ext = _upb_msg_getext(msg, &$4); "
        "return ext ? *UPB_PTR_AT(&ext->data, 0, $0) : $5; }\n",
        CTypeConst(ext), ExtensionIdentBase(ext), ext->name(),
        MessageName(ext->containing_type()), ExtensionLayout(ext),
        FieldDefault(ext));
  }
}

void GenerateMessageInHeader(const protobuf::Descriptor* message, Output& output) {
  MessageLayout layout(message);

  output("/* $0 */\n\n", message->full_name());
  std::string msg_name = ToCIdent(message->full_name());

  if (!message->options().map_entry()) {
    output(
        "UPB_INLINE $0 *$0_new(upb_arena *arena) {\n"
        "  return ($0 *)_upb_msg_new(&$1, arena);\n"
        "}\n"
        "UPB_INLINE $0 *$0_parse(const char *buf, size_t size,\n"
        "                        upb_arena *arena) {\n"
        "  $0 *ret = $0_new(arena);\n"
        "  if (!ret) return NULL;\n"
        "  if (!upb_decode(buf, size, ret, &$1, arena)) return NULL;\n"
        "  return ret;\n"
        "}\n"
        "UPB_INLINE $0 *$0_parse_ex(const char *buf, size_t size,\n"
        "                           const upb_extreg *extreg, int options,\n"
        "                           upb_arena *arena) {\n"
        "  $0 *ret = $0_new(arena);\n"
        "  if (!ret) return NULL;\n"
        "  if (!_upb_decode(buf, size, ret, &$1, extreg, options, arena)) {\n"
        "    return NULL;\n"
        "  }\n"
        "  return ret;\n"
        "}\n"
        "UPB_INLINE char *$0_serialize(const $0 *msg, upb_arena *arena, size_t "
        "*len) {\n"
        "  return upb_encode(msg, &$1, arena, len);\n"
        "}\n"
        "\n",
        MessageName(message), MessageInit(message));
  }

  for (int i = 0; i < message->real_oneof_decl_count(); i++) {
    const protobuf::OneofDescriptor* oneof = message->oneof_decl(i);
    std::string fullname = ToCIdent(oneof->full_name());
    output("typedef enum {\n");
    for (int j = 0; j < oneof->field_count(); j++) {
      const protobuf::FieldDescriptor* field = oneof->field(j);
      output("  $0_$1 = $2,\n", fullname, field->name(), field->number());
    }
    output(
        "  $0_NOT_SET = 0\n"
        "} $0_oneofcases;\n",
        fullname);
    output(
        "UPB_INLINE $0_oneofcases $1_$2_case(const $1* msg) { "
        "return ($0_oneofcases)*UPB_PTR_AT(msg, $3, int32_t); }\n"
        "\n",
        fullname, msg_name, oneof->name(),
        GetSizeInit(layout.GetOneofCaseOffset(oneof)));
  }

  // Generate const methods.

  for (auto field : FieldNumberOrder(message)) {
    // Generate hazzer (if any).
    if (layout.HasHasbit(field)) {
      output(
          "UPB_INLINE bool $0_has_$1(const $0 *msg) { "
          "return _upb_hasbit(msg, $2); }\n",
          msg_name, field->name(), layout.GetHasbitIndex(field));
    } else if (field->real_containing_oneof()) {
      output(
          "UPB_INLINE bool $0_has_$1(const $0 *msg) { "
          "return _upb_getoneofcase(msg, $2) == $3; }\n",
          msg_name, field->name(),
          GetSizeInit(
              layout.GetOneofCaseOffset(field->real_containing_oneof())),
          field->number());
    } else if (field->message_type()) {
      output(
          "UPB_INLINE bool $0_has_$1(const $0 *msg) { "
          "return _upb_has_submsg_nohasbit(msg, $2); }\n",
          msg_name, field->name(), GetSizeInit(layout.GetFieldOffset(field)));
    }

    // Generate getter.
    if (field->is_map()) {
      const protobuf::Descriptor* entry = field->message_type();
      const protobuf::FieldDescriptor* key = entry->FindFieldByNumber(1);
      const protobuf::FieldDescriptor* val = entry->FindFieldByNumber(2);
      output(
          "UPB_INLINE size_t $0_$1_size(const $0 *msg) {"
          "return _upb_msg_map_size(msg, $2); }\n",
          msg_name, field->name(), GetSizeInit(layout.GetFieldOffset(field)));
      output(
          "UPB_INLINE bool $0_$1_get(const $0 *msg, $2 key, $3 *val) { "
          "return _upb_msg_map_get(msg, $4, &key, $5, val, $6); }\n",
          msg_name, field->name(), CType(key), CType(val),
          GetSizeInit(layout.GetFieldOffset(field)),
          key->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_STRING
              ? "0"
              : "sizeof(key)",
          val->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_STRING
              ? "0"
              : "sizeof(*val)");
      output(
          "UPB_INLINE $0 $1_$2_next(const $1 *msg, size_t* iter) { "
          "return ($0)_upb_msg_map_next(msg, $3, iter); }\n",
          CTypeConst(field), msg_name, field->name(),
          GetSizeInit(layout.GetFieldOffset(field)));
    } else if (message->options().map_entry()) {
      output(
          "UPB_INLINE $0 $1_$2(const $1 *msg) {\n"
          "  $3 ret;\n"
          "  _upb_msg_map_$2(msg, &ret, $4);\n"
          "  return ret;\n"
          "}\n",
          CTypeConst(field), msg_name, field->name(), CType(field),
          field->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_STRING
              ? "0"
              : "sizeof(ret)");
    } else if (field->is_repeated()) {
      output(
          "UPB_INLINE $0 const* $1_$2(const $1 *msg, size_t *len) { "
          "return ($0 const*)_upb_array_accessor(msg, $3, len); }\n",
          CTypeConst(field), msg_name, field->name(),
          GetSizeInit(layout.GetFieldOffset(field)));
    } else if (field->real_containing_oneof()) {
      output(
          "UPB_INLINE $0 $1_$2(const $1 *msg) { "
          "return UPB_READ_ONEOF(msg, $0, $3, $4, $5, $6); }\n",
          CTypeConst(field), msg_name, field->name(),
          GetSizeInit(layout.GetFieldOffset(field)),
          GetSizeInit(layout.GetOneofCaseOffset(field->real_containing_oneof())),
          field->number(), FieldDefault(field));
    } else {
      output(
          "UPB_INLINE $0 $1_$2(const $1 *msg) { "
          "return *UPB_PTR_AT(msg, $3, $0); }\n",
          CTypeConst(field), msg_name, field->name(),
          GetSizeInit(layout.GetFieldOffset(field)));
    }
  }

  output("\n");

  // Generate mutable methods.

  for (auto field : FieldNumberOrder(message)) {
    if (field->is_map()) {
      // TODO(haberman): add map-based mutators.
      const protobuf::Descriptor* entry = field->message_type();
      const protobuf::FieldDescriptor* key = entry->FindFieldByNumber(1);
      const protobuf::FieldDescriptor* val = entry->FindFieldByNumber(2);
      output(
          "UPB_INLINE void $0_$1_clear($0 *msg) { _upb_msg_map_clear(msg, $2); }\n",
          msg_name, field->name(),
          GetSizeInit(layout.GetFieldOffset(field)));
      output(
          "UPB_INLINE bool $0_$1_set($0 *msg, $2 key, $3 val, upb_arena *a) { "
          "return _upb_msg_map_set(msg, $4, &key, $5, &val, $6, a); }\n",
          msg_name, field->name(), CType(key), CType(val),
          GetSizeInit(layout.GetFieldOffset(field)),
          key->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_STRING
              ? "0"
              : "sizeof(key)",
          val->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_STRING
              ? "0"
              : "sizeof(val)");
      output(
          "UPB_INLINE bool $0_$1_delete($0 *msg, $2 key) { "
          "return _upb_msg_map_delete(msg, $3, &key, $4); }\n",
          msg_name, field->name(), CType(key),
          GetSizeInit(layout.GetFieldOffset(field)),
          key->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_STRING
              ? "0"
              : "sizeof(key)");
      output(
          "UPB_INLINE $0 $1_$2_nextmutable($1 *msg, size_t* iter) { "
          "return ($0)_upb_msg_map_next(msg, $3, iter); }\n",
          CType(field), msg_name, field->name(),
          GetSizeInit(layout.GetFieldOffset(field)));
    } else if (field->is_repeated()) {
      output(
          "UPB_INLINE $0* $1_mutable_$2($1 *msg, size_t *len) {\n"
          "  return ($0*)_upb_array_mutable_accessor(msg, $3, len);\n"
          "}\n",
          CType(field), msg_name, field->name(),
          GetSizeInit(layout.GetFieldOffset(field)));
      output(
          "UPB_INLINE $0* $1_resize_$2($1 *msg, size_t len, "
          "upb_arena *arena) {\n"
          "  return ($0*)_upb_array_resize_accessor2(msg, $3, len, $4, arena);\n"
          "}\n",
          CType(field), msg_name, field->name(),
          GetSizeInit(layout.GetFieldOffset(field)),
          SizeLg2(field));
      if (field->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        output(
            "UPB_INLINE struct $0* $1_add_$2($1 *msg, upb_arena *arena) {\n"
            "  struct $0* sub = (struct $0*)_upb_msg_new(&$3, arena);\n"
            "  bool ok = _upb_array_append_accessor2(\n"
            "      msg, $4, $5, &sub, arena);\n"
            "  if (!ok) return NULL;\n"
            "  return sub;\n"
            "}\n",
            MessageName(field->message_type()), msg_name, field->name(),
            MessageInit(field->message_type()),
            GetSizeInit(layout.GetFieldOffset(field)),
            SizeLg2(field));
      } else {
        output(
            "UPB_INLINE bool $1_add_$2($1 *msg, $0 val, upb_arena *arena) {\n"
            "  return _upb_array_append_accessor2(msg, $3, $4, &val,\n"
            "      arena);\n"
            "}\n",
            CType(field), msg_name, field->name(),
            GetSizeInit(layout.GetFieldOffset(field)),
            SizeLg2(field));
      }
    } else {
      // Non-repeated field.
      if (message->options().map_entry() && field->name() == "key") {
        // Key cannot be mutated.
        continue;
      }

      // The common function signature for all setters.  Varying implementations
      // follow.
      output("UPB_INLINE void $0_set_$1($0 *msg, $2 value) {\n", msg_name,
             field->name(), CType(field));

      if (message->options().map_entry()) {
        output(
            "  _upb_msg_map_set_value(msg, &value, $0);\n"
            "}\n",
            field->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_STRING
                ? "0"
                : "sizeof(" + CType(field) + ")");
      } else if (field->real_containing_oneof()) {
        output(
            "  UPB_WRITE_ONEOF(msg, $0, $1, value, $2, $3);\n"
            "}\n",
            CType(field), GetSizeInit(layout.GetFieldOffset(field)),
            GetSizeInit(
                layout.GetOneofCaseOffset(field->real_containing_oneof())),
            field->number());
      } else {
        if (MessageLayout::HasHasbit(field)) {
          output("  _upb_sethas(msg, $0);\n", layout.GetHasbitIndex(field));
        }
        output(
            "  *UPB_PTR_AT(msg, $1, $0) = value;\n"
            "}\n",
            CType(field), GetSizeInit(layout.GetFieldOffset(field)));
      }

      if (field->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
          !message->options().map_entry()) {
        output(
            "UPB_INLINE struct $0* $1_mutable_$2($1 *msg, upb_arena *arena) {\n"
            "  struct $0* sub = (struct $0*)$1_$2(msg);\n"
            "  if (sub == NULL) {\n"
            "    sub = (struct $0*)_upb_msg_new(&$3, arena);\n"
            "    if (!sub) return NULL;\n"
            "    $1_set_$2(msg, sub);\n"
            "  }\n"
            "  return sub;\n"
            "}\n",
            MessageName(field->message_type()), msg_name, field->name(),
            MessageInit(field->message_type()));
      }
    }
  }

  output("\n");
}

void WriteHeader(const protobuf::FileDescriptor* file, Output& output) {
  EmitFileWarning(file, output);
  output(
      "#ifndef $0_UPB_H_\n"
      "#define $0_UPB_H_\n\n"
      "#include \"upb/msg_internal.h\"\n"
      "#include \"upb/decode.h\"\n"
      "#include \"upb/decode_fast.h\"\n"
      "#include \"upb/encode.h\"\n\n",
      ToPreproc(file->name()));

  for (int i = 0; i < file->public_dependency_count(); i++) {
    if (i == 0) {
      output("/* Public Imports. */\n");
    }
    output("#include \"$0\"\n", HeaderFilename(file));
    if (i == file->public_dependency_count() - 1) {
      output("\n");
    }
  }

  output(
      "#include \"upb/port_def.inc\"\n"
      "\n"
      "#ifdef __cplusplus\n"
      "extern \"C\" {\n"
      "#endif\n"
      "\n");

  const std::vector<const protobuf::Descriptor*> this_file_messages =
      SortedMessages(file);
  const std::vector<const protobuf::FieldDescriptor*> this_file_exts =
      SortedExtensions(file);

  // Forward-declare types defined in this file.
  for (auto message : this_file_messages) {
    output("struct $0;\n", ToCIdent(message->full_name()));
  }
  for (auto message : this_file_messages) {
    output("typedef struct $0 $0;\n", ToCIdent(message->full_name()));
  }
  for (auto message : this_file_messages) {
    output("extern const upb_msglayout $0;\n", MessageInit(message));
  }
  for (auto ext : this_file_exts) {
    output("extern const upb_msglayout_ext $0;\n", ExtensionLayout(ext));
  }

  // Forward-declare types not in this file, but used as submessages.
  // Order by full name for consistent ordering.
  std::map<std::string, const protobuf::Descriptor*> forward_messages;

  for (auto* message : this_file_messages) {
    for (int i = 0; i < message->field_count(); i++) {
      const protobuf::FieldDescriptor* field = message->field(i);
      if (field->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_MESSAGE &&
          field->file() != field->message_type()->file()) {
        forward_messages[field->message_type()->full_name()] =
            field->message_type();
      }
    }
  }
  for (auto ext : this_file_exts) {
    if (ext->file() != ext->containing_type()->file()) {
      forward_messages[ext->containing_type()->full_name()] =
          ext->containing_type();
    }
  }
  for (const auto& pair : forward_messages) {
    output("struct $0;\n", MessageName(pair.second));
  }
  for (const auto& pair : forward_messages) {
    output("extern const upb_msglayout $0;\n", MessageInit(pair.second));
  }

  if (!this_file_messages.empty()) {
    output("\n");
  }

  std::vector<const protobuf::EnumDescriptor*> this_file_enums =
      SortedEnums(file);

  for (auto enumdesc : this_file_enums) {
    output("typedef enum {\n");
    DumpEnumValues(enumdesc, output);
    output("} $0;\n\n", ToCIdent(enumdesc->full_name()));
  }

  output("\n");

  for (auto message : this_file_messages) {
    GenerateMessageInHeader(message, output);
  }

  for (auto ext : this_file_exts) {
    GenerateExtensionInHeader(ext, output);
  }


  output("extern const upb_msglayout_file $0;\n\n", FileLayoutName(file));

  if (file->name() == protobuf::FileDescriptorProto::descriptor()->file()->name()) {
    // This is gratuitously inefficient with how many times it rebuilds
    // MessageLayout objects for the same message. But we only do this for one
    // proto (descriptor.proto) so we don't worry about it.
    const protobuf::Descriptor* max32 = nullptr;
    const protobuf::Descriptor* max64 = nullptr;
    for (const auto* message : this_file_messages) {
      if (absl::EndsWith(message->name(), "Options")) {
        MessageLayout layout(message);
        if (max32 == nullptr) {
          max32 = message;
          max64 = message;
        } else {
          if (layout.message_size().size32 >
              MessageLayout(max32).message_size().size32) {
            max32 = message;
          }
          if (layout.message_size().size64 >
              MessageLayout(max64).message_size().size64) {
            max64 = message;
          }
        }
      }
    }

    output("/* Max size 32 is $0 */\n", max32->full_name());
    output("/* Max size 64 is $0 */\n", max64->full_name());
    MessageLayout::Size size;
    size.size32 = MessageLayout(max32).message_size().size32;
    size.size64 = MessageLayout(max32).message_size().size64;
    output("#define _UPB_MAXOPT_SIZE $0\n\n", GetSizeInit(size));
  }

  output(
      "#ifdef __cplusplus\n"
      "}  /* extern \"C\" */\n"
      "#endif\n"
      "\n"
      "#include \"upb/port_undef.inc\"\n"
      "\n"
      "#endif  /* $0_UPB_H_ */\n",
      ToPreproc(file->name()));
}

int TableDescriptorType(const protobuf::FieldDescriptor* field) {
  if (field->file()->syntax() == protobuf::FileDescriptor::SYNTAX_PROTO2 &&
      field->type() == protobuf::FieldDescriptor::TYPE_STRING) {
    // From the perspective of the binary encoder/decoder, proto2 string fields
    // are identical to bytes fields. Only in proto3 do we check UTF-8 for
    // string fields at parse time.
    //
    // If we ever use these tables for JSON encoding/decoding (for example by
    // embedding field names on the side) we will have to revisit this, because
    // string vs. bytes behavior is not affected by proto2 vs proto3.
    return protobuf::FieldDescriptor::TYPE_BYTES;
  } else {
    return field->type();
  }
}

struct SubmsgArray {
 public:
  SubmsgArray(const protobuf::Descriptor* message) : message_(message) {
    MessageLayout layout(message);
    std::vector<const protobuf::FieldDescriptor*> sorted_submsgs =
        SortedSubmessages(message);
    int i = 0;
    for (auto submsg : sorted_submsgs) {
      if (indexes_.find(submsg->message_type()) != indexes_.end()) {
        continue;
      }
      submsgs_.push_back(submsg->message_type());
      indexes_[submsg->message_type()] = i++;
    }
  }

  const std::vector<const protobuf::Descriptor*>& submsgs() const {
    return submsgs_;
  }

  int GetIndex(const protobuf::FieldDescriptor* field) {
    (void)message_;
    assert(field->containing_type() == message_);
    auto it = indexes_.find(field->message_type());
    assert(it != indexes_.end());
    return it->second;
  }

 private:
  const protobuf::Descriptor* message_;
  std::vector<const protobuf::Descriptor*> submsgs_;
  absl::flat_hash_map<const protobuf::Descriptor*, int> indexes_;
};

typedef std::pair<std::string, uint64_t> TableEntry;

uint64_t GetEncodedTag(const protobuf::FieldDescriptor* field) {
  protobuf::internal::WireFormatLite::WireType wire_type =
      protobuf::internal::WireFormat::WireTypeForField(field);
  uint32_t unencoded_tag =
      protobuf::internal::WireFormatLite::MakeTag(field->number(), wire_type);
  uint8_t tag_bytes[10] = {0};
  protobuf::io::CodedOutputStream::WriteVarint32ToArray(unencoded_tag,
                                                        tag_bytes);
  uint64_t encoded_tag = 0;
  memcpy(&encoded_tag, tag_bytes, sizeof(encoded_tag));
  // TODO: byte-swap for big endian.
  return encoded_tag;
}

int GetTableSlot(const protobuf::FieldDescriptor* field) {
  uint64_t tag = GetEncodedTag(field);
  if (tag > 0x7fff) {
    // Tag must fit within a two-byte varint.
    return -1;
  }
  return (tag & 0xf8) >> 3;
}

bool TryFillTableEntry(const protobuf::Descriptor* message,
                       const MessageLayout& layout,
                       const protobuf::FieldDescriptor* field,
                       TableEntry& ent) {
  std::string type = "";
  std::string cardinality = "";
  switch (field->type()) {
    case protobuf::FieldDescriptor::TYPE_BOOL:
      type = "b1";
      break;
    case protobuf::FieldDescriptor::TYPE_INT32:
    case protobuf::FieldDescriptor::TYPE_ENUM:
    case protobuf::FieldDescriptor::TYPE_UINT32:
      type = "v4";
      break;
    case protobuf::FieldDescriptor::TYPE_INT64:
    case protobuf::FieldDescriptor::TYPE_UINT64:
      type = "v8";
      break;
    case protobuf::FieldDescriptor::TYPE_FIXED32:
    case protobuf::FieldDescriptor::TYPE_SFIXED32:
    case protobuf::FieldDescriptor::TYPE_FLOAT:
      type = "f4";
      break;
    case protobuf::FieldDescriptor::TYPE_FIXED64:
    case protobuf::FieldDescriptor::TYPE_SFIXED64:
    case protobuf::FieldDescriptor::TYPE_DOUBLE:
      type = "f8";
      break;
    case protobuf::FieldDescriptor::TYPE_SINT32:
      type = "z4";
      break;
    case protobuf::FieldDescriptor::TYPE_SINT64:
      type = "z8";
      break;
    case protobuf::FieldDescriptor::TYPE_STRING:
      if (field->file()->syntax() == protobuf::FileDescriptor::SYNTAX_PROTO3) {
        // Only proto3 validates UTF-8.
        type = "s";
        break;
      }
      ABSL_FALLTHROUGH_INTENDED;
    case protobuf::FieldDescriptor::TYPE_BYTES:
      type = "b";
      break;
    case protobuf::FieldDescriptor::TYPE_MESSAGE:
      if (field->is_map()) {
        return false;  // Not supported yet (ever?).
      }
      type = "m";
      break;
    default:
      return false;  // Not supported yet.
  }

  switch (field->label()) {
    case protobuf::FieldDescriptor::LABEL_REPEATED:
      if (field->is_packed()) {
        cardinality = "p";
      } else {
        cardinality = "r";
      }
      break;
    case protobuf::FieldDescriptor::LABEL_OPTIONAL:
    case protobuf::FieldDescriptor::LABEL_REQUIRED:
      if (field->real_containing_oneof()) {
        cardinality = "o";
      } else {
        cardinality = "s";
      }
      break;
  }

  uint64_t expected_tag = GetEncodedTag(field);
  MessageLayout::Size offset = layout.GetFieldOffset(field);

  // Data is:
  //
  //                  48                32                16                 0
  // |--------|--------|--------|--------|--------|--------|--------|--------|
  // |   offset (16)   |case offset (16) |presence| submsg |  exp. tag (16)  |
  // |--------|--------|--------|--------|--------|--------|--------|--------|
  //
  // - |presence| is either hasbit index or field number for oneofs.

  uint64_t data = offset.size64 << 48 | expected_tag;

  if (field->is_repeated()) {
    // No hasbit/oneof-related fields.
  } if (field->real_containing_oneof()) {
    MessageLayout::Size case_offset =
        layout.GetOneofCaseOffset(field->real_containing_oneof());
    if (case_offset.size64 > 0xffff) return false;
    assert(field->number() < 256);
    data |= field->number() << 24;
    data |= case_offset.size64 << 32;
  } else {
    uint64_t hasbit_index = 63;  // No hasbit (set a high, unused bit).
    if (layout.HasHasbit(field)) {
      hasbit_index = layout.GetHasbitIndex(field);
      if (hasbit_index > 31) return false;
    }
    data |= hasbit_index << 24;
  }

  if (field->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
    SubmsgArray submsg_array(message);
    uint64_t idx = submsg_array.GetIndex(field);
    if (idx > 255) return false;
    data |= idx << 16;

    std::string size_ceil = "max";
    size_t size = SIZE_MAX;
    if (field->message_type()->file() == field->file()) {
      // We can only be guaranteed the size of the sub-message if it is in the
      // same file as us.  We could relax this to increase the speed of
      // cross-file sub-message parsing if we are comfortable requiring that
      // users compile all messages at the same time.
      MessageLayout sub_layout(field->message_type());
      size = sub_layout.message_size().size64 + 8;
    }
    std::vector<size_t> breaks = {64, 128, 192, 256};
    for (auto brk : breaks) {
      if (size <= brk) {
        size_ceil = std::to_string(brk);
        break;
      }
    }
    ent.first = absl::Substitute("upb_p$0$1_$2bt_max$3b", cardinality, type,
                                 expected_tag > 0xff ? "2" : "1", size_ceil);

  } else {
    ent.first = absl::Substitute("upb_p$0$1_$2bt", cardinality, type,
                                 expected_tag > 0xff ? "2" : "1");
  }
  ent.second = data;
  return true;
}

std::vector<TableEntry> FastDecodeTable(const protobuf::Descriptor* message,
                                        const MessageLayout& layout) {
  std::vector<TableEntry> table;
  for (const auto field : FieldHotnessOrder(message)) {
    TableEntry ent;
    int slot = GetTableSlot(field);
    // std::cerr << "table slot: " << field->number() << ": " << slot << "\n";
    if (slot < 0) {
      // Tag can't fit in the table.
      continue;
    }
    if (!TryFillTableEntry(message, layout, field, ent)) {
      // Unsupported field type or offset, hasbit index, etc. doesn't fit.
      continue;
    }
    while ((size_t)slot >= table.size()) {
      size_t size = std::max(static_cast<size_t>(1), table.size() * 2);
      table.resize(size, TableEntry{"fastdecode_generic", 0});
    }
    if (table[slot].first != "fastdecode_generic") {
      // A hotter field already filled this slot.
      continue;
    }
    table[slot] = ent;
  }
  return table;
}

void WriteField(const protobuf::FieldDescriptor* field,
                absl::string_view offset, absl::string_view presence,
                int submsg_index, Output& output) {
  std::string mode;
  std::string rep;
  if (field->is_map()) {
    mode = "_UPB_MODE_MAP";
    rep = "_UPB_REP_PTR";
  } else if (field->is_repeated()) {
    mode = "_UPB_MODE_ARRAY";
    rep = "_UPB_REP_PTR";
  } else {
    mode = "_UPB_MODE_SCALAR";
    rep = SizeRep(field);
  }

  if (field->is_packed()) {
    absl::StrAppend(&mode, " | _UPB_MODE_IS_PACKED");
  }

  if (field->is_extension()) {
    absl::StrAppend(&mode, " | _UPB_MODE_IS_EXTENSION");
  }

  output("{$0, $1, $2, $3, $4, $5 | ($6 << _UPB_REP_SHIFT)}", field->number(),
         offset, presence, submsg_index, TableDescriptorType(field), mode, rep);
}

// Writes a single field into a .upb.c source file.
void WriteMessageField(const protobuf::FieldDescriptor* field,
                       const MessageLayout& layout, int submsg_index,
                       Output& output) {
  std::string presence = "0";

  if (MessageLayout::HasHasbit(field)) {
    int index = layout.GetHasbitIndex(field);
    assert(index != 0);
    presence = absl::StrCat(index);
  } else if (field->real_containing_oneof()) {
    MessageLayout::Size case_offset =
        layout.GetOneofCaseOffset(field->real_containing_oneof());

    // We encode as negative to distinguish from hasbits.
    case_offset.size32 = ~case_offset.size32;
    case_offset.size64 = ~case_offset.size64;
    assert(case_offset.size32 < 0);
    assert(case_offset.size64 < 0);
    presence = GetSizeInit(case_offset);
  }

  output("  ");
  WriteField(field, GetSizeInit(layout.GetFieldOffset(field)), presence,
             submsg_index, output);
  output(",\n");
}

// Writes a single message into a .upb.c source file.
void WriteMessage(const protobuf::Descriptor* message, Output& output,
                  bool fasttable_enabled) {
  std::string msg_name = ToCIdent(message->full_name());
  std::string fields_array_ref = "NULL";
  std::string submsgs_array_ref = "NULL";
  uint8_t dense_below = 0;
  const int dense_below_max = std::numeric_limits<decltype(dense_below)>::max();
  MessageLayout layout(message);
  SubmsgArray submsg_array(message);

  if (!submsg_array.submsgs().empty()) {
    // TODO(haberman): could save a little bit of space by only generating a
    // "submsgs" array for every strongly-connected component.
    std::string submsgs_array_name = msg_name + "_submsgs";
    submsgs_array_ref = "&" + submsgs_array_name + "[0]";
    output("static const upb_msglayout_sub $0[$1] = {\n",
           submsgs_array_name, submsg_array.submsgs().size());

    for (auto submsg : submsg_array.submsgs()) {
      output("  {.submsg = &$0},\n", MessageInit(submsg));
    }

    output("};\n\n");
  }

  std::vector<const protobuf::FieldDescriptor*> field_number_order =
      FieldNumberOrder(message);
  if (!field_number_order.empty()) {
    std::string fields_array_name = msg_name + "__fields";
    fields_array_ref = "&" + fields_array_name + "[0]";
    output("static const upb_msglayout_field $0[$1] = {\n",
           fields_array_name, field_number_order.size());
    for (int i = 0; i < static_cast<int>(field_number_order.size()); i++) {
      auto field = field_number_order[i];
      int submsg_index = 0;

      if (i < dense_below_max && field->number() == i + 1 &&
          (i == 0 || field_number_order[i - 1]->number() == i)) {
        dense_below = i + 1;
      }

      if (field->cpp_type() == protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
        submsg_index = submsg_array.GetIndex(field);
      }

      WriteMessageField(field, layout, submsg_index, output);
    }
    output("};\n\n");
  }

  std::vector<TableEntry> table;
  uint8_t table_mask = -1;

  if (fasttable_enabled) {
    table = FastDecodeTable(message, layout);
  }

  if (table.size() > 1) {
    assert((table.size() & (table.size() - 1)) == 0);
    table_mask = (table.size() - 1) << 3;
  }

  std::string msgext = "_UPB_MSGEXT_NONE";

  if (message->extension_range_count()) {
    if (message->options().message_set_wire_format()) {
      msgext = "_UPB_MSGEXT_MSGSET";
    } else {
      msgext = "_UPB_MSGEXT_EXTENDABLE";
    }
  }

  output("const upb_msglayout $0 = {\n", MessageInit(message));
  output("  $0,\n", submsgs_array_ref);
  output("  $0,\n", fields_array_ref);
  output("  $0, $1, $2, $3, $4,\n",
         GetSizeInit(layout.message_size()),
         field_number_order.size(),
         msgext,
         dense_below,
         table_mask
  );
  if (!table.empty()) {
    output("  UPB_FASTTABLE_INIT({\n");
    for (const auto& ent : table) {
      output("    {0x$1, &$0},\n", ent.first,
             absl::StrCat(absl::Hex(ent.second, absl::kZeroPad16)));
    }
    output("  }),\n");
  }
  output("};\n\n");
}

void WriteExtension(const protobuf::FieldDescriptor* ext, Output& output) {
  output("const upb_msglayout_ext $0 = {\n  ", ExtensionLayout(ext));
  WriteField(ext, "0", "0", 0, output);
  output(",\n");
  output("  &$0,\n", MessageInit(ext->containing_type()));
  if (ext->message_type()) {
    output("  {.submsg = &$0},\n", MessageInit(ext->message_type()));
  } else {
    output("  {.submsg = NULL},\n");
  }
  output("\n};\n");
}

int WriteMessages(const protobuf::FileDescriptor* file, Output& output,
                   bool fasttable_enabled) {
  std::vector<const protobuf::Descriptor*> file_messages =
      SortedMessages(file);

  if (file_messages.empty()) return 0;

  for (auto message : file_messages) {
    WriteMessage(message, output, fasttable_enabled);
  }

  output("static const upb_msglayout *$0[$1] = {\n", kMessagesInit,
         file_messages.size());
  for (auto message : file_messages) {
    output("  &$0,\n", MessageInit(message));
  }
  output("};\n");
  output("\n");
  return file_messages.size();
}

int WriteExtensions(const protobuf::FileDescriptor* file, Output& output) {
  auto exts = SortedExtensions(file);
  absl::flat_hash_set<const protobuf::Descriptor*> forward_decls;

  if (exts.empty()) return 0;

  // Order by full name for consistent ordering.
  std::map<std::string, const protobuf::Descriptor*> forward_messages;

  for (auto ext : exts) {
    forward_messages[ext->containing_type()->full_name()] =
        ext->containing_type();
    if (ext->message_type()) {
      forward_messages[ext->message_type()->full_name()] = ext->message_type();
    }
  }

  for (const auto& decl : forward_messages) {
    output("extern const upb_msglayout $0;\n", MessageInit(decl.second));
  }

  for (auto ext : exts) {
    WriteExtension(ext, output);
  }

  output(
      "\n"
      "static const upb_msglayout_ext *$0[$1] = {\n",
      kExtensionsInit, exts.size());

  for (auto ext : exts) {
    output("  &$0,\n", ExtensionLayout(ext));
  }

  output(
      "};\n"
      "\n");
  return exts.size();
}

// Writes a .upb.c source file.
void WriteSource(const protobuf::FileDescriptor* file, Output& output,
                 bool fasttable_enabled) {
  EmitFileWarning(file, output);

  output(
      "#include <stddef.h>\n"
      "#include \"upb/msg_internal.h\"\n"
      "#include \"$0\"\n",
      HeaderFilename(file));

  for (int i = 0; i < file->dependency_count(); i++) {
    output("#include \"$0\"\n", HeaderFilename(file->dependency(i)));
  }

  output(
      "\n"
      "#include \"upb/port_def.inc\"\n"
      "\n");

  int msg_count = WriteMessages(file, output, fasttable_enabled);
  int ext_count = WriteExtensions(file, output);

  output("const upb_msglayout_file $0 = {\n", FileLayoutName(file));
  output("  $0,\n", msg_count ? kMessagesInit : "NULL");
  output("  $0,\n", ext_count ? kExtensionsInit : "NULL");
  output("  $0,\n", msg_count);
  output("  $0,\n", ext_count);
  output("};\n\n");

  output("#include \"upb/port_undef.inc\"\n");
  output("\n");
}

class Generator : public protoc::CodeGenerator {
  ~Generator() override {}
  bool Generate(const protobuf::FileDescriptor* file,
                const std::string& parameter, protoc::GeneratorContext* context,
                std::string* error) const override;
  uint64_t GetSupportedFeatures() const override {
    return FEATURE_PROTO3_OPTIONAL;
  }
};

bool Generator::Generate(const protobuf::FileDescriptor* file,
                         const std::string& parameter,
                         protoc::GeneratorContext* context,
                         std::string* error) const {
  bool fasttable_enabled = false;
  std::vector<std::pair<std::string, std::string>> params;
  google::protobuf::compiler::ParseGeneratorParameter(parameter, &params);

  for (const auto& pair : params) {
    if (pair.first == "fasttable") {
      fasttable_enabled = true;
    } else {
      *error = "Unknown parameter: " + pair.first;
      return false;
    }
  }

  Output h_output(context->Open(HeaderFilename(file)));
  WriteHeader(file, h_output);

  Output c_output(context->Open(SourceFilename(file)));
  WriteSource(file, c_output, fasttable_enabled);

  return true;
}

}  // namespace
}  // namespace upbc

int main(int argc, char** argv) {
  std::unique_ptr<google::protobuf::compiler::CodeGenerator> generator(
      new upbc::Generator());
  return google::protobuf::compiler::PluginMain(argc, argv, generator.get());
}
