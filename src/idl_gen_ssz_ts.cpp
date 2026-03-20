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

#include "idl_gen_ssz_ts.h"

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

namespace ssz_ts {

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

static std::set<std::string> TsKeywords() {
  return {
      "break",    "case",      "catch",    "class",    "const",
      "continue", "debugger",  "default",  "delete",   "do",
      "else",     "enum",      "export",   "extends",  "false",
      "finally",  "for",       "function", "if",       "import",
      "in",       "instanceof","let",      "new",      "null",
      "return",   "super",     "switch",   "this",     "throw",
      "true",     "try",       "typeof",   "var",      "void",
      "while",    "with",      "yield",    "async",    "await",
      "of",       "type",      "interface","implements","package",
      "private",  "protected", "public",   "static",
  };
}

static Namer::Config SszTsDefaultConfig() {
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
          /*filename_extension=*/".ts"};
}

// Convert a name to camelCase (handles snake_case and CamelCase input)
static std::string ToCamelCase(const std::string &input) {
  std::string result;
  bool next_upper = false;
  for (size_t i = 0; i < input.size(); i++) {
    char c = input[i];
    if (c == '_') {
      next_upper = true;
      continue;
    }
    if (result.empty()) {
      result += static_cast<char>(tolower(c));
    } else if (next_upper) {
      result += static_cast<char>(toupper(c));
      next_upper = false;
    } else {
      result += c;
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// Main Generator
// ---------------------------------------------------------------------------

class SszTsGenerator : public BaseGenerator {
 public:
  SszTsGenerator(const Parser &parser, const std::string &path,
                 const std::string &file_name)
      : BaseGenerator(parser, path, file_name, "", "", "ts"),
        namer_(WithFlagOptions(SszTsDefaultConfig(), parser.opts, path),
               TsKeywords()) {}

  bool generate() override {
    // Collect all types from the schema into one file
    std::vector<std::pair<const StructDef *, SszContainerInfo>> types;

    for (auto it = parser_.structs_.vec.begin();
         it != parser_.structs_.vec.end(); ++it) {
      auto &struct_def = **it;
      if (struct_def.generated) continue;

      SszContainerInfo container;
      if (!AnalyzeContainer(struct_def, container)) return false;

      types.push_back({&struct_def, std::move(container)});
    }

    if (types.empty()) return true;

    std::string code;

    // Generate all types into one file
    for (size_t i = 0; i < types.size(); i++) {
      auto &struct_def = *types[i].first;
      auto &container = types[i].second;

      std::string type_code;
      if (!GenerateType(struct_def, container, &type_code)) return false;
      code += type_code;
      if (i + 1 < types.size()) code += "\n";
    }

    return SaveFile(*types[0].first, code);
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

    // Check for progressive list (EIP-7916)
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

    info.elem_info.reset(new SszFieldInfo());
    if (!ResolveElemType(field, elem_type, *info.elem_info)) return false;

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
  // TypeScript type helpers
  // -----------------------------------------------------------------------

  static std::string TsFieldName(const FieldDef &field) {
    return ToCamelCase(field.name);
  }

  std::string TsTypeName(const SszFieldInfo &info, const FieldDef &field) {
    switch (info.ssz_type) {
      case SszType::Bool:
        return "boolean";
      case SszType::Uint8:
        return "number";
      case SszType::Uint16:
        return "number";
      case SszType::Uint32:
        return "number";
      case SszType::Uint64:
        return "bigint";
      case SszType::Uint128:
        return "Uint8Array /* 16 bytes */";
      case SszType::Uint256:
        return "Uint8Array /* 32 bytes */";
      case SszType::Container:
        if (info.struct_def) { return info.struct_def->name; }
        return "unknown";
      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          return "Uint8Array /* " + NumToString(info.limit) + " bytes */";
        }
        if (info.elem_info) {
          return "Array<" + TsElemTypeName(*info.elem_info) + ">";
        }
        return "unknown";
      }
      case SszType::ProgressiveList:
      case SszType::List: {
        if (field.value.type.base_type == BASE_TYPE_STRING) {
          return "Uint8Array";
        }
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          return "Uint8Array";
        }
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          return "Array<Uint8Array>";
        }
        if (info.elem_info) {
          return "Array<" + TsElemTypeName(*info.elem_info) + ">";
        }
        return "Uint8Array";
      }
      case SszType::Bitlist:
        return "Uint8Array";
      case SszType::Bitvector:
        return "Uint8Array /* " + NumToString((info.bitsize + 7) / 8) +
               " bytes */";
      case SszType::ProgressiveContainer:
        if (info.struct_def) { return info.struct_def->name; }
        return "unknown";
      case SszType::Union:
        return "unknown";
    }
    return "unknown";
  }

  static std::string TsElemTypeName(const SszFieldInfo &info) {
    switch (info.ssz_type) {
      case SszType::Bool:
        return "boolean";
      case SszType::Uint8:
        return "number";
      case SszType::Uint16:
        return "number";
      case SszType::Uint32:
        return "number";
      case SszType::Uint64:
        return "bigint";
      case SszType::Container:
        if (info.struct_def) {
          return info.struct_def->name;
        }
        return "unknown";
      case SszType::List:
        return "Uint8Array";
      case SszType::Uint128:
      case SszType::Uint256:
        return "Uint8Array";
      case SszType::Vector:
      case SszType::Bitlist:
      case SszType::Bitvector:
      case SszType::Union:
      case SszType::ProgressiveContainer:
      case SszType::ProgressiveList:
        return "unknown";
    }
    return "unknown";
  }

  std::string TsDefaultValue(const SszFieldInfo &info, const FieldDef &field) {
    switch (info.ssz_type) {
      case SszType::Bool:
        return "false";
      case SszType::Uint8:
      case SszType::Uint16:
      case SszType::Uint32:
        return "0";
      case SszType::Uint64:
        return "0n";
      case SszType::Uint128:
        return "new Uint8Array(16)";
      case SszType::Uint256:
        return "new Uint8Array(32)";
      case SszType::Container:
        if (info.struct_def) {
          return "new " + info.struct_def->name + "()";
        }
        return "null as any";
      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          return "new Uint8Array(" + NumToString(info.limit) + ")";
        }
        if (info.elem_info &&
            info.elem_info->ssz_type == SszType::Container &&
            info.elem_info->struct_def) {
          return "Array.from({length: " + NumToString(info.limit) +
                 "}, () => new " + info.elem_info->struct_def->name + "())";
        }
        if (info.elem_info) {
          std::string def_val;
          switch (info.elem_info->ssz_type) {
            case SszType::Bool: def_val = "false"; break;
            case SszType::Uint64: def_val = "0n"; break;
            default: def_val = "0"; break;
          }
          return "new Array<" + TsElemTypeName(*info.elem_info) + ">(" +
                 NumToString(info.limit) + ").fill(" + def_val + ")";
        }
        return "new Uint8Array(0)";
      }
      case SszType::ProgressiveList:
      case SszType::List: {
        if (field.value.type.base_type == BASE_TYPE_STRING) {
          return "new Uint8Array(0)";
        }
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          return "new Uint8Array(0)";
        }
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          return "[]";
        }
        if (info.elem_info) {
          return "[]";
        }
        return "new Uint8Array(0)";
      }
      case SszType::Bitlist:
        return "new Uint8Array(0)";
      case SszType::Bitvector:
        return "new Uint8Array(" + NumToString((info.bitsize + 7) / 8) + ")";
      case SszType::ProgressiveContainer:
        if (info.struct_def) {
          return "new " + info.struct_def->name + "()";
        }
        return "null as any";
      case SszType::Union:
        return "null as any";
    }
    return "null as any";
  }

  // -----------------------------------------------------------------------
  // Code generation entry point
  // -----------------------------------------------------------------------

  bool GenerateType(const StructDef &struct_def,
                    const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;

    // Generate class definition with fields
    GenTsClass(struct_def, container, &c);
    c += "\n";

    // Generate sszBytesLen
    GenSszBytesLen(container, &c);
    c += "\n";

    // Generate sszAppend
    GenSszAppend(container, &c);
    c += "\n";

    // Generate toSszBytes
    GenToSszBytes(&c);
    c += "\n";

    // Generate fromSszBytes
    GenFromSszBytes(struct_def, container, &c);
    c += "\n";

    // Generate treeHashRoot
    GenTreeHashRoot(&c);
    c += "\n";

    // Generate treeHashWith
    GenTreeHashWith(container, &c);

    // Close class
    c += "}\n";

    return true;
  }

  // -----------------------------------------------------------------------
  // TypeScript class generation
  // -----------------------------------------------------------------------

  void GenTsClass(const StructDef &struct_def,
                  const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    c += "export class " + struct_def.name + " {\n";
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      auto &info = it->second;
      std::string fname = TsFieldName(*field);
      std::string tname = TsTypeName(info, *field);
      std::string defval = TsDefaultValue(info, *field);
      c += "  public " + fname + ": " + tname + " = " + defval + ";\n";
    }
  }

  // -----------------------------------------------------------------------
  // sszBytesLen generation
  // -----------------------------------------------------------------------

  void GenSszBytesLen(const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;

    c += "  sszBytesLen(): number {\n";

    if (container.is_fixed) {
      c += "    return " + NumToString(container.static_size) + ";\n";
    } else {
      c += "    let size = " + NumToString(container.static_size) + ";\n";
      for (auto *field : container.dynamic_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "this." + TsFieldName(*field);
        GenSizeField(info, fname, &c, "    ");
      }
      c += "    return size;\n";
    }
    c += "  }\n";
  }

  void GenSizeField(const SszFieldInfo &info, const std::string &var,
                    std::string *code, const std::string &indent) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::ProgressiveList:
      case SszType::List: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          c += indent + "size += " + var + ".length * 4;\n";
          c += indent + "for (const item of " + var + ") {\n";
          c += indent + "  size += item.length;\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
            info.elem_info->ssz_type == SszType::Container &&
            info.elem_info->is_dynamic) {
          c += indent + "size += " + var + ".length * 4;\n";
          c += indent + "for (const item of " + var + ") {\n";
          c += indent + "  size += item.sszBytesLen();\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          c += indent + "size += " + var + ".length * " +
               NumToString(info.elem_info->fixed_size) + ";\n";
        } else {
          c += indent + "size += " + var + ".length;\n";
        }
        break;
      }
      case SszType::Bitlist:
        c += indent + "size += " + var + ".length;\n";
        break;
      case SszType::Container:
        c += indent + "size += " + var + ".sszBytesLen();\n";
        break;
      case SszType::Bool:
      case SszType::Uint8:
      case SszType::Uint16:
      case SszType::Uint32:
      case SszType::Uint64:
      case SszType::Uint128:
      case SszType::Uint256:
      case SszType::Vector:
      case SszType::Bitvector:
      case SszType::ProgressiveContainer:
      case SszType::Union:
        break;
    }
  }

  // -----------------------------------------------------------------------
  // sszAppend generation
  // -----------------------------------------------------------------------

  void GenSszAppend(const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;

    c += "  sszAppend(buf: Uint8Array, offset: number): number {\n";
    c += "    const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);\n";

    if (container.dynamic_fields.empty()) {
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        GenMarshalField(it->second, "this." + TsFieldName(*field), &c,
                        "    ");
      }
    } else {
      c += "    const startOffset = offset;\n";
      c += "\n";

      int offset_idx = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "this." + TsFieldName(*field);

        if (info.is_dynamic) {
          c += "    // Offset placeholder for '" + field->name + "'\n";
          c += "    const offsetPos" + NumToString(offset_idx) +
               " = offset;\n";
          c += "    offset += 4;\n";
          offset_idx++;
        } else {
          c += "    // Field '" + field->name + "'\n";
          GenMarshalField(info, fname, &c, "    ");
        }
      }

      c += "\n";

      offset_idx = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "this." + TsFieldName(*field);

        if (info.is_dynamic) {
          c += "    // Dynamic field '" + field->name + "'\n";
          c += "    dv.setUint32(offsetPos" + NumToString(offset_idx) +
               ", offset - startOffset, true);\n";
          GenMarshalField(info, fname, &c, "    ");
          offset_idx++;
        }
      }
    }

    c += "    return offset;\n";
    c += "  }\n";
  }

  void GenMarshalField(const SszFieldInfo &info, const std::string &var,
                       std::string *code, const std::string &indent) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::Bool:
        c += indent + "buf[offset++] = " + var + " ? 1 : 0;\n";
        break;

      case SszType::Uint8:
        c += indent + "buf[offset++] = " + var + ";\n";
        break;

      case SszType::Uint16:
        c += indent + "dv.setUint16(offset, " + var +
             ", true); offset += 2;\n";
        break;

      case SszType::Uint32:
        c += indent + "dv.setUint32(offset, " + var +
             ", true); offset += 4;\n";
        break;

      case SszType::Uint64:
        c += indent + "dv.setBigUint64(offset, " + var +
             ", true); offset += 8;\n";
        break;

      case SszType::Uint128:
        c += indent + "buf.set(" + var +
             ".subarray(0, 16), offset); offset += 16;\n";
        break;

      case SszType::Uint256:
        c += indent + "buf.set(" + var +
             ".subarray(0, 32), offset); offset += 32;\n";
        break;

      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "buf.set(" + var + ", offset); offset += " +
               NumToString(info.fixed_size) + ";\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "for (let i = 0; i < " + var + ".length; i++) {\n";
          c += indent + "  offset = " + var +
               "[i].sszAppend(buf, offset);\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          GenMarshalPrimitiveArray(info, var, code, indent);
        }
        break;
      }

      case SszType::ProgressiveList:
      case SszType::List: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          c += indent + "{\n";
          c += indent + "  const listStart = offset;\n";
          c += indent + "  for (let i = 0; i < " + var +
               ".length; i++) {\n";
          c += indent + "    offset += 4;\n";
          c += indent + "  }\n";
          c += indent + "  for (let i = 0; i < " + var +
               ".length; i++) {\n";
          c += indent +
               "    dv.setUint32(listStart + i * 4, offset - listStart, "
               "true);\n";
          c += indent + "    buf.set(" + var + "[i], offset);\n";
          c += indent + "    offset += " + var + "[i].length;\n";
          c += indent + "  }\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "buf.set(" + var + ", offset); offset += " + var +
               ".length;\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          if (info.elem_info->is_dynamic) {
            c += indent + "{\n";
            c += indent + "  const listStart = offset;\n";
            c += indent + "  for (let i = 0; i < " + var +
                 ".length; i++) {\n";
            c += indent + "    offset += 4;\n";
            c += indent + "  }\n";
            c += indent + "  for (let i = 0; i < " + var +
                 ".length; i++) {\n";
            c += indent +
                 "    dv.setUint32(listStart + i * 4, offset - listStart, "
                 "true);\n";
            c += indent + "    offset = " + var +
                 "[i].sszAppend(buf, offset);\n";
            c += indent + "  }\n";
            c += indent + "}\n";
          } else {
            c += indent + "for (const item of " + var + ") {\n";
            c += indent + "  offset = item.sszAppend(buf, offset);\n";
            c += indent + "}\n";
          }
        } else if (info.elem_info) {
          GenMarshalPrimitiveSlice(info, var, code, indent);
        } else {
          c += indent + "buf.set(" + var + ", offset); offset += " + var +
               ".length;\n";
        }
        break;
      }

      case SszType::Bitlist:
        c += indent + "buf.set(" + var + ", offset); offset += " + var +
             ".length;\n";
        break;

      case SszType::Bitvector:
        c += indent + "buf.set(" + var + ", offset); offset += " +
             NumToString(info.fixed_size) + ";\n";
        break;

      case SszType::Container:
        c += indent + "offset = " + var + ".sszAppend(buf, offset);\n";
        break;

      case SszType::ProgressiveContainer:
        c += indent + "offset = " + var + ".sszAppend(buf, offset);\n";
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
    c += indent + "for (let i = 0; i < " + var + ".length; i++) {\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "  buf[offset++] = " + var + "[i] ? 1 : 0;\n";
        break;
      case SszType::Uint16:
        c += indent + "  dv.setUint16(offset, " + var +
             "[i], true); offset += 2;\n";
        break;
      case SszType::Uint32:
        c += indent + "  dv.setUint32(offset, " + var +
             "[i], true); offset += 4;\n";
        break;
      case SszType::Uint64:
        c += indent + "  dv.setBigUint64(offset, " + var +
             "[i], true); offset += 8;\n";
        break;
      case SszType::Uint8:
      case SszType::Uint128:
      case SszType::Uint256:
      case SszType::Container:
      case SszType::Vector:
      case SszType::List:
      case SszType::Bitlist:
      case SszType::Bitvector:
      case SszType::Union:
      case SszType::ProgressiveContainer:
      case SszType::ProgressiveList:
        break;
    }
    c += indent + "}\n";
  }

  void GenMarshalPrimitiveSlice(const SszFieldInfo &info,
                                const std::string &var, std::string *code,
                                const std::string &indent) {
    std::string &c = *code;
    c += indent + "for (const v of " + var + ") {\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "  buf[offset++] = v ? 1 : 0;\n";
        break;
      case SszType::Uint16:
        c += indent + "  dv.setUint16(offset, v, true); offset += 2;\n";
        break;
      case SszType::Uint32:
        c += indent + "  dv.setUint32(offset, v, true); offset += 4;\n";
        break;
      case SszType::Uint64:
        c += indent + "  dv.setBigUint64(offset, v, true); offset += 8;\n";
        break;
      case SszType::Uint8:
      case SszType::Uint128:
      case SszType::Uint256:
      case SszType::Container:
      case SszType::Vector:
      case SszType::List:
      case SszType::Bitlist:
      case SszType::Bitvector:
      case SszType::Union:
      case SszType::ProgressiveContainer:
      case SszType::ProgressiveList:
        break;
    }
    c += indent + "}\n";
  }

  // -----------------------------------------------------------------------
  // toSszBytes generation
  // -----------------------------------------------------------------------

  void GenToSszBytes(std::string *code) {
    std::string &c = *code;
    c += "  toSszBytes(): Uint8Array {\n";
    c += "    const buf = new Uint8Array(this.sszBytesLen());\n";
    c += "    this.sszAppend(buf, 0);\n";
    c += "    return buf;\n";
    c += "  }\n";
  }

  // -----------------------------------------------------------------------
  // fromSszBytes generation
  // -----------------------------------------------------------------------

  void GenFromSszBytes(const StructDef &struct_def,
                       const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    std::string type_name = struct_def.name;

    c += "  static fromSszBytes(buf: Uint8Array): " + type_name + " {\n";
    c += "    const dv = new DataView(buf.buffer, buf.byteOffset, "
         "buf.byteLength);\n";

    c += "    if (buf.length < " + NumToString(container.static_size) +
         ") {\n";
    c += "      throw new SszError('buffer too small');\n";
    c += "    }\n";

    c += "    const t = new " + type_name + "();\n";

    if (container.dynamic_fields.empty()) {
      uint32_t offset = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "t." + TsFieldName(*field);
        GenUnmarshalField(info, fname, offset, &c, "    ");
        offset += info.fixed_size;
      }
    } else {
      uint32_t offset = 0;
      int dyn_idx = 0;
      std::vector<std::pair<const FieldDef *, int>> dyn_offset_names;

      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "t." + TsFieldName(*field);

        if (info.is_dynamic) {
          c += "    const offset" + NumToString(dyn_idx) +
               " = dv.getUint32(" + NumToString(offset) + ", true);\n";
          dyn_offset_names.push_back({field, dyn_idx});

          if (dyn_idx == 0) {
            c += "    if (offset0 !== " +
                 NumToString(container.static_size) + ") {\n";
            c += "      throw new SszError('invalid offset');\n";
            c += "    }\n";
          } else {
            c += "    if (offset" + NumToString(dyn_idx) + " < offset" +
                 NumToString(dyn_idx - 1) + " || offset" +
                 NumToString(dyn_idx) + " > buf.length) {\n";
            c += "      throw new SszError('invalid offset');\n";
            c += "    }\n";
          }
          offset += 4;
          dyn_idx++;
        } else {
          GenUnmarshalField(info, fname, offset, &c, "    ");
          offset += info.fixed_size;
        }
      }

      c += "\n";

      for (size_t i = 0; i < dyn_offset_names.size(); i++) {
        auto *field = dyn_offset_names[i].first;
        int oidx = dyn_offset_names[i].second;
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "t." + TsFieldName(*field);

        std::string start_off = "offset" + NumToString(oidx);
        std::string end_off;
        if (i + 1 < dyn_offset_names.size()) {
          end_off = "offset" + NumToString(dyn_offset_names[i + 1].second);
        } else {
          end_off = "buf.length";
        }

        c += "    {\n";
        c += "      const dynBuf = buf.subarray(" + start_off + ", " +
             end_off + ");\n";
        GenUnmarshalDynField(info, fname, *field, &c, "      ");
        c += "    }\n";
      }
    }

    c += "    return t;\n";
    c += "  }\n";
  }

  void GenUnmarshalField(const SszFieldInfo &info, const std::string &var,
                         uint32_t offset, std::string *code,
                         const std::string &indent) {
    std::string &c = *code;
    std::string off_str = NumToString(offset);

    switch (info.ssz_type) {
      case SszType::Bool:
        c += indent + "if (buf[" + off_str + "] > 1) {\n";
        c += indent + "  throw new SszError('invalid bool');\n";
        c += indent + "}\n";
        c += indent + var + " = buf[" + off_str + "] === 1;\n";
        break;

      case SszType::Uint8:
        c += indent + var + " = buf[" + off_str + "];\n";
        break;

      case SszType::Uint16:
        c += indent + var + " = dv.getUint16(" + off_str + ", true);\n";
        break;

      case SszType::Uint32:
        c += indent + var + " = dv.getUint32(" + off_str + ", true);\n";
        break;

      case SszType::Uint64:
        c += indent + var + " = dv.getBigUint64(" + off_str + ", true);\n";
        break;

      case SszType::Uint128:
        c += indent + var + " = buf.slice(" + off_str + ", " +
             NumToString(offset + 16) + ");\n";
        break;

      case SszType::Uint256:
        c += indent + var + " = buf.slice(" + off_str + ", " +
             NumToString(offset + 32) + ");\n";
        break;

      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + var + " = buf.slice(" + off_str + ", " +
               NumToString(offset + info.fixed_size) + ");\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          uint32_t elem_size = info.elem_info->fixed_size;
          c += indent + "for (let i = 0; i < " + NumToString(info.limit) +
               "; i++) {\n";
          c += indent + "  const start = " + off_str + " + i * " +
               NumToString(elem_size) + ";\n";
          c += indent + "  " + var + "[i] = " +
               info.elem_info->struct_def->name +
               ".fromSszBytes(buf.subarray(start, start + " +
               NumToString(elem_size) + "));\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          GenUnmarshalPrimitiveArray(info, var, offset, code, indent);
        }
        break;
      }

      case SszType::Bitvector:
        c += indent + var + " = buf.slice(" + off_str + ", " +
             NumToString(offset + info.fixed_size) + ");\n";
        break;

      case SszType::Container: {
        c += indent + var + " = " + info.struct_def->name +
             ".fromSszBytes(buf.subarray(" + off_str + ", " +
             NumToString(offset + info.fixed_size) + "));\n";
        break;
      }

      case SszType::ProgressiveContainer: {
        c += indent + var + " = " + info.struct_def->name +
             ".fromSszBytes(buf.subarray(" + off_str + ", " +
             NumToString(offset + info.fixed_size) + "));\n";
        break;
      }

      case SszType::List:
      case SszType::ProgressiveList:
      case SszType::Bitlist:
      case SszType::Union:
        break;
    }
  }

  void GenUnmarshalPrimitiveArray(const SszFieldInfo &info,
                                  const std::string &var, uint32_t offset,
                                  std::string *code,
                                  const std::string &indent) {
    std::string &c = *code;
    uint32_t elem_size = info.elem_info->fixed_size;
    c += indent + "for (let i = 0; i < " + NumToString(info.limit) +
         "; i++) {\n";
    c += indent + "  const off = " + NumToString(offset) + " + i * " +
         NumToString(elem_size) + ";\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "  if (buf[off] > 1) {\n";
        c += indent + "    throw new SszError('invalid bool');\n";
        c += indent + "  }\n";
        c += indent + "  " + var + "[i] = buf[off] === 1;\n";
        break;
      case SszType::Uint16:
        c += indent + "  " + var + "[i] = dv.getUint16(off, true);\n";
        break;
      case SszType::Uint32:
        c += indent + "  " + var + "[i] = dv.getUint32(off, true);\n";
        break;
      case SszType::Uint64:
        c += indent + "  " + var + "[i] = dv.getBigUint64(off, true);\n";
        break;
      case SszType::Uint8:
      case SszType::Uint128:
      case SszType::Uint256:
      case SszType::Container:
      case SszType::Vector:
      case SszType::List:
      case SszType::Bitlist:
      case SszType::Bitvector:
      case SszType::Union:
      case SszType::ProgressiveContainer:
      case SszType::ProgressiveList:
        break;
    }
    c += indent + "}\n";
  }

  void GenUnmarshalDynField(const SszFieldInfo &info, const std::string &var,
                            const FieldDef & /*field*/, std::string *code,
                            const std::string &indent) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::ProgressiveList:
      case SszType::List: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          c += indent + "if (dynBuf.length > 0) {\n";
          c += indent + "  const dynDv = new DataView(dynBuf.buffer, "
               "dynBuf.byteOffset, dynBuf.byteLength);\n";
          c += indent + "  const firstOff = dynDv.getUint32(0, true);\n";
          c += indent + "  if (firstOff % 4 !== 0) {\n";
          c += indent + "    throw new SszError('invalid offset');\n";
          c += indent + "  }\n";
          c += indent + "  const count = firstOff / 4;\n";
          c += indent + "  " + var +
               " = new Array<Uint8Array>(count);\n";
          c += indent + "  for (let i = 0; i < count; i++) {\n";
          c += indent +
               "    const start = dynDv.getUint32(i * 4, true);\n";
          c += indent +
               "    const end = i + 1 < count ? dynDv.getUint32((i + 1) * "
               "4, true) : dynBuf.length;\n";
          c += indent +
               "    if (start > end || end > dynBuf.length) {\n";
          c += indent + "      throw new SszError('invalid offset');\n";
          c += indent + "    }\n";
          c += indent + "    " + var +
               "[i] = dynBuf.slice(start, end);\n";
          c += indent + "  }\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + var + " = dynBuf.slice();\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          if (info.elem_info->is_dynamic) {
            c += indent + "if (dynBuf.length > 0) {\n";
            c += indent + "  const dynDv = new DataView(dynBuf.buffer, "
                 "dynBuf.byteOffset, dynBuf.byteLength);\n";
            c += indent +
                 "  const firstOff = dynDv.getUint32(0, true);\n";
            c += indent + "  if (firstOff % 4 !== 0) {\n";
            c += indent +
                 "    throw new SszError('invalid offset');\n";
            c += indent + "  }\n";
            c += indent + "  const count = firstOff / 4;\n";
            c += indent + "  " + var + " = new Array<" +
                 info.elem_info->struct_def->name + ">(count);\n";
            c += indent + "  for (let i = 0; i < count; i++) {\n";
            c += indent +
                 "    const start = dynDv.getUint32(i * 4, true);\n";
            c += indent +
                 "    const end = i + 1 < count ? dynDv.getUint32((i + "
                 "1) * 4, true) : dynBuf.length;\n";
            c += indent +
                 "    if (start > end || end > dynBuf.length) {\n";
            c += indent +
                 "      throw new SszError('invalid offset');\n";
            c += indent + "    }\n";
            c += indent + "    " + var + "[i] = " +
                 info.elem_info->struct_def->name +
                 ".fromSszBytes(dynBuf.subarray(start, end));\n";
            c += indent + "  }\n";
            c += indent + "}\n";
          } else {
            uint32_t elem_size = info.elem_info->fixed_size;
            c += indent + "{\n";
            c += indent + "  const count = Math.floor(dynBuf.length / " +
                 NumToString(elem_size) + ");\n";
            c += indent + "  " + var + " = new Array<" +
                 info.elem_info->struct_def->name + ">(count);\n";
            c += indent + "  for (let i = 0; i < count; i++) {\n";
            c += indent + "    const off = i * " +
                 NumToString(elem_size) + ";\n";
            c += indent + "    " + var + "[i] = " +
                 info.elem_info->struct_def->name +
                 ".fromSszBytes(dynBuf.subarray(off, off + " +
                 NumToString(elem_size) + "));\n";
            c += indent + "  }\n";
            c += indent + "}\n";
          }
        } else if (info.elem_info) {
          GenUnmarshalPrimitiveSlice(info, var, code, indent);
        } else {
          c += indent + var + " = dynBuf.slice();\n";
        }
        break;
      }
      case SszType::Bitlist:
        c += indent + var + " = dynBuf.slice();\n";
        break;
      case SszType::Container:
        c += indent + var + " = " + info.struct_def->name +
             ".fromSszBytes(dynBuf);\n";
        break;
      case SszType::ProgressiveContainer:
        c += indent + var + " = " + info.struct_def->name +
             ".fromSszBytes(dynBuf);\n";
        break;
      case SszType::Bool:
      case SszType::Uint8:
      case SszType::Uint16:
      case SszType::Uint32:
      case SszType::Uint64:
      case SszType::Uint128:
      case SszType::Uint256:
      case SszType::Vector:
      case SszType::Bitvector:
      case SszType::Union:
        break;
    }
  }

  void GenUnmarshalPrimitiveSlice(const SszFieldInfo &info,
                                  const std::string &var,
                                  std::string *code,
                                  const std::string &indent) {
    std::string &c = *code;
    uint32_t elem_size = info.elem_info->fixed_size;
    c += indent + "{\n";
    c += indent + "  const dynDv = new DataView(dynBuf.buffer, "
         "dynBuf.byteOffset, dynBuf.byteLength);\n";
    c += indent + "  const count = Math.floor(dynBuf.length / " +
         NumToString(elem_size) + ");\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "  " + var + " = new Array<boolean>(count);\n";
        c += indent + "  for (let i = 0; i < count; i++) {\n";
        c += indent + "    const off = i * " + NumToString(elem_size) +
             ";\n";
        c += indent + "    if (dynBuf[off] > 1) {\n";
        c += indent + "      throw new SszError('invalid bool');\n";
        c += indent + "    }\n";
        c += indent + "    " + var + "[i] = dynBuf[off] === 1;\n";
        c += indent + "  }\n";
        break;
      case SszType::Uint16:
        c += indent + "  " + var + " = new Array<number>(count);\n";
        c += indent + "  for (let i = 0; i < count; i++) {\n";
        c += indent + "    " + var +
             "[i] = dynDv.getUint16(i * 2, true);\n";
        c += indent + "  }\n";
        break;
      case SszType::Uint32:
        c += indent + "  " + var + " = new Array<number>(count);\n";
        c += indent + "  for (let i = 0; i < count; i++) {\n";
        c += indent + "    " + var +
             "[i] = dynDv.getUint32(i * 4, true);\n";
        c += indent + "  }\n";
        break;
      case SszType::Uint64:
        c += indent + "  " + var + " = new Array<bigint>(count);\n";
        c += indent + "  for (let i = 0; i < count; i++) {\n";
        c += indent + "    " + var +
             "[i] = dynDv.getBigUint64(i * 8, true);\n";
        c += indent + "  }\n";
        break;
      case SszType::Uint8:
      case SszType::Uint128:
      case SszType::Uint256:
      case SszType::Container:
      case SszType::Vector:
      case SszType::List:
      case SszType::Bitlist:
      case SszType::Bitvector:
      case SszType::Union:
      case SszType::ProgressiveContainer:
      case SszType::ProgressiveList:
        break;
    }
    c += indent + "}\n";
  }

  // -----------------------------------------------------------------------
  // treeHashRoot generation
  // -----------------------------------------------------------------------

  void GenTreeHashRoot(std::string *code) {
    std::string &c = *code;
    c += "  treeHashRoot(): Uint8Array {\n";
    c += "    const h = HasherPool.get();\n";
    c += "    try {\n";
    c += "      this.treeHashWith(h);\n";
    c += "      return h.hashRoot();\n";
    c += "    } finally {\n";
    c += "      HasherPool.put(h);\n";
    c += "    }\n";
    c += "  }\n";
  }

  void GenTreeHashWith(const SszContainerInfo &container,
                       std::string *code) {
    std::string &c = *code;

    c += "  treeHashWith(h: Hasher): void {\n";
    c += "    const idx = h.index();\n";
    c += "\n";

    if (container.is_progressive) {
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
          c += "    // Index " + NumToString(i) + ": " + field->name +
               "\n";
          GenHashField(iit->second, "this." + TsFieldName(*field), &c,
                       "    ");
        } else {
          c += "    // Index " + NumToString(i) + ": gap (inactive)\n";
          c += "    h.putUint8(0);\n";
        }
      }
      c += "\n";

      c += "    h.merkleizeProgressiveWithActiveFields(idx, new Uint8Array([";
      for (size_t i = 0; i < container.active_fields_bitvector.size(); i++) {
        if (i > 0) c += ", ";
        char hex[8];
        snprintf(hex, sizeof(hex), "0x%02x",
                 container.active_fields_bitvector[i]);
        c += hex;
      }
      c += "]));\n";
    } else {
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "this." + TsFieldName(*field);

        c += "    // Field '" + field->name + "'\n";
        GenHashField(info, fname, &c, "    ");
        c += "\n";
      }

      c += "    h.merkleize(idx);\n";
    }

    c += "  }\n";
  }

  void GenHashField(const SszFieldInfo &info, const std::string &var,
                    std::string *code, const std::string &indent) {
    std::string &c = *code;

    switch (info.ssz_type) {
      case SszType::Bool:
        c += indent + "h.putBool(" + var + ");\n";
        break;

      case SszType::Uint8:
        c += indent + "h.putUint8(" + var + ");\n";
        break;

      case SszType::Uint16:
        c += indent + "h.putUint16(" + var + ");\n";
        break;

      case SszType::Uint32:
        c += indent + "h.putUint32(" + var + ");\n";
        break;

      case SszType::Uint64:
        c += indent + "h.putUint64(" + var + ");\n";
        break;

      case SszType::Uint128:
      case SszType::Uint256:
        c += indent + "h.putBytes(" + var + ");\n";
        break;

      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "h.putBytes(" + var + ");\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "{\n";
          c += indent + "  const subIdx = h.index();\n";
          c += indent + "  for (const item of " + var + ") {\n";
          c += indent + "    item.treeHashWith(h);\n";
          c += indent + "  }\n";
          c += indent + "  h.merkleize(subIdx);\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          c += indent + "{\n";
          c += indent + "  const subIdx = h.index();\n";
          GenHashPrimitiveArray(info, var, code, indent + "  ");
          c += indent + "  h.fillUpTo32();\n";
          c += indent + "  h.merkleize(subIdx);\n";
          c += indent + "}\n";
        }
        break;
      }

      case SszType::List: {
        uint64_t limit = info.limit;
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          uint64_t inner_limit = info.elem_info->limit;
          uint64_t inner_chunk_limit =
              inner_limit > 0 ? ssz_limit_for_bytes(inner_limit) : 0;
          c += indent + "{\n";
          c += indent + "  const subIdx = h.index();\n";
          c += indent + "  for (const item of " + var + ") {\n";
          c += indent + "    const inner = h.index();\n";
          c += indent + "    h.appendBytes32(item);\n";
          if (inner_chunk_limit > 0) {
            c += indent +
                 "    h.merkleizeWithMixin(inner, item.length, " +
                 NumToString(inner_chunk_limit) + ");\n";
          } else {
            c += indent + "    h.merkleize(inner);\n";
          }
          c += indent + "  }\n";
          c += indent + "  h.merkleizeWithMixin(subIdx, " + var +
               ".length, " + NumToString(limit) + ");\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "{\n";
          c += indent + "  const subIdx = h.index();\n";
          c += indent + "  h.appendBytes32(" + var + ");\n";
          c += indent + "  h.merkleizeWithMixin(subIdx, " + var +
               ".length, " + NumToString(ssz_limit_for_bytes(limit)) +
               ");\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "{\n";
          c += indent + "  const subIdx = h.index();\n";
          c += indent + "  for (const item of " + var + ") {\n";
          c += indent + "    item.treeHashWith(h);\n";
          c += indent + "  }\n";
          c += indent + "  h.merkleizeWithMixin(subIdx, " + var +
               ".length, " + NumToString(limit) + ");\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          c += indent + "{\n";
          c += indent + "  const subIdx = h.index();\n";
          GenHashPrimitiveSlice(info, var, code, indent + "  ");
          c += indent + "  h.fillUpTo32();\n";
          uint64_t hash_limit =
              ssz_limit_for_elements(limit, info.elem_info->fixed_size);
          c += indent + "  h.merkleizeWithMixin(subIdx, " + var +
               ".length, " + NumToString(hash_limit) + ");\n";
          c += indent + "}\n";
        }
        break;
      }

      case SszType::Bitlist:
        c += indent + "h.putBitlist(" + var + ", " +
             NumToString(info.limit) + ");\n";
        break;

      case SszType::Bitvector:
        c += indent + "h.putBytes(" + var + ");\n";
        break;

      case SszType::Container:
        c += indent + var + ".treeHashWith(h);\n";
        break;

      case SszType::ProgressiveContainer:
        c += indent + var + ".treeHashWith(h);\n";
        break;

      case SszType::ProgressiveList: {
        if (info.elem_info &&
            info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "{\n";
          c += indent + "  const subIdx = h.index();\n";
          c += indent + "  h.appendBytes32(" + var + ");\n";
          c += indent + "  h.merkleizeProgressiveWithMixin(subIdx, " + var +
               ".length);\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "{\n";
          c += indent + "  const subIdx = h.index();\n";
          c += indent + "  for (const item of " + var + ") {\n";
          c += indent + "    item.treeHashWith(h);\n";
          c += indent + "  }\n";
          c += indent + "  h.merkleizeProgressiveWithMixin(subIdx, " + var +
               ".length);\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          c += indent + "{\n";
          c += indent + "  const subIdx = h.index();\n";
          GenHashPrimitiveSlice(info, var, code, indent + "  ");
          c += indent + "  h.fillUpTo32();\n";
          c += indent + "  h.merkleizeProgressiveWithMixin(subIdx, " + var +
               ".length);\n";
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
    c += indent + "for (const v of " + var + ") {\n";
    GenHashPrimitive(info.elem_info->ssz_type, "v", code, indent + "  ",
                     true);
    c += indent + "}\n";
  }

  void GenHashPrimitiveSlice(const SszFieldInfo &info, const std::string &var,
                             std::string *code, const std::string &indent) {
    std::string &c = *code;
    c += indent + "for (const v of " + var + ") {\n";
    GenHashPrimitive(info.elem_info->ssz_type, "v", code, indent + "  ",
                     true);
    c += indent + "}\n";
  }

  void GenHashPrimitive(SszType ssz_type, const std::string &var,
                        std::string *code, const std::string &indent,
                        bool append) {
    std::string &c = *code;
    std::string method = append ? "append" : "put";
    switch (ssz_type) {
      case SszType::Bool:
        c += indent + "h." + method + "Bool(" + var + ");\n";
        break;
      case SszType::Uint8:
        c += indent + "h." + method + "Uint8(" + var + ");\n";
        break;
      case SszType::Uint16:
        c += indent + "h." + method + "Uint16(" + var + ");\n";
        break;
      case SszType::Uint32:
        c += indent + "h." + method + "Uint32(" + var + ");\n";
        break;
      case SszType::Uint64:
        c += indent + "h." + method + "Uint64(" + var + ");\n";
        break;
      case SszType::Uint128:
      case SszType::Uint256:
      case SszType::Container:
      case SszType::Vector:
      case SszType::List:
      case SszType::Bitlist:
      case SszType::Bitvector:
      case SszType::Union:
      case SszType::ProgressiveContainer:
      case SszType::ProgressiveList:
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

  bool SaveFile(const StructDef &def, const std::string &classcode) {
    if (classcode.empty()) return true;

    Namespace &ns = *def.defined_namespace;

    std::string code;
    code += "// Code generated by the FlatBuffers compiler. DO NOT EDIT.\n\n";
    code += "import { Hasher, HasherPool, SszError } from './ssz_runtime';\n\n";

    code += classcode;

    // Strip trailing double newlines
    while (code.length() > 2 && code.substr(code.length() - 2) == "\n\n") {
      code.pop_back();
    }

    std::string directory = namer_.Directories(ns);
    EnsureDirExists(directory);

    // One file per schema
    std::string filename = directory + file_name_ + "_ssz.ts";
    return parser_.opts.file_saver->SaveFile(filename.c_str(), code, false);
  }
};

}  // namespace ssz_ts

// ---------------------------------------------------------------------------
// Code generator wrapper
// ---------------------------------------------------------------------------

static bool GenerateSszTs(const Parser &parser, const std::string &path,
                          const std::string &file_name) {
  ssz_ts::SszTsGenerator generator(parser, path, file_name);
  return generator.generate();
}

namespace {

class SszTsCodeGenerator : public CodeGenerator {
 public:
  Status GenerateCode(const Parser &parser, const std::string &path,
                      const std::string &filename) override {
    if (!GenerateSszTs(parser, path, filename)) { return Status::ERROR; }
    return Status::OK;
  }

  Status GenerateCode(const uint8_t * /*buffer*/, int64_t /*length*/,
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

  IDLOptions::Language Language() const override { return IDLOptions::kSszTs; }

  std::string LanguageName() const override { return "SszTs"; }
};

}  // namespace

std::unique_ptr<CodeGenerator> NewSszTsCodeGenerator() {
  return std::unique_ptr<SszTsCodeGenerator>(new SszTsCodeGenerator());
}

}  // namespace flatbuffers
