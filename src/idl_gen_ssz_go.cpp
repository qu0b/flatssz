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

#include "idl_gen_ssz_go.h"

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

namespace ssz_go {

// ---------------------------------------------------------------------------
// Phase 2: SSZ Type Resolution
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

static std::set<std::string> GoKeywords() {
  return {
      "break",    "default",     "func",   "interface", "select",
      "case",     "defer",       "go",     "map",       "struct",
      "chan",      "else",        "goto",   "package",   "switch",
      "const",    "fallthrough", "if",     "range",     "type",
      "continue", "for",         "import", "return",    "var",
  };
}

static Namer::Config SszGoDefaultConfig() {
  return {/*types=*/Case::kKeep,
          /*constants=*/Case::kUnknown,
          /*methods=*/Case::kUpperCamel,
          /*functions=*/Case::kUpperCamel,
          /*fields=*/Case::kUpperCamel,
          /*variables=*/Case::kLowerCamel,
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
          /*filenames=*/Case::kKeep,
          /*directories=*/Case::kKeep,
          /*output_path=*/"",
          /*filename_suffix=*/"_ssz",
          /*filename_extension=*/".go"};
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
// Main Generator
// ---------------------------------------------------------------------------

class SszGoGenerator : public BaseGenerator {
 public:
  SszGoGenerator(const Parser &parser, const std::string &path,
                 const std::string &file_name)
      : BaseGenerator(parser, path, file_name, "", "", "go"),
        namer_(WithFlagOptions(SszGoDefaultConfig(), parser.opts, path),
               GoKeywords()) {}

  bool generate() override {
    for (auto it = parser_.structs_.vec.begin();
         it != parser_.structs_.vec.end(); ++it) {
      auto &struct_def = **it;
      if (struct_def.generated) continue;

      // Resolve SSZ container info
      SszContainerInfo container;
      if (!AnalyzeContainer(struct_def, container)) return false;

      // Generate code
      std::string code;
      if (!GenerateType(struct_def, container, &code)) return false;

      if (!SaveType(struct_def, code)) return false;
    }
    return true;
  }

 private:
  const IdlNamer namer_;

  // -----------------------------------------------------------------------
  // Type resolution
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

    // Fixed-size array → SSZ Vector
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
        // String element in a vector → List[List[byte, inner_max], outer_max]
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
  // Go type helpers
  // -----------------------------------------------------------------------

  std::string GoTypeName(const SszFieldInfo &info, const FieldDef &field) {
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
        return "[16]byte";
      case SszType::Uint256:
        return "[32]byte";
      case SszType::Container:
        if (info.struct_def) { return info.struct_def->name; }
        return "UNKNOWN";
      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          return "[" + NumToString(info.limit) + "]byte";
        }
        if (info.elem_info) {
          return "[" + NumToString(info.limit) + "]" +
                 GoElemTypeName(*info.elem_info);
        }
        return "UNKNOWN";
      }
      case SszType::ProgressiveList:
      case SszType::List: {
        // String fields map to []byte in SSZ Go
        if (field.value.type.base_type == BASE_TYPE_STRING) {
          return "[]byte";
        }
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          return "[]byte";
        }
        if (info.elem_info) {
          return "[]" + GoElemTypeName(*info.elem_info);
        }
        return "[]byte";
      }
      case SszType::Bitlist:
        return "[]byte";
      case SszType::Bitvector:
        return "[" + NumToString((info.bitsize + 7) / 8) + "]byte";
      case SszType::ProgressiveContainer:
        if (info.struct_def) { return info.struct_def->name; }
        return "UNKNOWN";
      case SszType::Union:
        return "interface{}";
    }
    return "UNKNOWN";
  }

  std::string GoElemTypeName(const SszFieldInfo &info) {
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
          return "*" + info.struct_def->name;
        }
        return "UNKNOWN";
      case SszType::List:
        // Nested list (e.g. [][]byte for Transactions)
        return "[]byte";
      default:
        return "UNKNOWN";
    }
  }

  // -----------------------------------------------------------------------
  // Code generation entry point
  // -----------------------------------------------------------------------

  bool GenerateType(const StructDef &struct_def,
                    const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;

    // Generate struct definition
    GenGoStruct(struct_def, container, &c);
    c += "\n";

    // Generate SizeSSZ
    GenSizeSSZ(struct_def, container, &c);
    c += "\n";

    // Generate MarshalSSZTo
    GenMarshalSSZTo(struct_def, container, &c);
    c += "\n";

    // Generate MarshalSSZ convenience
    GenMarshalSSZ(struct_def, &c);
    c += "\n";

    // Generate UnmarshalSSZ
    GenUnmarshalSSZ(struct_def, container, &c);
    c += "\n";

    // Generate HashTreeRoot
    GenHashTreeRoot(struct_def, &c);
    c += "\n";

    // Generate HashTreeRootWith
    GenHashTreeRootWith(struct_def, container, &c);

    return true;
  }

  // -----------------------------------------------------------------------
  // Go struct generation
  // -----------------------------------------------------------------------

  void GenGoStruct(const StructDef &struct_def,
                   const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    c += "type " + struct_def.name + " struct {\n";
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      auto &info = it->second;
      c += "\t" + namer_.Field(*field) + " " + GoTypeName(info, *field);
      c += " `ssz:\"" + field->name + "\"`\n";
    }
    c += "}\n";
  }

  // -----------------------------------------------------------------------
  // SizeSSZ generation
  // -----------------------------------------------------------------------

  void GenSizeSSZ(const StructDef &struct_def,
                  const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    std::string type_name = struct_def.name;

    c += "func (t *" + type_name + ") SizeSSZ() int {\n";

    if (container.is_fixed) {
      c += "\treturn " + NumToString(container.static_size) + "\n";
    } else {
      c += "\tsize := " + NumToString(container.static_size) + "\n";
      for (auto *field : container.dynamic_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "t." + namer_.Field(*field);
        GenSizeField(info, fname, *field, &c, "\t");
      }
      c += "\treturn size\n";
    }
    c += "}\n";
  }

  void GenSizeField(const SszFieldInfo &info, const std::string &var,
                    const FieldDef & /*field*/, std::string *code,
                    const std::string &indent) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::ProgressiveList:
      case SszType::List: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          // List of byte lists ([][]byte): offsets + each element's length
          c += indent + "size += len(" + var + ") * 4\n";
          c += indent + "for _, item := range " + var + " {\n";
          c += indent + "\tsize += len(item)\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
            info.elem_info->ssz_type == SszType::Container &&
            info.elem_info->is_dynamic) {
          // Dynamic element list: offsets + each element's size
          c += indent + "size += len(" + var + ") * 4\n";
          c += indent + "for _, item := range " + var + " {\n";
          c += indent + "\tsize += item.SizeSSZ()\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          c += indent + "size += len(" + var + ") * " +
               NumToString(info.elem_info->fixed_size) + "\n";
        } else {
          c += indent + "size += len(" + var + ")\n";
        }
        break;
      }
      case SszType::Bitlist:
        c += indent + "size += len(" + var + ")\n";
        break;
      case SszType::Container:
        c += indent + "size += " + var + ".SizeSSZ()\n";
        break;
      default:
        break;
    }
  }

  // -----------------------------------------------------------------------
  // MarshalSSZ generation
  // -----------------------------------------------------------------------

  void GenMarshalSSZ(const StructDef &struct_def, std::string *code) {
    std::string &c = *code;
    std::string type_name = struct_def.name;
    c += "func (t *" + type_name +
         ") MarshalSSZ() ([]byte, error) {\n";
    c += "\treturn t.MarshalSSZTo(make([]byte, 0, t.SizeSSZ()))\n";
    c += "}\n";
  }

  void GenMarshalSSZTo(const StructDef &struct_def,
                       const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    std::string type_name = struct_def.name;

    c += "func (t *" + type_name +
         ") MarshalSSZTo(buf []byte) (dst []byte, err error) {\n";
    c += "\tdst = buf\n";

    if (container.dynamic_fields.empty()) {
      // All-fixed container: just marshal fields sequentially
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        GenMarshalField(it->second, "t." + namer_.Field(*field), *field, &c,
                        "\t");
      }
    } else {
      // Container with dynamic fields
      c += "\tdstlen := len(dst)\n";
      c += "\n";

      // Static section: fixed fields inline, offsets for dynamic fields
      int offset_idx = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "t." + namer_.Field(*field);

        if (info.is_dynamic) {
          c += "\t// Offset for '" + field->name + "'\n";
          c += "\toffset" + NumToString(offset_idx) + " := len(dst)\n";
          c += "\tdst = append(dst, 0, 0, 0, 0)\n";
          offset_idx++;
        } else {
          c += "\t// Field '" + field->name + "'\n";
          GenMarshalField(info, fname, *field, &c, "\t");
        }
      }

      c += "\n";

      // Dynamic section: backpatch offsets and marshal dynamic fields
      offset_idx = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "t." + namer_.Field(*field);

        if (info.is_dynamic) {
          c += "\t// Dynamic field '" + field->name + "'\n";
          c += "\tbinary.LittleEndian.PutUint32(dst[offset" +
               NumToString(offset_idx) +
               ":], uint32(len(dst)-dstlen))\n";
          GenMarshalField(info, fname, *field, &c, "\t");
          offset_idx++;
        }
      }
    }

    c += "\treturn dst, err\n";
    c += "}\n";
  }

  void GenMarshalField(const SszFieldInfo &info, const std::string &var,
                       const FieldDef & /*field*/, std::string *code,
                       const std::string &indent) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::Bool:
        c += indent + "if " + var + " {\n";
        c += indent + "\tdst = append(dst, 1)\n";
        c += indent + "} else {\n";
        c += indent + "\tdst = append(dst, 0)\n";
        c += indent + "}\n";
        break;

      case SszType::Uint8:
        c += indent + "dst = append(dst, " + var + ")\n";
        break;

      case SszType::Uint16:
        c += indent + "dst = binary.LittleEndian.AppendUint16(dst, " + var +
             ")\n";
        break;

      case SszType::Uint32:
        c += indent + "dst = binary.LittleEndian.AppendUint32(dst, " + var +
             ")\n";
        break;

      case SszType::Uint64:
        c += indent + "dst = binary.LittleEndian.AppendUint64(dst, " + var +
             ")\n";
        break;

      case SszType::Uint128:
      case SszType::Uint256:
        c += indent + "dst = append(dst, " + var + "[:]...)\n";
        break;

      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "dst = append(dst, " + var + "[:]...)\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "for i := range " + var + " {\n";
          if (info.elem_info->is_dynamic) {
            c += indent + "\tif dst, err = " + var +
                 "[i].MarshalSSZTo(dst); err != nil {\n";
            c += indent + "\t\treturn nil, err\n";
            c += indent + "\t}\n";
          } else {
            c += indent + "\tif dst, err = " + var +
                 "[i].MarshalSSZTo(dst); err != nil {\n";
            c += indent + "\t\treturn nil, err\n";
            c += indent + "\t}\n";
          }
          c += indent + "}\n";
        } else if (info.elem_info) {
          GenMarshalPrimitiveArray(info, var, code, indent);
        }
        break;
      }

      case SszType::ProgressiveList:
      case SszType::List: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          // List of byte lists ([][]byte): offset-based dynamic encoding
          c += indent + "{\n";
          c += indent + "\tlistStart := len(dst)\n";
          c += indent + "\tfor range " + var + " {\n";
          c += indent + "\t\tdst = append(dst, 0, 0, 0, 0)\n";
          c += indent + "\t}\n";
          c += indent + "\tfor i, item := range " + var + " {\n";
          c += indent +
               "\t\tbinary.LittleEndian.PutUint32(dst[listStart+i*4:], "
               "uint32(len(dst)-listStart))\n";
          c += indent + "\t\tdst = append(dst, item...)\n";
          c += indent + "\t}\n";
          c += indent + "}\n";
        } else if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "dst = append(dst, " + var + "...)\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          if (info.elem_info->is_dynamic) {
            // Dynamic element list: write offsets then data
            c += indent + "{\n";
            c += indent + "\tlistStart := len(dst)\n";
            c += indent + "\tfor range " + var + " {\n";
            c += indent + "\t\tdst = append(dst, 0, 0, 0, 0)\n";
            c += indent + "\t}\n";
            c += indent + "\tfor i, item := range " + var + " {\n";
            c += indent +
                 "\t\tbinary.LittleEndian.PutUint32(dst[listStart+i*4:], "
                 "uint32(len(dst)-listStart))\n";
            c += indent +
                 "\t\tif dst, err = item.MarshalSSZTo(dst); err != nil {\n";
            c += indent + "\t\t\treturn nil, err\n";
            c += indent + "\t\t}\n";
            c += indent + "\t}\n";
            c += indent + "}\n";
          } else {
            c += indent + "for _, item := range " + var + " {\n";
            c += indent +
                 "\tif dst, err = item.MarshalSSZTo(dst); err != nil {\n";
            c += indent + "\t\treturn nil, err\n";
            c += indent + "\t}\n";
            c += indent + "}\n";
          }
        } else if (info.elem_info) {
          GenMarshalPrimitiveSlice(info, var, code, indent);
        } else {
          c += indent + "dst = append(dst, " + var + "...)\n";
        }
        break;
      }

      case SszType::Bitlist:
        c += indent + "dst = append(dst, " + var + "...)\n";
        break;

      case SszType::Bitvector:
        c += indent + "dst = append(dst, " + var + "[:]...)\n";
        break;

      case SszType::Container:
        c += indent + "if dst, err = " + var +
             ".MarshalSSZTo(dst); err != nil {\n";
        c += indent + "\treturn nil, err\n";
        c += indent + "}\n";
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
    c += indent + "for i := range " + var + " {\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "\tif " + var + "[i] {\n";
        c += indent + "\t\tdst = append(dst, 1)\n";
        c += indent + "\t} else {\n";
        c += indent + "\t\tdst = append(dst, 0)\n";
        c += indent + "\t}\n";
        break;
      case SszType::Uint16:
        c += indent + "\tdst = binary.LittleEndian.AppendUint16(dst, " + var +
             "[i])\n";
        break;
      case SszType::Uint32:
        c += indent + "\tdst = binary.LittleEndian.AppendUint32(dst, " + var +
             "[i])\n";
        break;
      case SszType::Uint64:
        c += indent + "\tdst = binary.LittleEndian.AppendUint64(dst, " + var +
             "[i])\n";
        break;
      default:
        break;
    }
    c += indent + "}\n";
  }

  void GenMarshalPrimitiveSlice(const SszFieldInfo &info,
                                const std::string &var, std::string *code,
                                const std::string &indent) {
    std::string &c = *code;
    c += indent + "for _, v := range " + var + " {\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "\tif v {\n";
        c += indent + "\t\tdst = append(dst, 1)\n";
        c += indent + "\t} else {\n";
        c += indent + "\t\tdst = append(dst, 0)\n";
        c += indent + "\t}\n";
        break;
      case SszType::Uint16:
        c += indent + "\tdst = binary.LittleEndian.AppendUint16(dst, v)\n";
        break;
      case SszType::Uint32:
        c += indent + "\tdst = binary.LittleEndian.AppendUint32(dst, v)\n";
        break;
      case SszType::Uint64:
        c += indent + "\tdst = binary.LittleEndian.AppendUint64(dst, v)\n";
        break;
      default:
        break;
    }
    c += indent + "}\n";
  }

  // -----------------------------------------------------------------------
  // UnmarshalSSZ generation
  // -----------------------------------------------------------------------

  void GenUnmarshalSSZ(const StructDef &struct_def,
                       const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    std::string type_name = struct_def.name;

    c += "func (t *" + type_name + ") UnmarshalSSZ(buf []byte) error {\n";

    // Validate minimum size
    c += "\tif len(buf) < " + NumToString(container.static_size) + " {\n";
    c += "\t\treturn ssz.ErrBufferTooSmall\n";
    c += "\t}\n\n";

    if (container.dynamic_fields.empty()) {
      // All-fixed container
      uint32_t offset = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "t." + namer_.Field(*field);
        GenUnmarshalField(info, fname, *field, offset, &c, "\t");
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
        std::string fname = "t." + namer_.Field(*field);

        if (info.is_dynamic) {
          c += "\toffset" + NumToString(dyn_idx) +
               " := int(binary.LittleEndian.Uint32(buf[" +
               NumToString(offset) + ":" + NumToString(offset + 4) + "]))\n";
          dyn_offset_names.push_back({field, dyn_idx});

          // Validate first offset equals static size
          if (dyn_idx == 0) {
            c += "\tif offset0 != " + NumToString(container.static_size) +
                 " {\n";
            c += "\t\treturn ssz.ErrInvalidOffset\n";
            c += "\t}\n";
          } else {
            c += "\tif offset" + NumToString(dyn_idx) + " < offset" +
                 NumToString(dyn_idx - 1) + " || offset" +
                 NumToString(dyn_idx) + " > len(buf) {\n";
            c += "\t\treturn ssz.ErrInvalidOffset\n";
            c += "\t}\n";
          }
          offset += 4;
          dyn_idx++;
        } else {
          GenUnmarshalField(info, fname, *field, offset, &c, "\t");
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
        std::string fname = "t." + namer_.Field(*field);

        std::string start_off = "offset" + NumToString(oidx);
        std::string end_off;
        if (i + 1 < dyn_offset_names.size()) {
          end_off = "offset" + NumToString(dyn_offset_names[i + 1].second);
        } else {
          end_off = "len(buf)";
        }

        c += "\t{\n";
        c += "\t\tbuf := buf[" + start_off + ":" + end_off + "]\n";
        GenUnmarshalDynField(info, fname, *field, &c, "\t\t");
        c += "\t}\n";
      }
    }

    c += "\treturn nil\n";
    c += "}\n";
  }

  void GenUnmarshalField(const SszFieldInfo &info, const std::string &var,
                         const FieldDef & /*field*/, uint32_t offset,
                         std::string *code, const std::string &indent) {
    std::string &c = *code;
    std::string off_str = NumToString(offset);

    switch (info.ssz_type) {
      case SszType::Bool:
        c += indent + "if buf[" + off_str + "] > 1 {\n";
        c += indent + "\treturn ssz.ErrInvalidBool\n";
        c += indent + "}\n";
        c += indent + var + " = buf[" + off_str + "] == 1\n";
        break;

      case SszType::Uint8:
        c += indent + var + " = buf[" + off_str + "]\n";
        break;

      case SszType::Uint16:
        c += indent + var + " = binary.LittleEndian.Uint16(buf[" + off_str +
             ":" + NumToString(offset + 2) + "])\n";
        break;

      case SszType::Uint32:
        c += indent + var + " = binary.LittleEndian.Uint32(buf[" + off_str +
             ":" + NumToString(offset + 4) + "])\n";
        break;

      case SszType::Uint64:
        c += indent + var + " = binary.LittleEndian.Uint64(buf[" + off_str +
             ":" + NumToString(offset + 8) + "])\n";
        break;

      case SszType::Uint128:
      case SszType::Uint256:
        c += indent + "copy(" + var + "[:], buf[" + off_str + ":" +
             NumToString(offset + info.fixed_size) + "])\n";
        break;

      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "copy(" + var + "[:], buf[" + off_str + ":" +
               NumToString(offset + info.fixed_size) + "])\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          uint32_t elem_size = info.elem_info->fixed_size;
          c += indent + "for i := range " + var + " {\n";
          c += indent + "\tstart := " + off_str + " + i*" +
               NumToString(elem_size) + "\n";
          c += indent + "\tif err := " + var +
               "[i].UnmarshalSSZ(buf[start:start+" + NumToString(elem_size) +
               "]); err != nil {\n";
          c += indent + "\t\treturn err\n";
          c += indent + "\t}\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          GenUnmarshalPrimitiveArray(info, var, offset, code, indent);
        }
        break;
      }

      case SszType::Bitvector:
        c += indent + "copy(" + var + "[:], buf[" + off_str + ":" +
             NumToString(offset + info.fixed_size) + "])\n";
        break;

      case SszType::Container: {
        c += indent + "if err := " + var + ".UnmarshalSSZ(buf[" + off_str +
             ":" + NumToString(offset + info.fixed_size) + "]); err != nil {\n";
        c += indent + "\treturn err\n";
        c += indent + "}\n";
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
    c += indent + "for i := range " + var + " {\n";
    c += indent + "\toff := " + NumToString(offset) + " + i*" +
         NumToString(elem_size) + "\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "\tif buf[off] > 1 {\n";
        c += indent + "\t\treturn ssz.ErrInvalidBool\n";
        c += indent + "\t}\n";
        c += indent + "\t" + var + "[i] = buf[off] == 1\n";
        break;
      case SszType::Uint16:
        c += indent + "\t" + var +
             "[i] = binary.LittleEndian.Uint16(buf[off:off+2])\n";
        break;
      case SszType::Uint32:
        c += indent + "\t" + var +
             "[i] = binary.LittleEndian.Uint32(buf[off:off+4])\n";
        break;
      case SszType::Uint64:
        c += indent + "\t" + var +
             "[i] = binary.LittleEndian.Uint64(buf[off:off+8])\n";
        break;
      default:
        break;
    }
    c += indent + "}\n";
  }

  void GenUnmarshalDynField(const SszFieldInfo &info, const std::string &var,
                            const FieldDef &field, std::string *code,
                            const std::string &indent) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::ProgressiveList:
      case SszType::List: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          // List of byte lists ([][]byte)
          c += indent + "if len(buf) > 0 {\n";
          c += indent +
               "\tfirstOff := "
               "int(binary.LittleEndian.Uint32(buf[0:4]))\n";
          c += indent + "\tif firstOff%4 != 0 {\n";
          c += indent + "\t\treturn ssz.ErrInvalidOffset\n";
          c += indent + "\t}\n";
          c += indent + "\tcount := firstOff / 4\n";
          c += indent + "\t" + var + " = make([][]byte, count)\n";
          c += indent + "\tfor i := 0; i < count; i++ {\n";
          c += indent +
               "\t\tstart := "
               "int(binary.LittleEndian.Uint32(buf[i*4:i*4+4]))\n";
          c += indent + "\t\tvar end int\n";
          c += indent + "\t\tif i+1 < count {\n";
          c += indent +
               "\t\t\tend = "
               "int(binary.LittleEndian.Uint32(buf[(i+1)*4:(i+1)*4+4]))\n";
          c += indent + "\t\t} else {\n";
          c += indent + "\t\t\tend = len(buf)\n";
          c += indent + "\t\t}\n";
          c += indent +
               "\t\tif start > end || end > len(buf) {\n";
          c += indent + "\t\t\treturn ssz.ErrInvalidOffset\n";
          c += indent + "\t\t}\n";
          c += indent + "\t\t" + var +
               "[i] = make([]byte, end-start)\n";
          c += indent + "\t\tcopy(" + var + "[i], buf[start:end])\n";
          c += indent + "\t}\n";
          c += indent + "}\n";
        } else if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + var + " = make([]byte, len(buf))\n";
          c += indent + "copy(" + var + ", buf)\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          if (info.elem_info->is_dynamic) {
            // Dynamic element list
            c += indent + "if len(buf) > 0 {\n";
            c += indent +
                 "\tfirstOff := "
                 "int(binary.LittleEndian.Uint32(buf[0:4]))\n";
            c += indent + "\tif firstOff%4 != 0 {\n";
            c += indent + "\t\treturn ssz.ErrInvalidOffset\n";
            c += indent + "\t}\n";
            c += indent + "\tcount := firstOff / 4\n";
            c += indent + "\t" + var + " = make(" + GoTypeName(info, field) +
                 ", count)\n";
            c += indent + "\tfor i := 0; i < count; i++ {\n";
            c += indent +
                 "\t\tstart := "
                 "int(binary.LittleEndian.Uint32(buf[i*4:i*4+4]))\n";
            c += indent + "\t\tvar end int\n";
            c += indent + "\t\tif i+1 < count {\n";
            c += indent +
                 "\t\t\tend = "
                 "int(binary.LittleEndian.Uint32(buf[(i+1)*4:(i+1)*4+4]))\n";
            c += indent + "\t\t} else {\n";
            c += indent + "\t\t\tend = len(buf)\n";
            c += indent + "\t\t}\n";
            c += indent +
                 "\t\tif start > end || end > len(buf) {\n";
            c += indent + "\t\t\treturn ssz.ErrInvalidOffset\n";
            c += indent + "\t\t}\n";
            c += indent + "\t\t" + var +
                 "[i] = new(" + info.elem_info->struct_def->name + ")\n";
            c += indent + "\t\tif err := " + var +
                 "[i].UnmarshalSSZ(buf[start:end]); err != nil {\n";
            c += indent + "\t\t\treturn err\n";
            c += indent + "\t\t}\n";
            c += indent + "\t}\n";
            c += indent + "}\n";
          } else {
            uint32_t elem_size = info.elem_info->fixed_size;
            c += indent + "count := len(buf) / " + NumToString(elem_size) +
                 "\n";
            c += indent + var + " = make(" + GoTypeName(info, field) +
                 ", count)\n";
            c += indent + "for i := 0; i < count; i++ {\n";
            c += indent + "\t" + var + "[i] = new(" +
                 info.elem_info->struct_def->name + ")\n";
            c += indent + "\toff := i * " + NumToString(elem_size) + "\n";
            c += indent + "\tif err := " + var +
                 "[i].UnmarshalSSZ(buf[off:off+" + NumToString(elem_size) +
                 "]); err != nil {\n";
            c += indent + "\t\treturn err\n";
            c += indent + "\t}\n";
            c += indent + "}\n";
          }
        } else if (info.elem_info) {
          GenUnmarshalPrimitiveSlice(info, var, field, code, indent);
        } else {
          c += indent + var + " = make([]byte, len(buf))\n";
          c += indent + "copy(" + var + ", buf)\n";
        }
        break;
      }
      case SszType::Bitlist:
        c += indent + var + " = make([]byte, len(buf))\n";
        c += indent + "copy(" + var + ", buf)\n";
        break;
      case SszType::Container:
        c += indent + "if err := " + var +
             ".UnmarshalSSZ(buf); err != nil {\n";
        c += indent + "\treturn err\n";
        c += indent + "}\n";
        break;
      default:
        break;
    }
  }

  void GenUnmarshalPrimitiveSlice(const SszFieldInfo &info,
                                  const std::string &var,
                                  const FieldDef &field, std::string *code,
                                  const std::string &indent) {
    std::string &c = *code;
    uint32_t elem_size = info.elem_info->fixed_size;
    c += indent + "count := len(buf) / " + NumToString(elem_size) + "\n";
    c += indent + var + " = make(" + GoTypeName(info, field) + ", count)\n";
    c += indent + "for i := 0; i < count; i++ {\n";
    c += indent + "\toff := i * " + NumToString(elem_size) + "\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "\tif buf[off] > 1 {\n";
        c += indent + "\t\treturn ssz.ErrInvalidBool\n";
        c += indent + "\t}\n";
        c += indent + "\t" + var + "[i] = buf[off] == 1\n";
        break;
      case SszType::Uint16:
        c += indent + "\t" + var +
             "[i] = binary.LittleEndian.Uint16(buf[off:off+2])\n";
        break;
      case SszType::Uint32:
        c += indent + "\t" + var +
             "[i] = binary.LittleEndian.Uint32(buf[off:off+4])\n";
        break;
      case SszType::Uint64:
        c += indent + "\t" + var +
             "[i] = binary.LittleEndian.Uint64(buf[off:off+8])\n";
        break;
      default:
        break;
    }
    c += indent + "}\n";
  }

  // -----------------------------------------------------------------------
  // HashTreeRoot generation
  // -----------------------------------------------------------------------

  void GenHashTreeRoot(const StructDef &struct_def, std::string *code) {
    std::string &c = *code;
    std::string type_name = struct_def.name;
    c += "func (t *" + type_name +
         ") HashTreeRoot() (root [32]byte, err error) {\n";
    c += "\terr = hasher.WithDefaultHasher(func(hh sszutils.HashWalker) "
         "(err error) {\n";
    c += "\t\tif err = t.HashTreeRootWith(hh); err != nil {\n";
    c += "\t\t\treturn\n";
    c += "\t\t}\n";
    c += "\t\troot, err = hh.HashRoot()\n";
    c += "\t\treturn\n";
    c += "\t})\n";
    c += "\treturn\n";
    c += "}\n";
  }

  void GenHashTreeRootWith(const StructDef &struct_def,
                           const SszContainerInfo &container,
                           std::string *code) {
    std::string &c = *code;
    std::string type_name = struct_def.name;

    c += "func (t *" + type_name +
         ") HashTreeRootWith(hh sszutils.HashWalker) error {\n";
    c += "\tidx := hh.Index()\n";
    c += "\n";

    if (container.is_progressive) {
      // EIP-7495: emit leaves for indices 0..max_ssz_index,
      // filling gaps with zero-hash for inactive fields.
      // Build a map from ssz_index → field for quick lookup.
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
          c += "\t// Index " + NumToString(i) + ": " + field->name + "\n";
          GenHashField(iit->second, "t." + namer_.Field(*field), *field, &c,
                       "\t");
        } else {
          c += "\t// Index " + NumToString(i) + ": gap (inactive)\n";
          c += "\thh.PutUint8(0)\n";
        }
      }
      c += "\n";

      // Emit active_fields bitvector literal
      c += "\thh.MerkleizeProgressiveWithActiveFields(idx, []byte{";
      for (size_t i = 0; i < container.active_fields_bitvector.size(); i++) {
        if (i > 0) c += ", ";
        char hex[8];
        snprintf(hex, sizeof(hex), "0x%02x",
                 container.active_fields_bitvector[i]);
        c += hex;
      }
      c += "})\n";
    } else {
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "t." + namer_.Field(*field);

        c += "\t// Field '" + field->name + "'\n";
        GenHashField(info, fname, *field, &c, "\t");
        c += "\n";
      }

      c += "\thh.Merkleize(idx)\n";
    }

    c += "\treturn nil\n";
    c += "}\n";
  }

  void GenHashField(const SszFieldInfo &info, const std::string &var,
                    const FieldDef & /*field*/, std::string *code,
                    const std::string &indent) {
    std::string &c = *code;

    switch (info.ssz_type) {
      case SszType::Bool:
        c += indent + "hh.PutBool(" + var + ")\n";
        break;

      case SszType::Uint8:
        c += indent + "hh.PutUint8(" + var + ")\n";
        break;

      case SszType::Uint16:
        c += indent + "hh.PutUint16(" + var + ")\n";
        break;

      case SszType::Uint32:
        c += indent + "hh.PutUint32(" + var + ")\n";
        break;

      case SszType::Uint64:
        c += indent + "hh.PutUint64(" + var + ")\n";
        break;

      case SszType::Uint128:
      case SszType::Uint256:
        c += indent + "hh.PutBytes(" + var + "[:])\n";
        break;

      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "hh.PutBytes(" + var + "[:])\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "{\n";
          c += indent + "\tsubIdx := hh.Index()\n";
          c += indent + "\tfor i := range " + var + " {\n";
          c += indent + "\t\tif err := " + var +
               "[i].HashTreeRootWith(hh); err != nil {\n";
          c += indent + "\t\t\treturn err\n";
          c += indent + "\t\t}\n";
          c += indent + "\t}\n";
          c += indent + "\thh.Merkleize(subIdx)\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          c += indent + "{\n";
          c += indent + "\tsubIdx := hh.Index()\n";
          GenHashPrimitiveArray(info, var, code, indent + "\t");
          c += indent + "\thh.FillUpTo32()\n";
          c += indent + "\thh.Merkleize(subIdx)\n";
          c += indent + "}\n";
        }
        break;
      }

      case SszType::List: {
        uint64_t limit = info.limit;
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          // List of byte lists ([][]byte): hash each inner list, then merkleize
          uint64_t inner_limit = info.elem_info->limit;
          uint64_t inner_chunk_limit =
              inner_limit > 0 ? ssz_limit_for_bytes(inner_limit) : 0;
          c += indent + "{\n";
          c += indent + "\tsubIdx := hh.Index()\n";
          c += indent + "\tfor _, item := range " + var + " {\n";
          c += indent + "\t\tinner := hh.Index()\n";
          c += indent + "\t\thh.AppendBytes32(item)\n";
          if (inner_chunk_limit > 0) {
            c += indent + "\t\thh.MerkleizeWithMixin(inner, uint64(len(item)), " +
                 NumToString(inner_chunk_limit) + ")\n";
          } else {
            c += indent + "\t\thh.Merkleize(inner)\n";
          }
          c += indent + "\t}\n";
          c += indent + "\thh.MerkleizeWithMixin(subIdx, uint64(len(" + var +
               ")), " + NumToString(limit) + ")\n";
          c += indent + "}\n";
        } else if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "{\n";
          c += indent + "\tsubIdx := hh.Index()\n";
          c += indent + "\thh.AppendBytes32(" + var + ")\n";
          c += indent + "\thh.MerkleizeWithMixin(subIdx, uint64(len(" + var +
               ")), " + NumToString(ssz_limit_for_bytes(limit)) + ")\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "{\n";
          c += indent + "\tsubIdx := hh.Index()\n";
          c += indent + "\tfor _, item := range " + var + " {\n";
          c += indent + "\t\tif err := item.HashTreeRootWith(hh); err != nil "
                        "{\n";
          c += indent + "\t\t\treturn err\n";
          c += indent + "\t\t}\n";
          c += indent + "\t}\n";
          c += indent + "\thh.MerkleizeWithMixin(subIdx, uint64(len(" + var +
               ")), " + NumToString(limit) + ")\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          c += indent + "{\n";
          c += indent + "\tsubIdx := hh.Index()\n";
          GenHashPrimitiveSlice(info, var, code, indent + "\t");
          c += indent + "\thh.FillUpTo32()\n";
          uint64_t hash_limit =
              ssz_limit_for_elements(limit, info.elem_info->fixed_size);
          c += indent + "\thh.MerkleizeWithMixin(subIdx, uint64(len(" + var +
               ")), " + NumToString(hash_limit) + ")\n";
          c += indent + "}\n";
        }
        break;
      }

      case SszType::Bitlist:
        c += indent + "hh.PutBitlist(" + var + ", " +
             NumToString(info.limit) + ")\n";
        break;

      case SszType::Bitvector:
        c += indent + "hh.PutBytes(" + var + "[:])\n";
        break;

      case SszType::Container:
        c += indent + "if err := " + var +
             ".HashTreeRootWith(hh); err != nil {\n";
        c += indent + "\treturn err\n";
        c += indent + "}\n";
        break;

      case SszType::ProgressiveContainer:
        c += indent + "if err := " + var +
             ".HashTreeRootWith(hh); err != nil {\n";
        c += indent + "\treturn err\n";
        c += indent + "}\n";
        break;

      case SszType::ProgressiveList: {
        // EIP-7916: use MerkleizeProgressiveWithMixin instead of MerkleizeWithMixin
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "{\n";
          c += indent + "\tsubIdx := hh.Index()\n";
          c += indent + "\thh.AppendBytes32(" + var + ")\n";
          c += indent + "\thh.MerkleizeProgressiveWithMixin(subIdx, uint64(len(" +
               var + ")))\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "{\n";
          c += indent + "\tsubIdx := hh.Index()\n";
          c += indent + "\tfor _, item := range " + var + " {\n";
          c += indent + "\t\tif err := item.HashTreeRootWith(hh); err != nil {\n";
          c += indent + "\t\t\treturn err\n";
          c += indent + "\t\t}\n";
          c += indent + "\t}\n";
          c += indent + "\thh.MerkleizeProgressiveWithMixin(subIdx, uint64(len(" +
               var + ")))\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          c += indent + "{\n";
          c += indent + "\tsubIdx := hh.Index()\n";
          GenHashPrimitiveSlice(info, var, code, indent + "\t");
          c += indent + "\thh.FillUpTo32()\n";
          c += indent + "\thh.MerkleizeProgressiveWithMixin(subIdx, uint64(len(" +
               var + ")))\n";
          c += indent + "}\n";
        }
        break;
      }

      case SszType::Union:
        break;
    }
  }

  void GenHashPrimitiveArray(const SszFieldInfo &info, const std::string &var,
                             std::string *code, const std::string &indent) {
    std::string &c = *code;
    c += indent + "for i := range " + var + " {\n";
    std::string elem_var = var + "[i]";
    GenHashPrimitive(info.elem_info->ssz_type, elem_var, code, indent + "\t",
                     true);
    c += indent + "}\n";
  }

  void GenHashPrimitiveSlice(const SszFieldInfo &info, const std::string &var,
                             std::string *code, const std::string &indent) {
    std::string &c = *code;
    c += indent + "for _, v := range " + var + " {\n";
    GenHashPrimitive(info.elem_info->ssz_type, "v", code, indent + "\t", true);
    c += indent + "}\n";
  }

  void GenHashPrimitive(SszType ssz_type, const std::string &var,
                        std::string *code, const std::string &indent,
                        bool append) {
    std::string &c = *code;
    std::string method = append ? "Append" : "Put";
    switch (ssz_type) {
      case SszType::Bool:
        c += indent + "hh." + method + "Bool(" + var + ")\n";
        break;
      case SszType::Uint8:
        c += indent + "hh." + method + "Uint8(" + var + ")\n";
        break;
      case SszType::Uint16:
        c += indent + "hh." + method + "Uint16(" + var + ")\n";
        break;
      case SszType::Uint32:
        c += indent + "hh." + method + "Uint32(" + var + ")\n";
        break;
      case SszType::Uint64:
        c += indent + "hh." + method + "Uint64(" + var + ")\n";
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

  bool SaveType(const StructDef &def, const std::string &classcode) {
    if (classcode.empty()) return true;

    Namespace &ns = *def.defined_namespace;
    std::string pkg_name =
        ns.components.empty() ? ToSnakeCase(def.name) : LastNamespacePart(ns);

    std::string code;
    code += "// Code generated by the FlatBuffers compiler. DO NOT EDIT.\n\n";
    code += "package " + pkg_name + "\n\n";
    code += "import (\n";
    code += "\t\"encoding/binary\"\n";
    code += "\n";
    code += "\tssz \"github.com/google/flatbuffers/go/ssz\"\n";
    code += "\t\"github.com/pk910/dynamic-ssz/hasher\"\n";
    code += "\t\"github.com/pk910/dynamic-ssz/sszutils\"\n";
    code += ")\n\n";

    // Suppress unused import warnings
    code += "var _ = binary.LittleEndian\n";
    code += "var _ = hasher.FastHasherPool\n\n";

    code += classcode;

    // Strip trailing double newlines
    while (code.length() > 2 && code.substr(code.length() - 2) == "\n\n") {
      code.pop_back();
    }

    std::string directory = namer_.Directories(ns);
    std::string snake_name = ToSnakeCase(def.name);
    EnsureDirExists(directory);
    std::string filename = directory + snake_name + "_ssz.go";
    return parser_.opts.file_saver->SaveFile(filename.c_str(), code, false);
  }
};

}  // namespace ssz_go

// ---------------------------------------------------------------------------
// Code generator wrapper
// ---------------------------------------------------------------------------

static bool GenerateSszGo(const Parser &parser, const std::string &path,
                          const std::string &file_name) {
  ssz_go::SszGoGenerator generator(parser, path, file_name);
  return generator.generate();
}

namespace {

class SszGoCodeGenerator : public CodeGenerator {
 public:
  Status GenerateCode(const Parser &parser, const std::string &path,
                      const std::string &filename) override {
    if (!GenerateSszGo(parser, path, filename)) { return Status::ERROR; }
    return Status::OK;
  }

  Status GenerateCode(const uint8_t *, int64_t,
                      const CodeGenOptions &) override {
    return Status::NOT_IMPLEMENTED;
  }

  Status GenerateMakeRule(const Parser &parser, const std::string &path,
                          const std::string &filename,
                          std::string &output) override {
    (void)parser;
    (void)path;
    (void)filename;
    (void)output;
    return Status::NOT_IMPLEMENTED;
  }

  Status GenerateGrpcCode(const Parser &parser, const std::string &path,
                          const std::string &filename) override {
    (void)parser;
    (void)path;
    (void)filename;
    return Status::NOT_IMPLEMENTED;
  }

  Status GenerateRootFile(const Parser &parser,
                          const std::string &path) override {
    (void)parser;
    (void)path;
    return Status::NOT_IMPLEMENTED;
  }

  bool IsSchemaOnly() const override { return true; }

  bool SupportsBfbsGeneration() const override { return false; }

  bool SupportsRootFileGeneration() const override { return false; }

  IDLOptions::Language Language() const override { return IDLOptions::kSszGo; }

  std::string LanguageName() const override { return "SszGo"; }
};

}  // namespace

std::unique_ptr<CodeGenerator> NewSszGoCodeGenerator() {
  return std::unique_ptr<SszGoCodeGenerator>(new SszGoCodeGenerator());
}

}  // namespace flatbuffers
