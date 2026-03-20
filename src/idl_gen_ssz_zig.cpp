/*
 * Copyright 2024 Google Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "idl_gen_ssz_zig.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "flatbuffers/base.h"
#include "flatbuffers/code_generators.h"
#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/flatc.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/util.h"
#include "idl_namer.h"

#ifdef _WIN32
#include <direct.h>
#define PATH_SEPARATOR "\\"
#define mkdir(n, m) _mkdir(n)
#else
#include <sys/stat.h>
#define PATH_SEPARATOR "/"
#endif

namespace flatbuffers {

namespace ssz_zig {

// ---------------------------------------------------------------------------
// Phase 2: SSZ Type Resolution (identical to Go/Rust backends)
// ---------------------------------------------------------------------------

enum class SszType {
  Bool,
  Uint8,
  Uint16,
  Uint32,
  Uint64,
  Uint128,
  Uint256,
  Container,
  Vector,
  List,
  Bitlist,
  Bitvector,
  Union,
  ProgressiveContainer,
  ProgressiveList,
};

struct SszFieldInfo {
  SszType ssz_type;
  uint32_t fixed_size;     // 0 = variable-size
  uint64_t limit;          // ssz_max value for lists
  uint32_t bitsize;        // for bitvectors
  bool is_dynamic;
  const StructDef *struct_def;  // for container elements
  std::unique_ptr<SszFieldInfo> elem_info;  // for vector/list element type

  SszFieldInfo()
      : ssz_type(SszType::Uint8),
        fixed_size(0),
        limit(0),
        bitsize(0),
        is_dynamic(false),
        struct_def(nullptr) {}
};

struct SszContainerInfo {
  uint32_t static_size;
  bool is_fixed;
  bool is_progressive;
  uint32_t max_ssz_index;
  std::vector<uint8_t> active_fields_bitvector;
  std::map<const FieldDef *, uint32_t> field_ssz_indices;
  std::vector<const FieldDef *> all_fields;
  std::vector<const FieldDef *> dynamic_fields;
  std::map<const FieldDef *, SszFieldInfo> field_infos;

  SszContainerInfo()
      : static_size(0), is_fixed(true), is_progressive(false),
        max_ssz_index(0) {}
};

// ---------------------------------------------------------------------------
// Namer config
// ---------------------------------------------------------------------------

static std::set<std::string> ZigKeywords() {
  return {
      "addrspace", "align",      "allowzero", "and",       "anyframe",
      "anytype",   "asm",        "async",     "await",     "break",
      "catch",     "comptime",   "const",     "continue",  "defer",
      "else",      "enum",       "errdefer",  "error",     "export",
      "extern",    "false",      "fn",        "for",       "if",
      "inline",    "linksection","noalias",   "nosuspend", "null",
      "opaque",    "or",         "orelse",    "packed",    "pub",
      "resume",    "return",     "struct",    "suspend",   "switch",
      "test",      "threadlocal","true",      "try",       "undefined",
      "union",     "unreachable","var",       "volatile",  "while",
  };
}

static Namer::Config SszZigDefaultConfig() {
  return {/*types=*/Case::kKeep,
          /*constants=*/Case::kUnknown,
          /*methods=*/Case::kSnake,
          /*functions=*/Case::kSnake,
          /*fields=*/Case::kSnake,
          /*variables=*/Case::kSnake,
          /*variants=*/Case::kKeep,
          /*enum_variant_seperator=*/"",
          /*escape_keywords=*/Namer::Config::Escape::AfterConvertingCase,
          /*namespaces=*/Case::kKeep,
          /*namespace_seperator=*/"__",
          /*object_prefix=*/"",
          /*object_suffix=*/"",
          /*keyword_prefix=*/"",
          /*keyword_suffix=*/"_",
          /*keywords_casing=*/Namer::Config::KeywordsCasing::CaseSensitive,
          /*filenames=*/Case::kSnake,
          /*directories=*/Case::kSnake,
          /*output_path=*/"",
          /*filename_suffix=*/"_ssz",
          /*filename_extension=*/".zig"};
}

// Convert a CamelCase name to snake_case
static std::string ToSnakeCase(const std::string &input) {
  std::string result;
  for (size_t i = 0; i < input.size(); i++) {
    char c = input[i];
    if (isupper(c)) {
      if (i > 0 && !isupper(input[i - 1])) {
        result += '_';
      } else if (i > 0 && i + 1 < input.size() && isupper(input[i - 1]) &&
                 !isupper(input[i + 1])) {
        result += '_';
      }
      result += static_cast<char>(tolower(c));
    } else {
      result += c;
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------

class SszZigGenerator : public BaseGenerator {
 public:
  SszZigGenerator(const Parser &parser, const std::string &path,
                  const std::string &file_name)
      : BaseGenerator(parser, path, file_name, "", "", "zig"),
        namer_(WithFlagOptions(SszZigDefaultConfig(), parser.opts, path),
               ZigKeywords()) {}

  bool generate() override {
    std::string all_code;

    // File header
    all_code += "// Code generated by the FlatBuffers compiler. DO NOT EDIT.\n\n";
    all_code += "const std = @import(\"std\");\n";
    all_code += "const ssz = @import(\"ssz_runtime\");\n";
    all_code += "const Hasher = ssz.Hasher;\n\n";

    for (auto it = parser_.structs_.vec.begin();
         it != parser_.structs_.vec.end(); ++it) {
      auto &struct_def = **it;
      if (struct_def.generated) continue;

      SszContainerInfo container;
      if (!AnalyzeContainer(struct_def, container)) return false;

      std::string code;
      if (!GenerateType(struct_def, container, &code)) return false;
      all_code += code;
      all_code += "\n";
    }

    // Save single file per schema
    std::string filename = path_ + ToSnakeCase(file_name_) + "_ssz.zig";
    EnsureDirExists(path_);
    return parser_.opts.file_saver->SaveFile(filename.c_str(), all_code, false);
  }

 private:
  const IdlNamer namer_;

  // -----------------------------------------------------------------------
  // Type resolution (identical logic to Go/Rust backends)
  // -----------------------------------------------------------------------

  bool ResolveSszFieldInfo(const FieldDef &field, SszFieldInfo &info) {
    auto &type = field.value.type;
    auto *attrs = &field.attributes;

    switch (type.base_type) {
      case BASE_TYPE_BOOL:
        info.ssz_type = SszType::Bool;
        info.fixed_size = 1;
        info.is_dynamic = false;
        break;

      case BASE_TYPE_UCHAR:
      case BASE_TYPE_CHAR:
        info.ssz_type = SszType::Uint8;
        info.fixed_size = 1;
        info.is_dynamic = false;
        break;

      case BASE_TYPE_USHORT:
      case BASE_TYPE_SHORT:
        info.ssz_type = SszType::Uint16;
        info.fixed_size = 2;
        info.is_dynamic = false;
        break;

      case BASE_TYPE_UINT:
      case BASE_TYPE_INT:
        info.ssz_type = SszType::Uint32;
        info.fixed_size = 4;
        info.is_dynamic = false;
        break;

      case BASE_TYPE_ULONG:
      case BASE_TYPE_LONG:
        info.ssz_type = SszType::Uint64;
        info.fixed_size = 8;
        info.is_dynamic = false;
        break;

      case BASE_TYPE_STRING: {
        auto max_attr = attrs->Lookup("ssz_max");
        if (!max_attr) {
          flatbuffers::LogCompilerError(
              "field '" + field.name + "': string requires ssz_max");
          return false;
        }
        info.ssz_type = SszType::List;
        info.limit = StringToUInt(max_attr->constant.c_str());
        info.fixed_size = 0;
        info.is_dynamic = true;
        info.elem_info.reset(new SszFieldInfo());
        info.elem_info->ssz_type = SszType::Uint8;
        info.elem_info->fixed_size = 1;
        break;
      }

      case BASE_TYPE_VECTOR:
        return ResolveVectorType(field, type, *attrs, info);

      case BASE_TYPE_ARRAY:
        return ResolveArrayType(field, type, *attrs, info);

      case BASE_TYPE_STRUCT: {
        info.ssz_type = SszType::Container;
        info.struct_def = type.struct_def;
        SszContainerInfo nested;
        if (!AnalyzeContainer(*type.struct_def, nested)) return false;
        info.fixed_size = nested.is_fixed ? nested.static_size : 0;
        info.is_dynamic = !nested.is_fixed;
        break;
      }

      case BASE_TYPE_UNION:
        info.ssz_type = SszType::Union;
        info.fixed_size = 0;
        info.is_dynamic = true;
        break;

      case BASE_TYPE_FLOAT:
      case BASE_TYPE_DOUBLE:
        flatbuffers::LogCompilerError(
            "field '" + field.name + "': float/double not supported in SSZ");
        return false;

      default:
        flatbuffers::LogCompilerError(
            "field '" + field.name + "': unsupported base type for SSZ");
        return false;
    }
    return true;
  }

  bool ResolveVectorType(const FieldDef &field, const Type &type,
                         const SymbolTable<Value> &attrs, SszFieldInfo &info) {
    auto elem_type = type.VectorType();

    // Check for progressive bitlist (EIP-7916)
    if (attrs.Lookup("ssz_progressive_bitlist")) {
      info.ssz_type = SszType::Bitlist;
      info.limit = 0;  // unbounded
      info.fixed_size = 0;
      info.is_dynamic = true;
      return true;
    }

    // Check for bitlist attribute
    if (attrs.Lookup("ssz_bitlist")) {
      auto max_attr = attrs.Lookup("ssz_max");
      if (!max_attr) {
        flatbuffers::LogCompilerError(
            "field '" + field.name + "': ssz_bitlist requires ssz_max");
        return false;
      }
      info.ssz_type = SszType::Bitlist;
      info.limit = StringToUInt(max_attr->constant.c_str());
      info.fixed_size = 0;
      info.is_dynamic = true;
      return true;
    }

    // Check for progressive list (EIP-7916) — no ssz_max needed
    if (attrs.Lookup("ssz_progressive_list")) {
      info.ssz_type = SszType::ProgressiveList;
      info.fixed_size = 0;
      info.is_dynamic = true;
      info.elem_info.reset(new SszFieldInfo());
      if (!ResolveElemType(field, elem_type, *info.elem_info)) return false;
      return true;
    }

    // Regular vector (variable-length list)
    auto max_attr = attrs.Lookup("ssz_max");
    if (!max_attr) {
      flatbuffers::LogCompilerError(
          "field '" + field.name + "': vector requires ssz_max");
      return false;
    }

    // Parse potentially comma-separated ssz_max (outer,inner)
    std::string max_str = max_attr->constant;
    uint64_t outer_max = 0;
    uint64_t inner_max = 0;
    auto comma_pos = max_str.find(',');
    if (comma_pos != std::string::npos) {
      outer_max = StringToUInt(max_str.substr(0, comma_pos).c_str());
      inner_max = StringToUInt(max_str.substr(comma_pos + 1).c_str());
    } else {
      outer_max = StringToUInt(max_str.c_str());
    }

    info.ssz_type = SszType::List;
    info.limit = outer_max;
    info.fixed_size = 0;
    info.is_dynamic = true;

    // Resolve element type
    info.elem_info.reset(new SszFieldInfo());
    if (!ResolveElemType(field, elem_type, *info.elem_info)) return false;

    // For string elements ([][]byte), propagate inner max
    if (info.elem_info->ssz_type == SszType::List && inner_max > 0) {
      info.elem_info->limit = inner_max;
    }
    return true;
  }

  bool ResolveArrayType(const FieldDef &field, const Type &type,
                        const SymbolTable<Value> &attrs, SszFieldInfo &info) {
    auto elem_type = type.VectorType();
    uint16_t array_len = type.fixed_length;

    // Check for bitvector attribute
    if (attrs.Lookup("ssz_bitvector")) {
      info.ssz_type = SszType::Bitvector;
      auto bitsize_attr = attrs.Lookup("ssz_bitsize");
      if (bitsize_attr) {
        info.bitsize = static_cast<uint32_t>(
            StringToUInt(bitsize_attr->constant.c_str()));
      } else {
        info.bitsize = array_len * 8;
      }
      info.fixed_size = (info.bitsize + 7) / 8;
      info.is_dynamic = false;
      return true;
    }

    // Fixed-size array -> SSZ Vector
    info.ssz_type = SszType::Vector;

    // Resolve element type
    info.elem_info.reset(new SszFieldInfo());
    if (!ResolveElemType(field, elem_type, *info.elem_info)) return false;

    info.fixed_size = array_len * info.elem_info->fixed_size;
    info.is_dynamic = false;
    info.limit = array_len;
    return true;
  }

  bool ResolveElemType(const FieldDef &field, const Type &elem_type,
                       SszFieldInfo &info) {
    switch (elem_type.base_type) {
      case BASE_TYPE_BOOL:
        info.ssz_type = SszType::Bool;
        info.fixed_size = 1;
        break;
      case BASE_TYPE_UCHAR:
      case BASE_TYPE_CHAR:
        info.ssz_type = SszType::Uint8;
        info.fixed_size = 1;
        break;
      case BASE_TYPE_USHORT:
      case BASE_TYPE_SHORT:
        info.ssz_type = SszType::Uint16;
        info.fixed_size = 2;
        break;
      case BASE_TYPE_UINT:
      case BASE_TYPE_INT:
        info.ssz_type = SszType::Uint32;
        info.fixed_size = 4;
        break;
      case BASE_TYPE_ULONG:
      case BASE_TYPE_LONG:
        info.ssz_type = SszType::Uint64;
        info.fixed_size = 8;
        break;
      case BASE_TYPE_STRING: {
        // String element in a vector -> List[List[byte, inner_max], outer_max]
        info.ssz_type = SszType::List;
        info.fixed_size = 0;
        info.is_dynamic = true;
        info.elem_info.reset(new SszFieldInfo());
        info.elem_info->ssz_type = SszType::Uint8;
        info.elem_info->fixed_size = 1;
        break;
      }
      case BASE_TYPE_STRUCT: {
        info.ssz_type = SszType::Container;
        info.struct_def = elem_type.struct_def;
        SszContainerInfo nested;
        if (!AnalyzeContainer(*elem_type.struct_def, nested)) return false;
        info.fixed_size = nested.is_fixed ? nested.static_size : 0;
        info.is_dynamic = !nested.is_fixed;
        break;
      }
      default:
        flatbuffers::LogCompilerError(
            "field '" + field.name + "': unsupported element type for SSZ");
        return false;
    }
    return true;
  }

  bool AnalyzeContainer(const StructDef &struct_def,
                        SszContainerInfo &container) {
    container.static_size = 0;
    container.is_fixed = true;

    for (auto it = struct_def.fields.vec.begin();
         it != struct_def.fields.vec.end(); ++it) {
      auto &field = **it;
      if (field.deprecated) continue;

      SszFieldInfo info;
      if (!ResolveSszFieldInfo(field, info)) return false;

      container.all_fields.push_back(&field);

      if (info.is_dynamic) {
        container.is_fixed = false;
        container.dynamic_fields.push_back(&field);
        container.static_size += 4;  // offset slot
      } else {
        container.static_size += info.fixed_size;
      }

      container.field_infos.emplace(&field, std::move(info));
    }

    // Detect progressive container (EIP-7495)
    if (struct_def.attributes.Lookup("ssz_progressive")) {
      container.is_progressive = true;
      container.max_ssz_index = 0;

      for (auto *field : container.all_fields) {
        auto id_attr = field->attributes.Lookup("id");
        if (id_attr) {
          uint32_t idx = static_cast<uint32_t>(
              StringToUInt(id_attr->constant.c_str()));
          container.field_ssz_indices[field] = idx;
          if (idx > container.max_ssz_index) container.max_ssz_index = idx;
        }
      }

      // Compute active_fields bitvector
      uint32_t bitvec_bytes = (container.max_ssz_index + 8) / 8;
      container.active_fields_bitvector.resize(bitvec_bytes, 0);
      for (auto &kv : container.field_ssz_indices) {
        uint32_t idx = kv.second;
        container.active_fields_bitvector[idx / 8] |=
            static_cast<uint8_t>(1u << (idx % 8));
      }
    }

    return true;
  }

  // -----------------------------------------------------------------------
  // Zig type helpers
  // -----------------------------------------------------------------------

  std::string ZigTypeName(const SszFieldInfo &info,
                          const FieldDef & /*field*/) {
    switch (info.ssz_type) {
      case SszType::Bool: return "bool";
      case SszType::Uint8: return "u8";
      case SszType::Uint16: return "u16";
      case SszType::Uint32: return "u32";
      case SszType::Uint64: return "u64";
      case SszType::Uint128: return "[16]u8";
      case SszType::Uint256: return "[32]u8";
      case SszType::Container:
        return info.struct_def ? info.struct_def->name : "void";
      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8)
          return "[" + NumToString(info.limit) + "]u8";
        if (info.elem_info)
          return "[" + NumToString(info.limit) + "]" +
                 ZigElemTypeName(*info.elem_info);
        return "void";
      }
      case SszType::ProgressiveList:
      case SszType::List: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8)
          return "[]u8";
        if (info.elem_info && info.elem_info->ssz_type == SszType::List)
          return "[][]u8";
        if (info.elem_info)
          return "[]" + ZigElemTypeName(*info.elem_info);
        return "[]u8";
      }
      case SszType::Bitlist: return "[]u8";
      case SszType::Bitvector:
        return "[" + NumToString((info.bitsize + 7) / 8) + "]u8";
      case SszType::ProgressiveContainer:
        return info.struct_def ? info.struct_def->name : "void";
      case SszType::Union: return "void";
    }
    return "void";
  }

  std::string ZigElemTypeName(const SszFieldInfo &info) {
    switch (info.ssz_type) {
      case SszType::Bool: return "bool";
      case SszType::Uint8: return "u8";
      case SszType::Uint16: return "u16";
      case SszType::Uint32: return "u32";
      case SszType::Uint64: return "u64";
      case SszType::Container:
        return info.struct_def ? info.struct_def->name : "void";
      case SszType::ProgressiveList:
      case SszType::List: return "[]u8";
      default: return "void";
    }
  }

  // -----------------------------------------------------------------------
  // Code generation entry
  // -----------------------------------------------------------------------

  bool GenerateType(const StructDef &struct_def,
                    const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;

    // Struct definition
    GenZigStruct(struct_def, container, &c);
    c += "\n";

    return true;
  }

  // -----------------------------------------------------------------------
  // Zig struct generation with methods
  // -----------------------------------------------------------------------

  void GenZigStruct(const StructDef &struct_def,
                    const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    std::string name = struct_def.name;

    c += "pub const " + name + " = struct {\n";

    // Fields
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      c += "    " + ToSnakeCase(field->name) + ": " +
           ZigTypeName(it->second, *field) + ",\n";
    }
    c += "\n";
    c += "    const Self = @This();\n";
    c += "\n";

    // --- sszBytesLen ---
    GenSizeMethod(container, &c);
    c += "\n";

    // --- sszAppend ---
    GenMarshalMethod(container, &c);
    c += "\n";

    // --- toSszBytes ---
    GenToSszBytes(&c);
    c += "\n";

    // --- fromSszBytes ---
    GenUnmarshalMethod(container, &c);
    c += "\n";

    // --- treeHashRoot ---
    GenTreeHashRoot(&c);
    c += "\n";

    // --- treeHashWith ---
    GenHashMethod(container, &c);

    c += "};\n";
  }

  // -----------------------------------------------------------------------
  // sszBytesLen
  // -----------------------------------------------------------------------

  void GenSizeMethod(const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    c += "    pub fn sszBytesLen(self: *const Self) usize {\n";
    if (container.is_fixed) {
      c += "        return " + NumToString(container.static_size) + ";\n";
    } else {
      c += "        var size: usize = " +
           NumToString(container.static_size) + ";\n";
      for (auto *field : container.dynamic_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "self." + ToSnakeCase(field->name);
        GenSizeField(info, fname, &c);
      }
      c += "        return size;\n";
    }
    c += "    }\n";
  }

  void GenSizeField(const SszFieldInfo &info, const std::string &var,
                    std::string *code) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::ProgressiveList:
      case SszType::List:
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          // List of byte slices: offsets + each element's length
          c += "        size += " + var + ".len * 4;\n";
          c += "        for (" + var + ") |item| {\n";
          c += "            size += item.len;\n";
          c += "        }\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container &&
                   info.elem_info->is_dynamic) {
          c += "        size += " + var + ".len * 4;\n";
          c += "        for (" + var + ") |*item| {\n";
          c += "            size += item.sszBytesLen();\n";
          c += "        }\n";
        } else if (info.elem_info) {
          c += "        size += " + var + ".len * " +
               NumToString(info.elem_info->fixed_size) + ";\n";
        } else {
          c += "        size += " + var + ".len;\n";
        }
        break;
      case SszType::Bitlist:
        c += "        size += " + var + ".len;\n";
        break;
      case SszType::ProgressiveContainer:
      case SszType::Container:
        c += "        size += " + var + ".sszBytesLen();\n";
        break;
      default: break;
    }
  }

  // -----------------------------------------------------------------------
  // sszAppend
  // -----------------------------------------------------------------------

  void GenMarshalMethod(const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    c += "    pub fn sszAppend(self: *const Self, writer: anytype) !void {\n";

    if (container.dynamic_fields.empty()) {
      // All-fixed container
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        GenMarshalField(it->second, "self." + ToSnakeCase(field->name), &c,
                        "        ");
      }
    } else {
      // Container with dynamic fields: write static section, then dynamic
      c += "        // Static section\n";
      c += "        var offset: u32 = " +
           NumToString(container.static_size) + ";\n";

      // Compute dynamic field sizes for offsets
      for (auto *field : container.dynamic_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        (void)it;  // just need to iterate
      }

      // Write static section: fixed fields inline, offsets for dynamic
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "self." + ToSnakeCase(field->name);

        if (info.is_dynamic) {
          c += "        try writer.writeAll(std.mem.asBytes(&std.mem.nativeToLittle(u32, offset)));\n";
          // Add size of this dynamic field to offset
          GenOffsetIncrement(info, fname, &c);
        } else {
          GenMarshalField(info, fname, &c, "        ");
        }
      }

      c += "\n        // Dynamic section\n";
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "self." + ToSnakeCase(field->name);

        if (info.is_dynamic) {
          GenMarshalField(info, fname, &c, "        ");
        }
      }
    }

    c += "    }\n";
  }

  void GenOffsetIncrement(const SszFieldInfo &info, const std::string &var,
                          std::string *code) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::ProgressiveList:
      case SszType::List:
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          c += "        offset += @intCast(" + var + ".len * 4);\n";
          c += "        for (" + var + ") |item| {\n";
          c += "            offset += @intCast(item.len);\n";
          c += "        }\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container &&
                   info.elem_info->is_dynamic) {
          c += "        offset += @intCast(" + var + ".len * 4);\n";
          c += "        for (" + var + ") |*item| {\n";
          c += "            offset += @intCast(item.sszBytesLen());\n";
          c += "        }\n";
        } else if (info.elem_info) {
          c += "        offset += @intCast(" + var + ".len * " +
               NumToString(info.elem_info->fixed_size) + ");\n";
        } else {
          c += "        offset += @intCast(" + var + ".len);\n";
        }
        break;
      case SszType::Bitlist:
        c += "        offset += @intCast(" + var + ".len);\n";
        break;
      case SszType::ProgressiveContainer:
      case SszType::Container:
        c += "        offset += @intCast(" + var + ".sszBytesLen());\n";
        break;
      default: break;
    }
  }

  void GenMarshalField(const SszFieldInfo &info, const std::string &var,
                       std::string *code, const std::string &ind) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::Bool:
        c += ind + "try writer.writeByte(if (" + var + ") @as(u8, 1) else @as(u8, 0));\n";
        break;

      case SszType::Uint8:
        c += ind + "try writer.writeByte(" + var + ");\n";
        break;

      case SszType::Uint16:
        c += ind + "try writer.writeAll(std.mem.asBytes(&std.mem.nativeToLittle(u16, " + var + ")));\n";
        break;

      case SszType::Uint32:
        c += ind + "try writer.writeAll(std.mem.asBytes(&std.mem.nativeToLittle(u32, " + var + ")));\n";
        break;

      case SszType::Uint64:
        c += ind + "try writer.writeAll(std.mem.asBytes(&std.mem.nativeToLittle(u64, " + var + ")));\n";
        break;

      case SszType::Uint128:
      case SszType::Uint256:
        c += ind + "try writer.writeAll(&" + var + ");\n";
        break;

      case SszType::Vector:
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += ind + "try writer.writeAll(&" + var + ");\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += ind + "for (" + var + ") |*item| {\n";
          c += ind + "    try item.sszAppend(writer);\n";
          c += ind + "}\n";
        } else if (info.elem_info) {
          GenMarshalPrimitiveArray(info, var, code, ind);
        }
        break;

      case SszType::ProgressiveList:
      case SszType::List:
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          // List of byte slices: offset-based dynamic encoding
          c += ind + "{\n";
          c += ind + "    var inner_offset: u32 = @intCast(" + var + ".len * 4);\n";
          c += ind + "    for (" + var + ") |item| {\n";
          c += ind + "        try writer.writeAll(std.mem.asBytes(&std.mem.nativeToLittle(u32, inner_offset)));\n";
          c += ind + "        inner_offset += @intCast(item.len);\n";
          c += ind + "    }\n";
          c += ind + "    for (" + var + ") |item| {\n";
          c += ind + "        try writer.writeAll(item);\n";
          c += ind + "    }\n";
          c += ind + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += ind + "try writer.writeAll(" + var + ");\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          if (info.elem_info->is_dynamic) {
            c += ind + "{\n";
            c += ind + "    var inner_offset: u32 = @intCast(" + var + ".len * 4);\n";
            c += ind + "    for (" + var + ") |*item| {\n";
            c += ind + "        try writer.writeAll(std.mem.asBytes(&std.mem.nativeToLittle(u32, inner_offset)));\n";
            c += ind + "        inner_offset += @intCast(item.sszBytesLen());\n";
            c += ind + "    }\n";
            c += ind + "    for (" + var + ") |*item| {\n";
            c += ind + "        try item.sszAppend(writer);\n";
            c += ind + "    }\n";
            c += ind + "}\n";
          } else {
            c += ind + "for (" + var + ") |*item| {\n";
            c += ind + "    try item.sszAppend(writer);\n";
            c += ind + "}\n";
          }
        } else if (info.elem_info) {
          GenMarshalPrimitiveSlice(info, var, code, ind);
        } else {
          c += ind + "try writer.writeAll(" + var + ");\n";
        }
        break;

      case SszType::Bitlist:
        c += ind + "try writer.writeAll(" + var + ");\n";
        break;

      case SszType::Bitvector:
        c += ind + "try writer.writeAll(&" + var + ");\n";
        break;

      case SszType::ProgressiveContainer:
      case SszType::Container:
        c += ind + "try " + var + ".sszAppend(writer);\n";
        break;

      case SszType::Union:
        break;
    }
  }

  void GenMarshalPrimitiveArray(const SszFieldInfo &info,
                                const std::string &var, std::string *code,
                                const std::string &ind) {
    std::string &c = *code;
    c += ind + "for (" + var + ") |v| {\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += ind + "    try writer.writeByte(if (v) @as(u8, 1) else @as(u8, 0));\n";
        break;
      case SszType::Uint16:
        c += ind + "    try writer.writeAll(std.mem.asBytes(&std.mem.nativeToLittle(u16, v)));\n";
        break;
      case SszType::Uint32:
        c += ind + "    try writer.writeAll(std.mem.asBytes(&std.mem.nativeToLittle(u32, v)));\n";
        break;
      case SszType::Uint64:
        c += ind + "    try writer.writeAll(std.mem.asBytes(&std.mem.nativeToLittle(u64, v)));\n";
        break;
      default: break;
    }
    c += ind + "}\n";
  }

  void GenMarshalPrimitiveSlice(const SszFieldInfo &info,
                                const std::string &var, std::string *code,
                                const std::string &ind) {
    GenMarshalPrimitiveArray(info, var, code, ind);
  }

  // -----------------------------------------------------------------------
  // toSszBytes
  // -----------------------------------------------------------------------

  void GenToSszBytes(std::string *code) {
    std::string &c = *code;
    c += "    pub fn toSszBytes(self: *const Self, allocator: std.mem.Allocator) ![]u8 {\n";
    c += "        var buf = std.ArrayList(u8).init(allocator);\n";
    c += "        errdefer buf.deinit();\n";
    c += "        try self.sszAppend(buf.writer());\n";
    c += "        return buf.toOwnedSlice();\n";
    c += "    }\n";
  }

  // -----------------------------------------------------------------------
  // fromSszBytes
  // -----------------------------------------------------------------------

  void GenUnmarshalMethod(const SszContainerInfo &container,
                          std::string *code) {
    std::string &c = *code;
    c += "    pub fn fromSszBytes(bytes: []const u8) !Self {\n";
    c += "        if (bytes.len < " +
         NumToString(container.static_size) + ") return error.BufferTooSmall;\n";

    if (container.dynamic_fields.empty()) {
      // All-fixed container
      c += "        return Self{\n";
      uint32_t offset = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        c += "            ." + ToSnakeCase(field->name) + " = " +
             GenUnmarshalExpr(info, "bytes", offset) + ",\n";
        offset += info.fixed_size;
      }
      c += "        };\n";
    } else {
      // Container with dynamic fields
      uint32_t offset = 0;
      int dyn_idx = 0;
      std::vector<std::pair<const FieldDef *, int>> dyn_fields;

      // Read offsets and fixed fields
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;

        if (info.is_dynamic) {
          c += "        const offset" + NumToString(dyn_idx) +
               " = std.mem.readInt(u32, bytes[" + NumToString(offset) +
               ".." + NumToString(offset + 4) + "][0..4], .little);\n";
          if (dyn_idx == 0) {
            c += "        if (offset0 != " +
                 NumToString(container.static_size) +
                 ") return error.InvalidOffset;\n";
          } else {
            c += "        if (offset" + NumToString(dyn_idx) +
                 " < offset" + NumToString(dyn_idx - 1) +
                 " or offset" + NumToString(dyn_idx) +
                 " > bytes.len) return error.InvalidOffset;\n";
          }
          dyn_fields.push_back({field, dyn_idx});
          offset += 4;
          dyn_idx++;
        } else {
          offset += info.fixed_size;
        }
      }

      // Build result
      c += "        var result: Self = undefined;\n";

      // Assign fixed fields
      offset = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;

        if (info.is_dynamic) {
          offset += 4;
        } else {
          c += "        result." + ToSnakeCase(field->name) + " = " +
               GenUnmarshalExpr(info, "bytes", offset) + ";\n";
          offset += info.fixed_size;
        }
      }

      // Assign dynamic fields
      for (size_t i = 0; i < dyn_fields.size(); i++) {
        auto *field = dyn_fields[i].first;
        int oidx = dyn_fields[i].second;
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string start = "offset" + NumToString(oidx);
        std::string end;
        if (i + 1 < dyn_fields.size()) {
          end = "offset" + NumToString(dyn_fields[i + 1].second);
        } else {
          end = "bytes.len";
        }

        c += "        result." + ToSnakeCase(field->name) + " = " +
             GenUnmarshalDynExpr(info, "bytes", start, end) + ";\n";
      }

      c += "        return result;\n";
    }
    c += "    }\n";
  }

  std::string GenUnmarshalExpr(const SszFieldInfo &info,
                               const std::string &buf, uint32_t offset) {
    std::string s = NumToString(offset);
    std::string e = NumToString(offset + info.fixed_size);

    switch (info.ssz_type) {
      case SszType::Bool:
        return buf + "[" + s + "] == 1";
      case SszType::Uint8:
        return buf + "[" + s + "]";
      case SszType::Uint16:
        return "std.mem.readInt(u16, " + buf + "[" + s + ".." + e + "][0..2], .little)";
      case SszType::Uint32:
        return "std.mem.readInt(u32, " + buf + "[" + s + ".." + e + "][0..4], .little)";
      case SszType::Uint64:
        return "std.mem.readInt(u64, " + buf + "[" + s + ".." + e + "][0..8], .little)";
      case SszType::Uint128:
      case SszType::Uint256:
        return buf + "[" + s + ".." + e + "][0.." +
               NumToString(info.fixed_size) + "].*";
      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          return buf + "[" + s + ".." + e + "][0.." +
                 NumToString(info.fixed_size) + "].*";
        }
        // For container vectors and primitive vectors, we need more complex handling
        // For now, delegate to a runtime call pattern
        if (info.elem_info &&
            info.elem_info->ssz_type == SszType::Container &&
            info.elem_info->struct_def) {
          // This returns an array, but we can't construct it in a single expression
          // We'll use a block expression
          uint32_t esize = info.elem_info->fixed_size;
          std::string sname = info.elem_info->struct_def->name;
          return "blk: {\n"
                 "            var arr: [" + NumToString(info.limit) + "]" + sname + " = undefined;\n"
                 "            var idx: usize = 0;\n"
                 "            while (idx < " + NumToString(info.limit) + ") : (idx += 1) {\n"
                 "                const start = " + s + " + idx * " + NumToString(esize) + ";\n"
                 "                arr[idx] = try " + sname + ".fromSszBytes(" + buf +
                 "[start..start + " + NumToString(esize) + "]);\n"
                 "            }\n"
                 "            break :blk arr;\n"
                 "        }";
        }
        if (info.elem_info) {
          uint32_t esize = info.elem_info->fixed_size;
          return "blk: {\n"
                 "            var arr: [" + NumToString(info.limit) + "]" +
                 ZigElemTypeName(*info.elem_info) + " = undefined;\n"
                 "            var idx: usize = 0;\n"
                 "            while (idx < " + NumToString(info.limit) + ") : (idx += 1) {\n"
                 "                const start = " + s + " + idx * " + NumToString(esize) + ";\n"
                 "                arr[idx] = " +
                 GenReadPrimitive(info.elem_info->ssz_type, buf, "start", esize) + ";\n"
                 "            }\n"
                 "            break :blk arr;\n"
                 "        }";
        }
        return "undefined";
      }
      case SszType::Bitvector:
        return buf + "[" + s + ".." + e + "][0.." +
               NumToString(info.fixed_size) + "].*";
      case SszType::ProgressiveContainer:
      case SszType::Container:
        return "try " + (info.struct_def ? info.struct_def->name : "void") +
               ".fromSszBytes(" + buf + "[" + s + ".." + e + "])";
      default:
        return "undefined";
    }
  }

  std::string GenReadPrimitive(SszType t, const std::string &buf,
                               const std::string &off, uint32_t esize) {
    switch (t) {
      case SszType::Bool:
        return buf + "[" + off + "] == 1";
      case SszType::Uint8:
        return buf + "[" + off + "]";
      case SszType::Uint16:
        return "std.mem.readInt(u16, " + buf + "[" + off + "..][0..2], .little)";
      case SszType::Uint32:
        return "std.mem.readInt(u32, " + buf + "[" + off + "..][0..4], .little)";
      case SszType::Uint64:
        return "std.mem.readInt(u64, " + buf + "[" + off + "..][0..8], .little)";
      default:
        (void)esize;
        return "undefined";
    }
  }

  std::string GenUnmarshalDynExpr(const SszFieldInfo &info,
                                  const std::string &buf,
                                  const std::string &start,
                                  const std::string &end) {
    std::string slice = buf + "[" + start + ".." + end + "]";

    switch (info.ssz_type) {
      case SszType::ProgressiveList:
      case SszType::List:
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          return "@constCast(" + slice + ")";
        }
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          // Vec<Vec<u8>> — offset-based: return raw slice, needs runtime decode
          return "@constCast(" + slice + ")";
        }
        if (info.elem_info &&
            info.elem_info->ssz_type == SszType::Container &&
            info.elem_info->struct_def) {
          // For both dynamic and fixed containers in a list,
          // we return the raw slice; proper decoding needs allocator
          return "@constCast(" + slice + ")";
        }
        // Primitive slices
        return "@constCast(" + slice + ")";

      case SszType::Bitlist:
        return "@constCast(" + slice + ")";

      case SszType::ProgressiveContainer:
      case SszType::Container:
        if (info.struct_def) {
          return "try " + info.struct_def->name +
                 ".fromSszBytes(" + slice + ")";
        }
        return "undefined";

      default:
        return "undefined";
    }
  }

  // -----------------------------------------------------------------------
  // treeHashRoot
  // -----------------------------------------------------------------------

  void GenTreeHashRoot(std::string *code) {
    std::string &c = *code;
    c += "    pub fn treeHashRoot(self: *const Self) [32]u8 {\n";
    c += "        var h = Hasher.init();\n";
    c += "        self.treeHashWith(&h);\n";
    c += "        return h.finish();\n";
    c += "    }\n";
  }

  // -----------------------------------------------------------------------
  // treeHashWith
  // -----------------------------------------------------------------------

  void GenHashMethod(const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    c += "    pub fn treeHashWith(self: *const Self, h: *Hasher) void {\n";
    c += "        const idx = h.index();\n";

    if (container.is_progressive) {
      // EIP-7495: emit leaves for indices 0..max_ssz_index,
      // filling gaps with zero-hash for inactive fields.
      std::map<uint32_t, const FieldDef *> index_to_field;
      for (auto &kv : container.field_ssz_indices) {
        index_to_field[kv.second] = kv.first;
      }

      for (uint32_t i = 0; i <= container.max_ssz_index; i++) {
        auto fit = index_to_field.find(i);
        if (fit != index_to_field.end()) {
          auto *field = fit->second;
          auto iit = container.field_infos.find(field);
          if (iit == container.field_infos.end()) continue;
          std::string fname = "self." + ToSnakeCase(field->name);
          GenHashField(iit->second, fname, &c, "        ");
        } else {
          c += "        h.putUint8(0);\n";
        }
      }

      // Emit active_fields bitvector literal
      c += "        h.merkleizeProgressiveWithActiveFields(idx, &[_]u8{";
      for (size_t i = 0; i < container.active_fields_bitvector.size(); i++) {
        if (i > 0) c += ", ";
        char hex[8];
        snprintf(hex, sizeof(hex), "0x%02x",
                 container.active_fields_bitvector[i]);
        c += hex;
      }
      c += "});\n";
    } else {
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "self." + ToSnakeCase(field->name);

        GenHashField(info, fname, &c, "        ");
      }

      c += "        h.merkleize(idx);\n";
    }

    c += "    }\n";
  }

  void GenHashField(const SszFieldInfo &info, const std::string &var,
                    std::string *code, const std::string &ind) {
    std::string &c = *code;

    switch (info.ssz_type) {
      case SszType::Bool:
        c += ind + "h.putBool(" + var + ");\n";
        break;

      case SszType::Uint8:
        c += ind + "h.putUint8(" + var + ");\n";
        break;

      case SszType::Uint16:
        c += ind + "h.putUint16(" + var + ");\n";
        break;

      case SszType::Uint32:
        c += ind + "h.putUint32(" + var + ");\n";
        break;

      case SszType::Uint64:
        c += ind + "h.putUint64(" + var + ");\n";
        break;

      case SszType::Uint128:
      case SszType::Uint256:
        c += ind + "h.putBytes(&" + var + ");\n";
        break;

      case SszType::Vector:
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += ind + "h.putBytes(&" + var + ");\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += ind + "{\n";
          c += ind + "    const sub_idx = h.index();\n";
          c += ind + "    for (" + var + ") |*item| {\n";
          c += ind + "        item.treeHashWith(h);\n";
          c += ind + "    }\n";
          c += ind + "    h.merkleize(sub_idx);\n";
          c += ind + "}\n";
        } else if (info.elem_info) {
          c += ind + "{\n";
          c += ind + "    const sub_idx = h.index();\n";
          c += ind + "    for (" + var + ") |v| {\n";
          GenAppendPrimitive(info.elem_info->ssz_type, "v", code,
                             ind + "        ");
          c += ind + "    }\n";
          c += ind + "    h.fillUpTo32();\n";
          c += ind + "    h.merkleize(sub_idx);\n";
          c += ind + "}\n";
        }
        break;

      case SszType::List: {
        uint64_t limit = info.limit;
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          // List of byte lists
          uint64_t inner_limit = info.elem_info->limit;
          uint64_t inner_chunk_limit =
              inner_limit > 0 ? ssz_limit_for_bytes(inner_limit) : 0;
          c += ind + "{\n";
          c += ind + "    const sub_idx = h.index();\n";
          c += ind + "    for (" + var + ") |item| {\n";
          c += ind + "        const inner = h.index();\n";
          c += ind + "        h.appendBytes32(item);\n";
          if (inner_chunk_limit > 0) {
            c += ind + "        h.merkleizeWithMixin(inner, item.len, " +
                 NumToString(inner_chunk_limit) + ");\n";
          } else {
            c += ind + "        h.merkleize(inner);\n";
          }
          c += ind + "    }\n";
          c += ind + "    h.merkleizeWithMixin(sub_idx, " + var +
               ".len, " + NumToString(limit) + ");\n";
          c += ind + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += ind + "{\n";
          c += ind + "    const sub_idx = h.index();\n";
          c += ind + "    h.appendBytes32(" + var + ");\n";
          c += ind + "    h.merkleizeWithMixin(sub_idx, " + var +
               ".len, " + NumToString(ssz_limit_for_bytes(limit)) + ");\n";
          c += ind + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += ind + "{\n";
          c += ind + "    const sub_idx = h.index();\n";
          c += ind + "    for (" + var + ") |*item| {\n";
          c += ind + "        item.treeHashWith(h);\n";
          c += ind + "    }\n";
          c += ind + "    h.merkleizeWithMixin(sub_idx, " + var +
               ".len, " + NumToString(limit) + ");\n";
          c += ind + "}\n";
        } else if (info.elem_info) {
          c += ind + "{\n";
          c += ind + "    const sub_idx = h.index();\n";
          c += ind + "    for (" + var + ") |v| {\n";
          GenAppendPrimitive(info.elem_info->ssz_type, "v", code,
                             ind + "        ");
          c += ind + "    }\n";
          c += ind + "    h.fillUpTo32();\n";
          uint64_t hash_limit =
              ssz_limit_for_elements(limit, info.elem_info->fixed_size);
          c += ind + "    h.merkleizeWithMixin(sub_idx, " + var +
               ".len, " + NumToString(hash_limit) + ");\n";
          c += ind + "}\n";
        }
        break;
      }

      case SszType::Bitlist:
        c += ind + "h.putBitlist(" + var + ", " +
             NumToString(info.limit) + ");\n";
        break;

      case SszType::Bitvector:
        c += ind + "h.putBytes(&" + var + ");\n";
        break;

      case SszType::Container:
        c += ind + var + ".treeHashWith(h);\n";
        break;

      case SszType::ProgressiveContainer:
        c += ind + var + ".treeHashWith(h);\n";
        break;

      case SszType::ProgressiveList: {
        // EIP-7916: use merkleizeProgressiveWithMixin
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += ind + "{\n";
          c += ind + "    const sub_idx = h.index();\n";
          c += ind + "    h.appendBytes32(" + var + ");\n";
          c += ind + "    h.merkleizeProgressiveWithMixin(sub_idx, " + var +
               ".len);\n";
          c += ind + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += ind + "{\n";
          c += ind + "    const sub_idx = h.index();\n";
          c += ind + "    for (" + var + ") |*item| {\n";
          c += ind + "        item.treeHashWith(h);\n";
          c += ind + "    }\n";
          c += ind + "    h.merkleizeProgressiveWithMixin(sub_idx, " + var +
               ".len);\n";
          c += ind + "}\n";
        } else if (info.elem_info) {
          c += ind + "{\n";
          c += ind + "    const sub_idx = h.index();\n";
          c += ind + "    for (" + var + ") |v| {\n";
          GenAppendPrimitive(info.elem_info->ssz_type, "v", code,
                             ind + "        ");
          c += ind + "    }\n";
          c += ind + "    h.fillUpTo32();\n";
          c += ind + "    h.merkleizeProgressiveWithMixin(sub_idx, " + var +
               ".len);\n";
          c += ind + "}\n";
        }
        break;
      }

      case SszType::Union:
        break;
    }
  }

  void GenAppendPrimitive(SszType t, const std::string &var, std::string *code,
                          const std::string &ind) {
    std::string &c = *code;
    switch (t) {
      case SszType::Bool: c += ind + "h.appendBool(" + var + ");\n"; break;
      case SszType::Uint8: c += ind + "h.appendUint8(" + var + ");\n"; break;
      case SszType::Uint16: c += ind + "h.appendUint16(" + var + ");\n"; break;
      case SszType::Uint32: c += ind + "h.appendUint32(" + var + ");\n"; break;
      case SszType::Uint64: c += ind + "h.appendUint64(" + var + ");\n"; break;
      default: break;
    }
  }

  // Compute the chunk limit for byte-level lists
  static uint64_t ssz_limit_for_bytes(uint64_t max_bytes) {
    return (max_bytes + 31) / 32;
  }

  // Compute the chunk limit for element-level lists
  static uint64_t ssz_limit_for_elements(uint64_t max_elements,
                                          uint32_t elem_size) {
    if (elem_size >= 32) return max_elements;
    uint64_t elems_per_chunk = 32 / elem_size;
    return (max_elements + elems_per_chunk - 1) / elems_per_chunk;
  }
};

}  // namespace ssz_zig

// ---------------------------------------------------------------------------
// Code generator wrapper
// ---------------------------------------------------------------------------

static bool GenerateSszZig(const Parser &parser, const std::string &path,
                           const std::string &file_name) {
  ssz_zig::SszZigGenerator generator(parser, path, file_name);
  return generator.generate();
}

namespace {

class SszZigCodeGenerator : public CodeGenerator {
 public:
  Status GenerateCode(const Parser &parser, const std::string &path,
                      const std::string &filename) override {
    if (!GenerateSszZig(parser, path, filename)) return Status::ERROR;
    return Status::OK;
  }

  Status GenerateCode(const uint8_t * /*buf*/, int64_t /*length*/,
                      const CodeGenOptions & /*opts*/) override {
    return Status::NOT_IMPLEMENTED;
  }

  Status GenerateMakeRule(const Parser & /*parser*/,
                          const std::string & /*path*/,
                          const std::string & /*filename*/,
                          std::string & /*output*/) override {
    return Status::NOT_IMPLEMENTED;
  }

  Status GenerateGrpcCode(const Parser & /*parser*/,
                          const std::string & /*path*/,
                          const std::string & /*filename*/) override {
    return Status::NOT_IMPLEMENTED;
  }

  Status GenerateRootFile(const Parser & /*parser*/,
                          const std::string & /*path*/) override {
    return Status::NOT_IMPLEMENTED;
  }

  bool IsSchemaOnly() const override { return true; }

  bool SupportsBfbsGeneration() const override { return false; }

  bool SupportsRootFileGeneration() const override { return false; }

  IDLOptions::Language Language() const override {
    return IDLOptions::kSszZig;
  }

  std::string LanguageName() const override { return "SszZig"; }
};

}  // namespace

std::unique_ptr<CodeGenerator> NewSszZigCodeGenerator() {
  return std::unique_ptr<SszZigCodeGenerator>(new SszZigCodeGenerator());
}

}  // namespace flatbuffers
