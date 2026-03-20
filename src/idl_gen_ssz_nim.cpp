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

#include "idl_gen_ssz_nim.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "flatbuffers/base.h"
#include "flatbuffers/flatc.h"
#include "flatbuffers/code_generators.h"
#include "flatbuffers/flatbuffers.h"
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

namespace ssz_nim {

// ---------------------------------------------------------------------------
// Phase 2: SSZ Type Resolution (copied exactly from ssz_go)
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

static std::set<std::string> NimKeywords() {
  return {
      "addr",     "and",       "as",       "asm",       "bind",
      "block",    "break",     "case",     "cast",      "concept",
      "const",    "continue",  "converter","defer",     "discard",
      "distinct", "div",       "do",       "elif",      "else",
      "end",      "enum",      "except",   "export",    "finally",
      "for",      "from",      "func",     "if",        "import",
      "in",       "include",   "interface","is",        "isnot",
      "iterator", "let",       "macro",    "method",    "mixin",
      "mod",      "nil",       "not",      "notin",     "object",
      "of",       "or",        "out",      "proc",      "ptr",
      "raise",    "ref",       "return",   "shl",       "shr",
      "static",   "template",  "try",      "tuple",     "type",
      "using",    "var",       "when",     "while",     "xor",
      "yield",
  };
}

static Namer::Config SszNimDefaultConfig() {
  return {/*types=*/Case::kKeep,
          /*constants=*/Case::kUnknown,
          /*methods=*/Case::kLowerCamel,
          /*functions=*/Case::kLowerCamel,
          /*fields=*/Case::kLowerCamel,
          /*variables=*/Case::kLowerCamel,
          /*variants=*/Case::kKeep,
          /*enum_variant_seperator=*/"",
          /*escape_keywords=*/Namer::Config::Escape::AfterConvertingCase,
          /*namespaces=*/Case::kKeep,
          /*namespace_seperator=*/"_",
          /*object_prefix=*/"",
          /*object_suffix=*/"",
          /*keyword_prefix=*/"",
          /*keyword_suffix=*/"_",
          /*keywords_casing=*/Namer::Config::KeywordsCasing::CaseSensitive,
          /*filenames=*/Case::kKeep,
          /*directories=*/Case::kKeep,
          /*output_path=*/"",
          /*filename_suffix=*/"_ssz",
          /*filename_extension=*/".nim"};
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

// Convert first char to lower for Nim camelCase field names
static std::string ToCamelCase(const std::string &input) {
  if (input.empty()) return input;
  std::string result = input;
  result[0] = static_cast<char>(tolower(result[0]));
  return result;
}

// ---------------------------------------------------------------------------
// Main Generator
// ---------------------------------------------------------------------------

class SszNimGenerator : public BaseGenerator {
 public:
  SszNimGenerator(const Parser &parser, const std::string &path,
                  const std::string &file_name)
      : BaseGenerator(parser, path, file_name, "", "", "nim"),
        namer_(WithFlagOptions(SszNimDefaultConfig(), parser.opts, path),
               NimKeywords()) {}

  bool generate() override {
    // Collect all container infos for all structs in this schema
    std::vector<std::pair<const StructDef *, SszContainerInfo>> containers;

    for (auto it = parser_.structs_.vec.begin();
         it != parser_.structs_.vec.end(); ++it) {
      auto &struct_def = **it;
      if (struct_def.generated) continue;

      SszContainerInfo container;
      if (!AnalyzeContainer(struct_def, container)) return false;
      containers.push_back({&struct_def, std::move(container)});
    }

    if (containers.empty()) return true;

    // Generate all types into one file
    std::string code;
    for (size_t i = 0; i < containers.size(); i++) {
      auto &struct_def = *containers[i].first;
      auto &container = containers[i].second;

      if (!GenerateType(struct_def, container, &code)) return false;
      if (i + 1 < containers.size()) code += "\n";
    }

    return SaveFile(containers[0].first->defined_namespace, code);
  }

 private:
  const IdlNamer namer_;

  // -----------------------------------------------------------------------
  // Type resolution (copied exactly from ssz_go)
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
        // String maps to List[Uint8], requires ssz_max
        auto max_attr = attrs->Lookup("ssz_max");
        if (!max_attr) {
          flatbuffers::LogCompilerError("field '" + field.name +
                           "': string type requires ssz_max attribute");
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

      case BASE_TYPE_VECTOR: {
        return ResolveVectorType(field, type, *attrs, info);
      }

      case BASE_TYPE_ARRAY: {
        return ResolveArrayType(field, type, *attrs, info);
      }

      case BASE_TYPE_STRUCT: {
        info.ssz_type = SszType::Container;
        info.struct_def = type.struct_def;
        // Recursively analyze to determine if fixed
        SszContainerInfo nested;
        if (!AnalyzeContainer(*type.struct_def, nested)) return false;
        info.fixed_size = nested.is_fixed ? nested.static_size : 0;
        info.is_dynamic = !nested.is_fixed;
        break;
      }

      case BASE_TYPE_UNION: {
        info.ssz_type = SszType::Union;
        info.fixed_size = 0;
        info.is_dynamic = true;
        break;
      }

      case BASE_TYPE_FLOAT:
      case BASE_TYPE_DOUBLE:
        flatbuffers::LogCompilerError("field '" + field.name +
                         "': float/double types are not supported in SSZ");
        return false;

      default:
        flatbuffers::LogCompilerError("field '" + field.name +
                         "': unsupported base type for SSZ");
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
        flatbuffers::LogCompilerError("field '" + field.name +
                         "': ssz_bitlist requires ssz_max attribute");
        return false;
      }
      info.ssz_type = SszType::Bitlist;
      info.limit = StringToUInt(max_attr->constant.c_str());
      info.fixed_size = 0;
      info.is_dynamic = true;
      return true;
    }

    // Check for progressive list (EIP-7916) -- no ssz_max needed
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
      flatbuffers::LogCompilerError("field '" + field.name +
                       "': vector type requires ssz_max attribute");
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
        flatbuffers::LogCompilerError("field '" + field.name +
                         "': unsupported element type for SSZ vector/list");
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
  // Nim type helpers
  // -----------------------------------------------------------------------

  std::string NimTypeName(const SszFieldInfo &info, const FieldDef &field) {
    switch (info.ssz_type) {
      case SszType::Bool:
        return "bool";
      case SszType::Uint8:
        return "uint8";
      case SszType::Uint16:
        return "uint16";
      case SszType::Uint32:
        return "uint32";
      case SszType::Uint64:
        return "uint64";
      case SszType::Uint128:
        return "array[16, byte]";
      case SszType::Uint256:
        return "array[32, byte]";
      case SszType::Container:
        if (info.struct_def) { return info.struct_def->name; }
        return "UNKNOWN";
      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          return "array[" + NumToString(info.limit) + ", byte]";
        }
        if (info.elem_info) {
          return "array[" + NumToString(info.limit) + ", " +
                 NimElemTypeName(*info.elem_info) + "]";
        }
        return "UNKNOWN";
      }
      case SszType::ProgressiveList:
      case SszType::List: {
        if (field.value.type.base_type == BASE_TYPE_STRING) {
          return "seq[byte]";
        }
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          return "seq[byte]";
        }
        if (info.elem_info) {
          return "seq[" + NimElemTypeName(*info.elem_info) + "]";
        }
        return "seq[byte]";
      }
      case SszType::Bitlist:
        return "seq[byte]";
      case SszType::Bitvector:
        return "array[" + NumToString((info.bitsize + 7) / 8) + ", byte]";
      case SszType::ProgressiveContainer:
        if (info.struct_def) { return info.struct_def->name; }
        return "UNKNOWN";
      case SszType::Union:
        return "SszUnion";
    }
    return "UNKNOWN";
  }

  static std::string NimElemTypeName(const SszFieldInfo &info) {
    switch (info.ssz_type) {
      case SszType::Bool:
        return "bool";
      case SszType::Uint8:
        return "uint8";
      case SszType::Uint16:
        return "uint16";
      case SszType::Uint32:
        return "uint32";
      case SszType::Uint64:
        return "uint64";
      case SszType::Container:
        if (info.struct_def) {
          return info.struct_def->name;
        }
        return "UNKNOWN";
      case SszType::List:
        // Nested list (e.g. seq[seq[byte]] for Transactions)
        return "seq[byte]";
      default:
        return "UNKNOWN";
    }
  }

  std::string NimFieldName(const FieldDef &field) {
    return ToCamelCase(namer_.Field(field));
  }

  // -----------------------------------------------------------------------
  // Code generation entry point
  // -----------------------------------------------------------------------

  bool GenerateType(const StructDef &struct_def,
                    const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;

    // Generate type definition
    GenNimType(struct_def, container, &c);
    c += "\n";

    // Generate sszBytesLen
    GenSszBytesLen(struct_def, container, &c);
    c += "\n";

    // Generate marshalSSZTo
    GenMarshalSSZTo(struct_def, container, &c);
    c += "\n";

    // Generate marshalSSZ convenience
    GenMarshalSSZ(struct_def, &c);
    c += "\n";

    // Generate fromSSZBytes
    GenFromSSZBytes(struct_def, container, &c);
    c += "\n";

    // Generate hashTreeRoot
    GenHashTreeRoot(struct_def, &c);
    c += "\n";

    // Generate hashTreeRootWith
    GenHashTreeRootWith(struct_def, container, &c);

    return true;
  }

  // -----------------------------------------------------------------------
  // Nim type definition generation
  // -----------------------------------------------------------------------

  void GenNimType(const StructDef &struct_def,
                  const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    c += "type " + struct_def.name + "* = object\n";
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      auto &info = it->second;
      c += "  " + NimFieldName(*field) + "*: " +
           NimTypeName(info, *field) + "\n";
    }
  }

  // -----------------------------------------------------------------------
  // sszBytesLen generation
  // -----------------------------------------------------------------------

  void GenSszBytesLen(const StructDef &struct_def,
                      const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    std::string type_name = struct_def.name;

    c += "proc sszBytesLen*(t: " + type_name + "): int =\n";

    if (container.is_fixed) {
      c += "  result = " + NumToString(container.static_size) + "\n";
    } else {
      c += "  result = " + NumToString(container.static_size) + "\n";
      for (auto *field : container.dynamic_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "t." + NimFieldName(*field);
        GenSizeField(info, fname, &c, "  ");
      }
    }
  }

  void GenSizeField(const SszFieldInfo &info, const std::string &var,
                    std::string *code, const std::string &indent) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::ProgressiveList:
      case SszType::List: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          // List of byte lists: offsets + each element's length
          c += indent + "result += len(" + var + ") * 4\n";
          c += indent + "for item in " + var + ":\n";
          c += indent + "  result += len(item)\n";
        } else if (info.elem_info &&
            info.elem_info->ssz_type == SszType::Container &&
            info.elem_info->is_dynamic) {
          // Dynamic element list: offsets + each element's size
          c += indent + "result += len(" + var + ") * 4\n";
          c += indent + "for item in " + var + ":\n";
          c += indent + "  result += sszBytesLen(item)\n";
        } else if (info.elem_info) {
          c += indent + "result += len(" + var + ") * " +
               NumToString(info.elem_info->fixed_size) + "\n";
        } else {
          c += indent + "result += len(" + var + ")\n";
        }
        break;
      }
      case SszType::Bitlist:
        c += indent + "result += len(" + var + ")\n";
        break;
      case SszType::Container:
        c += indent + "result += sszBytesLen(" + var + ")\n";
        break;
      default:
        break;
    }
  }

  // -----------------------------------------------------------------------
  // marshalSSZ generation
  // -----------------------------------------------------------------------

  void GenMarshalSSZ(const StructDef &struct_def, std::string *code) {
    std::string &c = *code;
    std::string type_name = struct_def.name;
    c += "proc marshalSSZ*(t: " + type_name + "): seq[byte] =\n";
    c += "  result = newSeqOfCap[byte](sszBytesLen(t))\n";
    c += "  marshalSSZTo(t, result)\n";
  }

  void GenMarshalSSZTo(const StructDef &struct_def,
                       const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    std::string type_name = struct_def.name;

    c += "proc marshalSSZTo*(t: " + type_name +
         ", buf: var seq[byte]) =\n";

    if (container.dynamic_fields.empty()) {
      // All-fixed container: just marshal fields sequentially
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        GenMarshalField(it->second, "t." + NimFieldName(*field),
                        &c, "  ");
      }
    } else {
      // Container with dynamic fields
      c += "  let dstStart = len(buf)\n";
      c += "\n";

      // Static section: fixed fields inline, offsets for dynamic fields
      int offset_idx = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "t." + NimFieldName(*field);

        if (info.is_dynamic) {
          c += "  # Offset for '" + field->name + "'\n";
          c += "  let offsetPos" + NumToString(offset_idx) +
               " = len(buf)\n";
          c += "  buf.add([byte 0, 0, 0, 0])\n";
          offset_idx++;
        } else {
          c += "  # Field '" + field->name + "'\n";
          GenMarshalField(info, fname, &c, "  ");
        }
      }

      c += "\n";

      // Dynamic section: backpatch offsets and marshal dynamic fields
      offset_idx = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "t." + NimFieldName(*field);

        if (info.is_dynamic) {
          c += "  # Dynamic field '" + field->name + "'\n";
          c += "  block:\n";
          c += "    let offset = uint32(len(buf) - dstStart)\n";
          c += "    var tmp: array[4, byte]\n";
          c += "    littleEndian32(addr tmp[0], unsafeAddr offset)\n";
          c += "    copyMem(addr buf[offsetPos" +
               NumToString(offset_idx) + "], addr tmp[0], 4)\n";
          GenMarshalField(info, fname, &c, "  ");
          offset_idx++;
        }
      }
    }
  }

  void GenMarshalField(const SszFieldInfo &info, const std::string &var,
                       std::string *code, const std::string &indent) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::Bool:
        c += indent + "if " + var + ":\n";
        c += indent + "  buf.add(1'u8)\n";
        c += indent + "else:\n";
        c += indent + "  buf.add(0'u8)\n";
        break;

      case SszType::Uint8:
        c += indent + "buf.add(" + var + ")\n";
        break;

      case SszType::Uint16: {
        c += indent + "block:\n";
        c += indent + "  var tmp: array[2, byte]\n";
        c += indent + "  var v = " + var + "\n";
        c += indent + "  littleEndian16(addr tmp[0], addr v)\n";
        c += indent + "  buf.add(tmp)\n";
        break;
      }

      case SszType::Uint32: {
        c += indent + "block:\n";
        c += indent + "  var tmp: array[4, byte]\n";
        c += indent + "  var v = " + var + "\n";
        c += indent + "  littleEndian32(addr tmp[0], addr v)\n";
        c += indent + "  buf.add(tmp)\n";
        break;
      }

      case SszType::Uint64: {
        c += indent + "block:\n";
        c += indent + "  var tmp: array[8, byte]\n";
        c += indent + "  var v = " + var + "\n";
        c += indent + "  littleEndian64(addr tmp[0], addr v)\n";
        c += indent + "  buf.add(tmp)\n";
        break;
      }

      case SszType::Uint128:
      case SszType::Uint256:
        c += indent + "buf.add(" + var + ")\n";
        break;

      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "buf.add(" + var + ")\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "for i in 0 ..< len(" + var + "):\n";
          c += indent + "  marshalSSZTo(" + var + "[i], buf)\n";
        } else if (info.elem_info) {
          GenMarshalPrimitiveArray(info, var, code, indent);
        }
        break;
      }

      case SszType::ProgressiveList:
      case SszType::List: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          // List of byte lists: offset-based dynamic encoding
          c += indent + "block:\n";
          c += indent + "  let listStart = len(buf)\n";
          c += indent + "  for i in 0 ..< len(" + var + "):\n";
          c += indent + "    buf.add([byte 0, 0, 0, 0])\n";
          c += indent + "  for i in 0 ..< len(" + var + "):\n";
          c += indent + "    block:\n";
          c += indent + "      let offset = uint32(len(buf) - listStart)\n";
          c += indent + "      var tmp: array[4, byte]\n";
          c += indent +
               "      littleEndian32(addr tmp[0], unsafeAddr offset)\n";
          c += indent +
               "      copyMem(addr buf[listStart + i * 4], addr tmp[0], 4)\n";
          c += indent + "    buf.add(" + var + "[i])\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "buf.add(" + var + ")\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          if (info.elem_info->is_dynamic) {
            // Dynamic element list: write offsets then data
            c += indent + "block:\n";
            c += indent + "  let listStart = len(buf)\n";
            c += indent + "  for i in 0 ..< len(" + var + "):\n";
            c += indent + "    buf.add([byte 0, 0, 0, 0])\n";
            c += indent + "  for i in 0 ..< len(" + var + "):\n";
            c += indent + "    block:\n";
            c += indent +
                 "      let offset = uint32(len(buf) - listStart)\n";
            c += indent + "      var tmp: array[4, byte]\n";
            c += indent +
                 "      littleEndian32(addr tmp[0], unsafeAddr offset)\n";
            c += indent +
                 "      copyMem(addr buf[listStart + i * 4],"
                 " addr tmp[0], 4)\n";
            c += indent + "    marshalSSZTo(" + var + "[i], buf)\n";
          } else {
            c += indent + "for item in " + var + ":\n";
            c += indent + "  marshalSSZTo(item, buf)\n";
          }
        } else if (info.elem_info) {
          GenMarshalPrimitiveSlice(info, var, code, indent);
        } else {
          c += indent + "buf.add(" + var + ")\n";
        }
        break;
      }

      case SszType::Bitlist:
        c += indent + "buf.add(" + var + ")\n";
        break;

      case SszType::Bitvector:
        c += indent + "buf.add(" + var + ")\n";
        break;

      case SszType::Container:
      case SszType::ProgressiveContainer:
        c += indent + "marshalSSZTo(" + var + ", buf)\n";
        break;

      case SszType::Union:
        // Union marshaling would go here
        break;
    }
  }

  void GenMarshalPrimitiveArray(const SszFieldInfo &info,
                                const std::string &var, std::string *code,
                                const std::string &indent) {
    std::string &c = *code;
    c += indent + "for i in 0 ..< len(" + var + "):\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "  if " + var + "[i]:\n";
        c += indent + "    buf.add(1'u8)\n";
        c += indent + "  else:\n";
        c += indent + "    buf.add(0'u8)\n";
        break;
      case SszType::Uint16:
        c += indent + "  block:\n";
        c += indent + "    var tmp: array[2, byte]\n";
        c += indent + "    var v = " + var + "[i]\n";
        c += indent + "    littleEndian16(addr tmp[0], addr v)\n";
        c += indent + "    buf.add(tmp)\n";
        break;
      case SszType::Uint32:
        c += indent + "  block:\n";
        c += indent + "    var tmp: array[4, byte]\n";
        c += indent + "    var v = " + var + "[i]\n";
        c += indent + "    littleEndian32(addr tmp[0], addr v)\n";
        c += indent + "    buf.add(tmp)\n";
        break;
      case SszType::Uint64:
        c += indent + "  block:\n";
        c += indent + "    var tmp: array[8, byte]\n";
        c += indent + "    var v = " + var + "[i]\n";
        c += indent + "    littleEndian64(addr tmp[0], addr v)\n";
        c += indent + "    buf.add(tmp)\n";
        break;
      default:
        break;
    }
  }

  void GenMarshalPrimitiveSlice(const SszFieldInfo &info,
                                const std::string &var, std::string *code,
                                const std::string &indent) {
    std::string &c = *code;
    c += indent + "for v in " + var + ":\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "  if v:\n";
        c += indent + "    buf.add(1'u8)\n";
        c += indent + "  else:\n";
        c += indent + "    buf.add(0'u8)\n";
        break;
      case SszType::Uint16:
        c += indent + "  block:\n";
        c += indent + "    var tmp: array[2, byte]\n";
        c += indent + "    var vv = v\n";
        c += indent + "    littleEndian16(addr tmp[0], addr vv)\n";
        c += indent + "    buf.add(tmp)\n";
        break;
      case SszType::Uint32:
        c += indent + "  block:\n";
        c += indent + "    var tmp: array[4, byte]\n";
        c += indent + "    var vv = v\n";
        c += indent + "    littleEndian32(addr tmp[0], addr vv)\n";
        c += indent + "    buf.add(tmp)\n";
        break;
      case SszType::Uint64:
        c += indent + "  block:\n";
        c += indent + "    var tmp: array[8, byte]\n";
        c += indent + "    var vv = v\n";
        c += indent + "    littleEndian64(addr tmp[0], addr vv)\n";
        c += indent + "    buf.add(tmp)\n";
        break;
      default:
        break;
    }
  }

  // -----------------------------------------------------------------------
  // fromSSZBytes generation
  // -----------------------------------------------------------------------

  void GenFromSSZBytes(const StructDef &struct_def,
                       const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    std::string type_name = struct_def.name;

    c += "proc fromSSZBytes*(T: typedesc[" + type_name +
         "], data: openArray[byte]): " + type_name + " =\n";

    // Validate minimum size
    c += "  if len(data) < " + NumToString(container.static_size) + ":\n";
    c += "    raise newException(SszError, \"buffer too small\")\n";

    if (container.dynamic_fields.empty()) {
      // All-fixed container
      uint32_t offset = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "result." + NimFieldName(*field);
        GenUnmarshalField(info, fname, offset, &c, "  ");
        offset += info.fixed_size;
      }
    } else {
      // Container with dynamic fields
      uint32_t offset = 0;
      int dyn_idx = 0;
      std::vector<std::pair<const FieldDef *, int>> dyn_offset_names;

      // Read static fields and offsets
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "result." + NimFieldName(*field);

        if (info.is_dynamic) {
          c += "  var offset" + NumToString(dyn_idx) + ": uint32\n";
          c += "  littleEndian32(addr offset" + NumToString(dyn_idx) +
               ", unsafeAddr data[" + NumToString(offset) + "])\n";
          c += "  let off" + NumToString(dyn_idx) +
               " = int(offset" + NumToString(dyn_idx) + ")\n";
          dyn_offset_names.push_back({field, dyn_idx});

          // Validate first offset equals static size
          if (dyn_idx == 0) {
            c += "  if off0 != " + NumToString(container.static_size) +
                 ":\n";
            c += "    raise newException(SszError, \"invalid offset\")\n";
          } else {
            c += "  if off" + NumToString(dyn_idx) + " < off" +
                 NumToString(dyn_idx - 1) + " or off" +
                 NumToString(dyn_idx) + " > len(data):\n";
            c += "    raise newException(SszError, \"invalid offset\")\n";
          }
          offset += 4;
          dyn_idx++;
        } else {
          GenUnmarshalField(info, fname, offset, &c, "  ");
          offset += info.fixed_size;
        }
      }

      c += "\n";

      // Unmarshal dynamic fields using offset ranges
      for (size_t i = 0; i < dyn_offset_names.size(); i++) {
        auto *field = dyn_offset_names[i].first;
        int oidx = dyn_offset_names[i].second;
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "result." + NimFieldName(*field);

        std::string start_off = "off" + NumToString(oidx);
        std::string end_off;
        if (i + 1 < dyn_offset_names.size()) {
          end_off = "off" + NumToString(dyn_offset_names[i + 1].second);
        } else {
          end_off = "len(data)";
        }

        c += "  block:\n";
        c += "    let dynData = data.toOpenArray(" + start_off + ", " +
             end_off + " - 1)\n";
        GenUnmarshalDynField(info, fname, *field, &c, "    ");
      }
    }
  }

  void GenUnmarshalField(const SszFieldInfo &info, const std::string &var,
                         uint32_t offset, std::string *code,
                         const std::string &indent) {
    std::string &c = *code;
    std::string off_str = NumToString(offset);

    switch (info.ssz_type) {
      case SszType::Bool:
        c += indent + "if data[" + off_str + "] > 1'u8:\n";
        c += indent + "  raise newException(SszError, \"invalid bool\")\n";
        c += indent + var + " = data[" + off_str + "] == 1'u8\n";
        break;

      case SszType::Uint8:
        c += indent + var + " = data[" + off_str + "]\n";
        break;

      case SszType::Uint16:
        c += indent + "littleEndian16(addr " + var +
             ", unsafeAddr data[" + off_str + "])\n";
        break;

      case SszType::Uint32:
        c += indent + "littleEndian32(addr " + var +
             ", unsafeAddr data[" + off_str + "])\n";
        break;

      case SszType::Uint64:
        c += indent + "littleEndian64(addr " + var +
             ", unsafeAddr data[" + off_str + "])\n";
        break;

      case SszType::Uint128:
      case SszType::Uint256:
        c += indent + "copyMem(addr " + var + "[0], unsafeAddr data[" +
             off_str + "], " + NumToString(info.fixed_size) + ")\n";
        break;

      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "copyMem(addr " + var + "[0], unsafeAddr data[" +
               off_str + "], " + NumToString(info.fixed_size) + ")\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          uint32_t elem_size = info.elem_info->fixed_size;
          c += indent + "for i in 0 ..< " + NumToString(info.limit) +
               ":\n";
          c += indent + "  let start = " + off_str + " + i * " +
               NumToString(elem_size) + "\n";
          c += indent + "  " + var + "[i] = fromSSZBytes(" +
               info.elem_info->struct_def->name +
               ", data.toOpenArray(start, start + " +
               NumToString(elem_size) + " - 1))\n";
        } else if (info.elem_info) {
          GenUnmarshalPrimitiveArray(info, var, offset, code, indent);
        }
        break;
      }

      case SszType::Bitvector:
        c += indent + "copyMem(addr " + var + "[0], unsafeAddr data[" +
             off_str + "], " + NumToString(info.fixed_size) + ")\n";
        break;

      case SszType::Container: {
        c += indent + var + " = fromSSZBytes(" +
             info.struct_def->name + ", data.toOpenArray(" + off_str +
             ", " + NumToString(offset + info.fixed_size) + " - 1))\n";
        break;
      }

      default:
        break;
    }
  }

  void GenUnmarshalPrimitiveArray(const SszFieldInfo &info,
                                  const std::string &var, uint32_t offset,
                                  std::string *code,
                                  const std::string &indent) {
    std::string &c = *code;
    uint32_t elem_size = info.elem_info->fixed_size;
    c += indent + "for i in 0 ..< " + NumToString(info.limit) + ":\n";
    c += indent + "  let off = " + NumToString(offset) + " + i * " +
         NumToString(elem_size) + "\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "  if data[off] > 1'u8:\n";
        c += indent +
             "    raise newException(SszError, \"invalid bool\")\n";
        c += indent + "  " + var + "[i] = data[off] == 1'u8\n";
        break;
      case SszType::Uint16:
        c += indent + "  littleEndian16(addr " + var +
             "[i], unsafeAddr data[off])\n";
        break;
      case SszType::Uint32:
        c += indent + "  littleEndian32(addr " + var +
             "[i], unsafeAddr data[off])\n";
        break;
      case SszType::Uint64:
        c += indent + "  littleEndian64(addr " + var +
             "[i], unsafeAddr data[off])\n";
        break;
      default:
        break;
    }
  }

  void GenUnmarshalDynField(const SszFieldInfo &info, const std::string &var,
                            const FieldDef & /*field*/, std::string *code,
                            const std::string &indent) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::ProgressiveList:
      case SszType::List: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          // List of byte lists (seq[seq[byte]])
          c += indent + "if len(dynData) > 0:\n";
          c += indent + "  var firstOff: uint32\n";
          c += indent +
               "  littleEndian32(addr firstOff, unsafeAddr dynData[0])\n";
          c += indent + "  if int(firstOff) mod 4 != 0:\n";
          c += indent +
               "    raise newException(SszError, \"invalid offset\")\n";
          c += indent + "  let count = int(firstOff) div 4\n";
          c += indent + "  " + var + " = newSeq[seq[byte]](count)\n";
          c += indent + "  for i in 0 ..< count:\n";
          c += indent + "    var startOff: uint32\n";
          c += indent +
               "    littleEndian32(addr startOff,"
               " unsafeAddr dynData[i * 4])\n";
          c += indent + "    var endOff: int\n";
          c += indent + "    if i + 1 < count:\n";
          c += indent + "      var nextOff: uint32\n";
          c += indent +
               "      littleEndian32(addr nextOff,"
               " unsafeAddr dynData[(i + 1) * 4])\n";
          c += indent + "      endOff = int(nextOff)\n";
          c += indent + "    else:\n";
          c += indent + "      endOff = len(dynData)\n";
          c += indent +
               "    if int(startOff) > endOff or"
               " endOff > len(dynData):\n";
          c += indent +
               "      raise newException(SszError, \"invalid offset\")\n";
          c += indent + "    " + var +
               "[i] = @(dynData.toOpenArray(int(startOff),"
               " endOff - 1))\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + var + " = @(dynData)\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          if (info.elem_info->is_dynamic) {
            // Dynamic element list
            c += indent + "if len(dynData) > 0:\n";
            c += indent + "  var firstOff: uint32\n";
            c += indent +
                 "  littleEndian32(addr firstOff,"
                 " unsafeAddr dynData[0])\n";
            c += indent + "  if int(firstOff) mod 4 != 0:\n";
            c += indent +
                 "    raise newException(SszError, \"invalid offset\")\n";
            c += indent + "  let count = int(firstOff) div 4\n";
            c += indent + "  " + var + " = newSeq[" +
                 NimElemTypeName(*info.elem_info) + "](count)\n";
            c += indent + "  for i in 0 ..< count:\n";
            c += indent + "    var startOff: uint32\n";
            c += indent +
                 "    littleEndian32(addr startOff,"
                 " unsafeAddr dynData[i * 4])\n";
            c += indent + "    var endOff: int\n";
            c += indent + "    if i + 1 < count:\n";
            c += indent + "      var nextOff: uint32\n";
            c += indent +
                 "      littleEndian32(addr nextOff,"
                 " unsafeAddr dynData[(i + 1) * 4])\n";
            c += indent + "      endOff = int(nextOff)\n";
            c += indent + "    else:\n";
            c += indent + "      endOff = len(dynData)\n";
            c += indent +
                 "    if int(startOff) > endOff or"
                 " endOff > len(dynData):\n";
            c += indent +
                 "      raise newException(SszError,"
                 " \"invalid offset\")\n";
            c += indent + "    " + var + "[i] = fromSSZBytes(" +
                 info.elem_info->struct_def->name +
                 ", dynData.toOpenArray(int(startOff), endOff - 1))\n";
          } else {
            uint32_t elem_size = info.elem_info->fixed_size;
            c += indent + "let count = len(dynData) div " +
                 NumToString(elem_size) + "\n";
            c += indent + var + " = newSeq[" +
                 NimElemTypeName(*info.elem_info) + "](count)\n";
            c += indent + "for i in 0 ..< count:\n";
            c += indent + "  let off = i * " +
                 NumToString(elem_size) + "\n";
            c += indent + "  " + var + "[i] = fromSSZBytes(" +
                 info.elem_info->struct_def->name +
                 ", dynData.toOpenArray(off, off + " +
                 NumToString(elem_size) + " - 1))\n";
          }
        } else if (info.elem_info) {
          GenUnmarshalPrimitiveSlice(info, var, code, indent);
        } else {
          c += indent + var + " = @(dynData)\n";
        }
        break;
      }
      case SszType::Bitlist:
        c += indent + var + " = @(dynData)\n";
        break;
      case SszType::Container:
        c += indent + var + " = fromSSZBytes(" +
             info.struct_def->name + ", dynData)\n";
        break;
      default:
        break;
    }
  }

  void GenUnmarshalPrimitiveSlice(const SszFieldInfo &info,
                                  const std::string &var,
                                  std::string *code,
                                  const std::string &indent) {
    std::string &c = *code;
    uint32_t elem_size = info.elem_info->fixed_size;
    c += indent + "let count = len(dynData) div " +
         NumToString(elem_size) + "\n";

    std::string nim_elem_type;
    switch (info.elem_info->ssz_type) {
      case SszType::Bool: nim_elem_type = "bool"; break;
      case SszType::Uint16: nim_elem_type = "uint16"; break;
      case SszType::Uint32: nim_elem_type = "uint32"; break;
      case SszType::Uint64: nim_elem_type = "uint64"; break;
      default: nim_elem_type = "uint8"; break;
    }

    c += indent + var + " = newSeq[" + nim_elem_type + "](count)\n";
    c += indent + "for i in 0 ..< count:\n";
    c += indent + "  let off = i * " + NumToString(elem_size) + "\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "  if dynData[off] > 1'u8:\n";
        c += indent +
             "    raise newException(SszError, \"invalid bool\")\n";
        c += indent + "  " + var + "[i] = dynData[off] == 1'u8\n";
        break;
      case SszType::Uint16:
        c += indent + "  littleEndian16(addr " + var +
             "[i], unsafeAddr dynData[off])\n";
        break;
      case SszType::Uint32:
        c += indent + "  littleEndian32(addr " + var +
             "[i], unsafeAddr dynData[off])\n";
        break;
      case SszType::Uint64:
        c += indent + "  littleEndian64(addr " + var +
             "[i], unsafeAddr dynData[off])\n";
        break;
      default:
        break;
    }
  }

  // -----------------------------------------------------------------------
  // hashTreeRoot generation
  // -----------------------------------------------------------------------

  void GenHashTreeRoot(const StructDef &struct_def, std::string *code) {
    std::string &c = *code;
    std::string type_name = struct_def.name;
    c += "proc hashTreeRoot*(t: " + type_name +
         "): array[32, byte] =\n";
    c += "  var h = HasherPool.get()\n";
    c += "  defer: HasherPool.put(h)\n";
    c += "  hashTreeRootWith(t, h)\n";
    c += "  result = h.hashRoot()\n";
  }

  void GenHashTreeRootWith(const StructDef &struct_def,
                           const SszContainerInfo &container,
                           std::string *code) {
    std::string &c = *code;
    std::string type_name = struct_def.name;

    c += "proc hashTreeRootWith*(t: " + type_name +
         ", h: var Hasher) =\n";
    c += "  let idx = h.index()\n";
    c += "\n";

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
          c += "  # Index " + NumToString(i) + ": " + field->name + "\n";
          GenHashField(iit->second, "t." + NimFieldName(*field),
                       &c, "  ");
        } else {
          c += "  # Index " + NumToString(i) + ": gap (inactive)\n";
          c += "  h.putUint8(0'u8)\n";
        }
      }
      c += "\n";

      // Emit active_fields bitvector literal
      c += "  h.merkleizeProgressiveWithActiveFields(idx, @[";
      for (size_t i = 0; i < container.active_fields_bitvector.size();
           i++) {
        if (i > 0) c += ", ";
        char hex[8];
        snprintf(hex, sizeof(hex), "0x%02x'u8",
                 container.active_fields_bitvector[i]);
        c += hex;
      }
      c += "])\n";
    } else {
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "t." + NimFieldName(*field);

        c += "  # Field '" + field->name + "'\n";
        GenHashField(info, fname, &c, "  ");
        c += "\n";
      }

      c += "  h.merkleize(idx)\n";
    }
  }

  void GenHashField(const SszFieldInfo &info, const std::string &var,
                    std::string *code, const std::string &indent) {
    std::string &c = *code;

    switch (info.ssz_type) {
      case SszType::Bool:
        c += indent + "h.putBool(" + var + ")\n";
        break;

      case SszType::Uint8:
        c += indent + "h.putUint8(" + var + ")\n";
        break;

      case SszType::Uint16:
        c += indent + "h.putUint16(" + var + ")\n";
        break;

      case SszType::Uint32:
        c += indent + "h.putUint32(" + var + ")\n";
        break;

      case SszType::Uint64:
        c += indent + "h.putUint64(" + var + ")\n";
        break;

      case SszType::Uint128:
      case SszType::Uint256:
        c += indent + "h.putBytes(" + var + ")\n";
        break;

      case SszType::Vector: {
        if (info.elem_info &&
            info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "h.putBytes(" + var + ")\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "block:\n";
          c += indent + "  let subIdx = h.index()\n";
          c += indent + "  for i in 0 ..< len(" + var + "):\n";
          c += indent + "    hashTreeRootWith(" + var + "[i], h)\n";
          c += indent + "  h.merkleize(subIdx)\n";
        } else if (info.elem_info) {
          c += indent + "block:\n";
          c += indent + "  let subIdx = h.index()\n";
          GenHashPrimitiveArray(info, var, code, indent + "  ");
          c += indent + "  h.fillUpTo32()\n";
          c += indent + "  h.merkleize(subIdx)\n";
        }
        break;
      }

      case SszType::List: {
        uint64_t limit = info.limit;
        if (info.elem_info &&
            info.elem_info->ssz_type == SszType::List) {
          // List of byte lists: hash each inner list, then merkleize
          uint64_t inner_limit = info.elem_info->limit;
          uint64_t inner_chunk_limit =
              inner_limit > 0 ? ssz_limit_for_bytes(inner_limit) : 0;
          c += indent + "block:\n";
          c += indent + "  let subIdx = h.index()\n";
          c += indent + "  for item in " + var + ":\n";
          c += indent + "    let inner = h.index()\n";
          c += indent + "    h.appendBytes32(item)\n";
          if (inner_chunk_limit > 0) {
            c += indent +
                 "    h.merkleizeWithMixin(inner, uint64(len(item)), " +
                 NumToString(inner_chunk_limit) + "'u64)\n";
          } else {
            c += indent + "    h.merkleize(inner)\n";
          }
          c += indent + "  h.merkleizeWithMixin(subIdx, uint64(len(" +
               var + ")), " + NumToString(limit) + "'u64)\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "block:\n";
          c += indent + "  let subIdx = h.index()\n";
          c += indent + "  h.appendBytes32(" + var + ")\n";
          c += indent + "  h.merkleizeWithMixin(subIdx, uint64(len(" +
               var + ")), " +
               NumToString(ssz_limit_for_bytes(limit)) + "'u64)\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "block:\n";
          c += indent + "  let subIdx = h.index()\n";
          c += indent + "  for item in " + var + ":\n";
          c += indent + "    hashTreeRootWith(item, h)\n";
          c += indent + "  h.merkleizeWithMixin(subIdx, uint64(len(" +
               var + ")), " + NumToString(limit) + "'u64)\n";
        } else if (info.elem_info) {
          c += indent + "block:\n";
          c += indent + "  let subIdx = h.index()\n";
          GenHashPrimitiveSlice(info, var, code, indent + "  ");
          c += indent + "  h.fillUpTo32()\n";
          uint64_t hash_limit =
              ssz_limit_for_elements(limit, info.elem_info->fixed_size);
          c += indent + "  h.merkleizeWithMixin(subIdx, uint64(len(" +
               var + ")), " + NumToString(hash_limit) + "'u64)\n";
        }
        break;
      }

      case SszType::Bitlist:
        c += indent + "h.putBitlist(" + var + ", " +
             NumToString(info.limit) + "'u64)\n";
        break;

      case SszType::Bitvector:
        c += indent + "h.putBytes(" + var + ")\n";
        break;

      case SszType::Container:
        c += indent + "hashTreeRootWith(" + var + ", h)\n";
        break;

      case SszType::ProgressiveContainer:
        c += indent + "hashTreeRootWith(" + var + ", h)\n";
        break;

      case SszType::ProgressiveList: {
        // EIP-7916: use merkleizeProgressiveWithMixin
        if (info.elem_info &&
            info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "block:\n";
          c += indent + "  let subIdx = h.index()\n";
          c += indent + "  h.appendBytes32(" + var + ")\n";
          c += indent +
               "  h.merkleizeProgressiveWithMixin(subIdx, uint64(len(" +
               var + ")))\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "block:\n";
          c += indent + "  let subIdx = h.index()\n";
          c += indent + "  for item in " + var + ":\n";
          c += indent + "    hashTreeRootWith(item, h)\n";
          c += indent +
               "  h.merkleizeProgressiveWithMixin(subIdx, uint64(len(" +
               var + ")))\n";
        } else if (info.elem_info) {
          c += indent + "block:\n";
          c += indent + "  let subIdx = h.index()\n";
          GenHashPrimitiveSlice(info, var, code, indent + "  ");
          c += indent + "  h.fillUpTo32()\n";
          c += indent +
               "  h.merkleizeProgressiveWithMixin(subIdx, uint64(len(" +
               var + ")))\n";
        }
        break;
      }

      case SszType::Union:
        break;
    }
  }

  void GenHashPrimitiveArray(const SszFieldInfo &info,
                             const std::string &var,
                             std::string *code,
                             const std::string &indent) {
    std::string &c = *code;
    c += indent + "for i in 0 ..< len(" + var + "):\n";
    std::string elem_var = var + "[i]";
    GenHashPrimitive(info.elem_info->ssz_type, elem_var, code,
                     indent + "  ", true);
  }

  void GenHashPrimitiveSlice(const SszFieldInfo &info,
                             const std::string &var,
                             std::string *code,
                             const std::string &indent) {
    std::string &c = *code;
    c += indent + "for v in " + var + ":\n";
    GenHashPrimitive(info.elem_info->ssz_type, "v", code,
                     indent + "  ", true);
  }

  void GenHashPrimitive(SszType ssz_type, const std::string &var,
                        std::string *code, const std::string &indent,
                        bool append) {
    std::string &c = *code;
    std::string method = append ? "append" : "put";
    switch (ssz_type) {
      case SszType::Bool:
        c += indent + "h." + method + "Bool(" + var + ")\n";
        break;
      case SszType::Uint8:
        c += indent + "h." + method + "Uint8(" + var + ")\n";
        break;
      case SszType::Uint16:
        c += indent + "h." + method + "Uint16(" + var + ")\n";
        break;
      case SszType::Uint32:
        c += indent + "h." + method + "Uint32(" + var + ")\n";
        break;
      case SszType::Uint64:
        c += indent + "h." + method + "Uint64(" + var + ")\n";
        break;
      default:
        break;
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

  // -----------------------------------------------------------------------
  // File saving
  // -----------------------------------------------------------------------

  bool SaveFile(const Namespace *ns, const std::string &classcode) {
    if (classcode.empty()) return true;

    std::string code;
    code +=
        "# Code generated by the FlatBuffers compiler."
        " DO NOT EDIT.\n\n";
    code += "import std/endians\n";
    code += "import ssz_runtime\n\n";

    code += classcode;

    // Strip trailing double newlines
    while (code.length() > 2 &&
           code.substr(code.length() - 2) == "\n\n") {
      code.pop_back();
    }

    std::string directory;
    if (ns) {
      directory = namer_.Directories(*ns);
    }
    if (directory.empty()) {
      directory = path_;
    }
    EnsureDirExists(directory);

    // Use the schema file name as the output file name
    std::string snake_name = ToSnakeCase(file_name_);
    std::string filename = directory + snake_name + "_ssz.nim";
    return parser_.opts.file_saver->SaveFile(
        filename.c_str(), code, false);
  }
};

}  // namespace ssz_nim

// ---------------------------------------------------------------------------
// Code generator wrapper
// ---------------------------------------------------------------------------

static bool GenerateSszNim(const Parser &parser, const std::string &path,
                           const std::string &file_name) {
  ssz_nim::SszNimGenerator generator(parser, path, file_name);
  return generator.generate();
}

namespace {

class SszNimCodeGenerator : public CodeGenerator {
 public:
  Status GenerateCode(const Parser &parser, const std::string &path,
                      const std::string &filename) override {
    if (!GenerateSszNim(parser, path, filename)) { return Status::ERROR; }
    return Status::OK;
  }

  Status GenerateCode(const uint8_t * /*buffer*/, int64_t /*length*/,
                      const CodeGenOptions & /*options*/) override {
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
    return IDLOptions::kSszNim;
  }

  std::string LanguageName() const override { return "SszNim"; }
};

}  // namespace

std::unique_ptr<CodeGenerator> NewSszNimCodeGenerator() {
  return std::unique_ptr<SszNimCodeGenerator>(
      new SszNimCodeGenerator());
}

}  // namespace flatbuffers
