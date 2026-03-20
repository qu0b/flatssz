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

#include "idl_gen_ssz_csharp.h"

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

namespace ssz_csharp {

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

static std::set<std::string> CSharpKeywords() {
  return {
      "abstract", "as",       "base",      "bool",      "break",
      "byte",     "case",     "catch",     "char",      "checked",
      "class",    "const",    "continue",  "decimal",   "default",
      "delegate", "do",       "double",    "else",      "enum",
      "event",    "explicit", "extern",    "false",     "finally",
      "fixed",    "float",    "for",       "foreach",   "goto",
      "if",       "implicit", "in",        "int",       "interface",
      "internal", "is",       "lock",      "long",      "namespace",
      "new",      "null",     "object",    "operator",  "out",
      "override", "params",   "private",   "protected", "public",
      "readonly", "ref",      "return",    "sbyte",     "sealed",
      "short",    "sizeof",   "stackalloc","static",    "string",
      "struct",   "switch",   "this",      "throw",     "true",
      "try",      "typeof",   "uint",      "ulong",     "unchecked",
      "unsafe",   "ushort",   "using",     "virtual",   "void",
      "volatile", "while",
  };
}

static Namer::Config SszCSharpDefaultConfig() {
  return {/*types=*/Case::kUpperCamel,
          /*constants=*/Case::kUnknown,
          /*methods=*/Case::kUpperCamel,
          /*functions=*/Case::kUpperCamel,
          /*fields=*/Case::kUpperCamel,
          /*variables=*/Case::kLowerCamel,
          /*variants=*/Case::kKeep,
          /*enum_variant_seperator=*/"",
          /*escape_keywords=*/Namer::Config::Escape::AfterConvertingCase,
          /*namespaces=*/Case::kUpperCamel,
          /*namespace_seperator=*/".",
          /*object_prefix=*/"",
          /*object_suffix=*/"",
          /*keyword_prefix=*/"",
          /*keyword_suffix=*/"_",
          /*keywords_casing=*/Namer::Config::KeywordsCasing::CaseSensitive,
          /*filenames=*/Case::kKeep,
          /*directories=*/Case::kKeep,
          /*output_path=*/"",
          /*filename_suffix=*/"_ssz",
          /*filename_extension=*/".cs"};
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

class SszCSharpGenerator : public BaseGenerator {
 public:
  SszCSharpGenerator(const Parser &parser, const std::string &path,
                     const std::string &file_name)
      : BaseGenerator(parser, path, file_name, "", "", "cs"),
        namer_(WithFlagOptions(SszCSharpDefaultConfig(), parser.opts, path),
               CSharpKeywords()) {}

  bool generate() override {
    std::string all_code;

    // Determine namespace
    std::string ns_name = "SszGenerated";
    if (!parser_.structs_.vec.empty()) {
      auto &first_struct = *parser_.structs_.vec.front();
      if (first_struct.defined_namespace &&
          !first_struct.defined_namespace->components.empty()) {
        ns_name = FullNamespace(".", *first_struct.defined_namespace);
      }
    }

    // File header
    all_code += "// Code generated by the FlatBuffers compiler. DO NOT EDIT.\n\n";
    all_code += "using System;\n";
    all_code += "using System.Buffers.Binary;\n";
    all_code += "using System.Collections.Generic;\n";
    all_code += "using SszFlatbuffers;\n";
    all_code += "\n";
    all_code += "namespace " + ns_name + "\n";
    all_code += "{\n";

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

    all_code += "}\n";

    // Save single file per schema
    std::string filename = path_ + ToSnakeCase(file_name_) + "_ssz.cs";
    EnsureDirExists(path_);
    return parser_.opts.file_saver->SaveFile(filename.c_str(), all_code, false);
  }

 private:
  const IdlNamer namer_;

  // -----------------------------------------------------------------------
  // Type resolution (identical logic to Go backend)
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
  // C# type helpers
  // -----------------------------------------------------------------------

  std::string CSharpTypeName(const SszFieldInfo &info, const FieldDef &field) {
    switch (info.ssz_type) {
      case SszType::Bool:
        return "bool";
      case SszType::Uint8:
        return "byte";
      case SszType::Uint16:
        return "ushort";
      case SszType::Uint32:
        return "uint";
      case SszType::Uint64:
        return "ulong";
      case SszType::Uint128:
        return "byte[]";
      case SszType::Uint256:
        return "byte[]";
      case SszType::Container:
        if (info.struct_def) { return info.struct_def->name; }
        return "object";
      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          return "byte[]";
        }
        if (info.elem_info &&
            info.elem_info->ssz_type == SszType::Container) {
          return info.elem_info->struct_def->name + "[]";
        }
        if (info.elem_info) {
          return CSharpPrimitiveTypeName(info.elem_info->ssz_type) + "[]";
        }
        return "byte[]";
      }
      case SszType::ProgressiveList:
      case SszType::List: {
        if (field.value.type.base_type == BASE_TYPE_STRING) {
          return "byte[]";
        }
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          return "byte[]";
        }
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          return "List<byte[]>";
        }
        if (info.elem_info &&
            info.elem_info->ssz_type == SszType::Container) {
          return "List<" + info.elem_info->struct_def->name + ">";
        }
        if (info.elem_info) {
          return CSharpPrimitiveTypeName(info.elem_info->ssz_type) + "[]";
        }
        return "byte[]";
      }
      case SszType::Bitlist:
        return "byte[]";
      case SszType::Bitvector:
        return "byte[]";
      case SszType::ProgressiveContainer:
        if (info.struct_def) { return info.struct_def->name; }
        return "object";
      case SszType::Union:
        return "object";
    }
    return "object";
  }

  static std::string CSharpPrimitiveTypeName(SszType t) {
    switch (t) {
      case SszType::Bool: return "bool";
      case SszType::Uint8: return "byte";
      case SszType::Uint16: return "ushort";
      case SszType::Uint32: return "uint";
      case SszType::Uint64: return "ulong";
      default: return "byte";
    }
  }

  // -----------------------------------------------------------------------
  // Field initializer for constructor (fixed-size byte arrays)
  // -----------------------------------------------------------------------

  std::string FieldInitializer(const SszFieldInfo &info) {
    switch (info.ssz_type) {
      case SszType::Uint128:
        return "new byte[16]";
      case SszType::Uint256:
        return "new byte[32]";
      case SszType::Vector:
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          return "new byte[" + NumToString(info.limit) + "]";
        }
        if (info.elem_info &&
            info.elem_info->ssz_type == SszType::Container) {
          return "new " + info.elem_info->struct_def->name + "[" +
                 NumToString(info.limit) + "]";
        }
        if (info.elem_info) {
          return "new " + CSharpPrimitiveTypeName(info.elem_info->ssz_type) +
                 "[" + NumToString(info.limit) + "]";
        }
        return "new byte[0]";
      case SszType::Bitvector:
        return "new byte[" + NumToString((info.bitsize + 7) / 8) + "]";
      case SszType::Container:
        if (info.struct_def) {
          return "new " + info.struct_def->name + "()";
        }
        return "null";
      default:
        return "";
    }
  }

  bool NeedsConstructorInit(const SszFieldInfo &info) {
    switch (info.ssz_type) {
      case SszType::Uint128:
      case SszType::Uint256:
      case SszType::Vector:
      case SszType::Bitvector:
        return true;
      case SszType::Container:
        return info.struct_def != nullptr;
      default:
        return false;
    }
  }

  // -----------------------------------------------------------------------
  // Code generation entry point
  // -----------------------------------------------------------------------

  bool GenerateType(const StructDef &struct_def,
                    const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;

    c += "    public class " + struct_def.name + "\n";
    c += "    {\n";

    // Fields
    GenCSharpFields(container, &c);
    c += "\n";

    // Constructor
    GenCSharpConstructor(struct_def, container, &c);
    c += "\n";

    // SszBytesLen
    GenSszBytesLen(container, &c);
    c += "\n";

    // MarshalSSZ
    GenMarshalSSZ(&c);
    c += "\n";

    // MarshalSSZTo
    GenMarshalSSZTo(container, &c);
    c += "\n";

    // FromSSZBytes
    GenFromSSZBytes(struct_def, container, &c);
    c += "\n";

    // HashTreeRoot
    GenHashTreeRoot(container, &c);

    c += "    }\n";

    return true;
  }

  // -----------------------------------------------------------------------
  // C# class generation
  // -----------------------------------------------------------------------

  void GenCSharpFields(const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      auto &info = it->second;
      c += "        public " + CSharpTypeName(info, *field) + " " +
           namer_.Field(*field) + ";\n";
    }
  }

  void GenCSharpConstructor(const StructDef &struct_def,
                            const SszContainerInfo &container,
                            std::string *code) {
    std::string &c = *code;

    // Check if any fields need constructor initialization
    bool needs_ctor = false;
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      if (NeedsConstructorInit(it->second)) { needs_ctor = true; break; }
    }
    if (!needs_ctor) return;

    c += "        public " + struct_def.name + "()\n";
    c += "        {\n";
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      auto &info = it->second;
      if (NeedsConstructorInit(info)) {
        c += "            " + namer_.Field(*field) + " = " +
             FieldInitializer(info) + ";\n";
      }
    }
    c += "        }\n";
  }

  // -----------------------------------------------------------------------
  // SszBytesLen generation
  // -----------------------------------------------------------------------

  void GenSszBytesLen(const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    c += "        public int SszBytesLen()\n";
    c += "        {\n";

    if (container.is_fixed) {
      c += "            return " + NumToString(container.static_size) + ";\n";
    } else {
      c += "            int size = " +
           NumToString(container.static_size) + ";\n";
      for (auto *field : container.dynamic_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = namer_.Field(*field);
        GenSizeField(info, fname, &c, "            ");
      }
      c += "            return size;\n";
    }
    c += "        }\n";
  }

  void GenSizeField(const SszFieldInfo &info, const std::string &var,
                    std::string *code, const std::string &indent) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::ProgressiveList:
      case SszType::List: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          // List of byte lists: offsets + each element's length
          c += indent + "size += " + var + ".Count * 4;\n";
          c += indent + "foreach (var item in " + var + ")\n";
          c += indent + "    size += item.Length;\n";
        } else if (info.elem_info &&
            info.elem_info->ssz_type == SszType::Container &&
            info.elem_info->is_dynamic) {
          c += indent + "size += " + var + ".Count * 4;\n";
          c += indent + "foreach (var item in " + var + ")\n";
          c += indent + "    size += item.SszBytesLen();\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "size += " + var + ".Count * " +
               NumToString(info.elem_info->fixed_size) + ";\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "size += " + var + ".Length;\n";
        } else if (info.elem_info) {
          c += indent + "size += " + var + ".Length * " +
               NumToString(info.elem_info->fixed_size) + ";\n";
        } else {
          c += indent + "size += " + var + ".Length;\n";
        }
        break;
      }
      case SszType::Bitlist:
        c += indent + "size += " + var + ".Length;\n";
        break;
      case SszType::Container:
        c += indent + "size += " + var + ".SszBytesLen();\n";
        break;
      default:
        break;
    }
  }

  // -----------------------------------------------------------------------
  // MarshalSSZ generation
  // -----------------------------------------------------------------------

  void GenMarshalSSZ(std::string *code) {
    std::string &c = *code;
    c += "        public byte[] MarshalSSZ()\n";
    c += "        {\n";
    c += "            byte[] buf = new byte[SszBytesLen()];\n";
    c += "            MarshalSSZTo(buf);\n";
    c += "            return buf;\n";
    c += "        }\n";
  }

  void GenMarshalSSZTo(const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    c += "        public void MarshalSSZTo(Span<byte> buf)\n";
    c += "        {\n";
    c += "            int offset = 0;\n";

    if (container.dynamic_fields.empty()) {
      // All-fixed container: just marshal fields sequentially
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        GenMarshalField(it->second, namer_.Field(*field), &c,
                        "            ");
      }
    } else {
      // Container with dynamic fields
      c += "            int fixedStart = offset;\n";
      c += "\n";

      // Static section
      int dyn_idx = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = namer_.Field(*field);

        if (info.is_dynamic) {
          c += "            // Offset placeholder for '" + field->name + "'\n";
          c += "            int offsetPos" + NumToString(dyn_idx) +
               " = offset;\n";
          c += "            offset += 4;\n";
          dyn_idx++;
        } else {
          c += "            // Field '" + field->name + "'\n";
          GenMarshalField(info, fname, &c, "            ");
        }
      }

      c += "\n";

      // Dynamic section: backpatch offsets and marshal dynamic fields
      dyn_idx = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = namer_.Field(*field);

        if (info.is_dynamic) {
          c += "            // Dynamic field '" + field->name + "'\n";
          c += "            BinaryPrimitives.WriteUInt32LittleEndian("
               "buf.Slice(offsetPos" + NumToString(dyn_idx) +
               "), (uint)(offset - fixedStart));\n";
          GenMarshalField(info, fname, &c, "            ");
          dyn_idx++;
        }
      }
    }

    c += "        }\n";
  }

  void GenMarshalField(const SszFieldInfo &info, const std::string &var,
                       std::string *code, const std::string &indent) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::Bool:
        c += indent + "buf[offset] = (byte)(" + var + " ? 1 : 0);\n";
        c += indent + "offset += 1;\n";
        break;

      case SszType::Uint8:
        c += indent + "buf[offset] = " + var + ";\n";
        c += indent + "offset += 1;\n";
        break;

      case SszType::Uint16:
        c += indent + "BinaryPrimitives.WriteUInt16LittleEndian("
             "buf.Slice(offset), " + var + ");\n";
        c += indent + "offset += 2;\n";
        break;

      case SszType::Uint32:
        c += indent + "BinaryPrimitives.WriteUInt32LittleEndian("
             "buf.Slice(offset), " + var + ");\n";
        c += indent + "offset += 4;\n";
        break;

      case SszType::Uint64:
        c += indent + "BinaryPrimitives.WriteUInt64LittleEndian("
             "buf.Slice(offset), " + var + ");\n";
        c += indent + "offset += 8;\n";
        break;

      case SszType::Uint128:
        c += indent + var + ".AsSpan().CopyTo(buf.Slice(offset, 16));\n";
        c += indent + "offset += 16;\n";
        break;

      case SszType::Uint256:
        c += indent + var + ".AsSpan().CopyTo(buf.Slice(offset, 32));\n";
        c += indent + "offset += 32;\n";
        break;

      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + var + ".AsSpan().CopyTo(buf.Slice(offset, " +
               NumToString(info.fixed_size) + "));\n";
          c += indent + "offset += " + NumToString(info.fixed_size) + ";\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "for (int i = 0; i < " + var + ".Length; i++)\n";
          c += indent + "{\n";
          c += indent + "    " + var +
               "[i].MarshalSSZTo(buf.Slice(offset));\n";
          c += indent + "    offset += " +
               NumToString(info.elem_info->fixed_size) + ";\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          GenMarshalPrimitiveArray(info, var, code, indent);
        }
        break;
      }

      case SszType::ProgressiveList:
      case SszType::List: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          // List of byte lists: offset-based dynamic encoding
          c += indent + "{\n";
          c += indent + "    int listStart = offset;\n";
          c += indent + "    for (int i = 0; i < " + var + ".Count; i++)\n";
          c += indent + "        offset += 4;\n";
          c += indent + "    for (int i = 0; i < " + var + ".Count; i++)\n";
          c += indent + "    {\n";
          c += indent + "        BinaryPrimitives.WriteUInt32LittleEndian("
               "buf.Slice(listStart + i * 4), "
               "(uint)(offset - listStart));\n";
          c += indent + "        " + var +
               "[i].AsSpan().CopyTo(buf.Slice(offset));\n";
          c += indent + "        offset += " + var + "[i].Length;\n";
          c += indent + "    }\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + var + ".AsSpan().CopyTo(buf.Slice(offset));\n";
          c += indent + "offset += " + var + ".Length;\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          if (info.elem_info->is_dynamic) {
            c += indent + "{\n";
            c += indent + "    int listStart = offset;\n";
            c += indent + "    for (int i = 0; i < " + var +
                 ".Count; i++)\n";
            c += indent + "        offset += 4;\n";
            c += indent + "    for (int i = 0; i < " + var +
                 ".Count; i++)\n";
            c += indent + "    {\n";
            c += indent +
                 "        BinaryPrimitives.WriteUInt32LittleEndian("
                 "buf.Slice(listStart + i * 4), "
                 "(uint)(offset - listStart));\n";
            c += indent + "        " + var +
                 "[i].MarshalSSZTo(buf.Slice(offset));\n";
            c += indent + "        offset += " + var +
                 "[i].SszBytesLen();\n";
            c += indent + "    }\n";
            c += indent + "}\n";
          } else {
            c += indent + "foreach (var item in " + var + ")\n";
            c += indent + "{\n";
            c += indent + "    item.MarshalSSZTo(buf.Slice(offset));\n";
            c += indent + "    offset += " +
                 NumToString(info.elem_info->fixed_size) + ";\n";
            c += indent + "}\n";
          }
        } else if (info.elem_info) {
          GenMarshalPrimitiveSlice(info, var, code, indent);
        } else {
          c += indent + var + ".AsSpan().CopyTo(buf.Slice(offset));\n";
          c += indent + "offset += " + var + ".Length;\n";
        }
        break;
      }

      case SszType::Bitlist:
        c += indent + var + ".AsSpan().CopyTo(buf.Slice(offset));\n";
        c += indent + "offset += " + var + ".Length;\n";
        break;

      case SszType::Bitvector:
        c += indent + var + ".AsSpan().CopyTo(buf.Slice(offset, " +
             NumToString(info.fixed_size) + "));\n";
        c += indent + "offset += " + NumToString(info.fixed_size) + ";\n";
        break;

      case SszType::Container:
        c += indent + var + ".MarshalSSZTo(buf.Slice(offset));\n";
        c += indent + "offset += " + NumToString(info.fixed_size) + ";\n";
        break;

      case SszType::ProgressiveContainer:
        c += indent + var + ".MarshalSSZTo(buf.Slice(offset));\n";
        c += indent + "offset += " + var + ".SszBytesLen();\n";
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
    c += indent + "for (int i = 0; i < " + var + ".Length; i++)\n";
    c += indent + "{\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "    buf[offset] = (byte)(" + var +
             "[i] ? 1 : 0);\n";
        c += indent + "    offset += 1;\n";
        break;
      case SszType::Uint16:
        c += indent + "    BinaryPrimitives.WriteUInt16LittleEndian("
             "buf.Slice(offset), " + var + "[i]);\n";
        c += indent + "    offset += 2;\n";
        break;
      case SszType::Uint32:
        c += indent + "    BinaryPrimitives.WriteUInt32LittleEndian("
             "buf.Slice(offset), " + var + "[i]);\n";
        c += indent + "    offset += 4;\n";
        break;
      case SszType::Uint64:
        c += indent + "    BinaryPrimitives.WriteUInt64LittleEndian("
             "buf.Slice(offset), " + var + "[i]);\n";
        c += indent + "    offset += 8;\n";
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
    c += indent + "for (int i = 0; i < " + var + ".Length; i++)\n";
    c += indent + "{\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "    buf[offset] = (byte)(" + var +
             "[i] ? 1 : 0);\n";
        c += indent + "    offset += 1;\n";
        break;
      case SszType::Uint16:
        c += indent + "    BinaryPrimitives.WriteUInt16LittleEndian("
             "buf.Slice(offset), " + var + "[i]);\n";
        c += indent + "    offset += 2;\n";
        break;
      case SszType::Uint32:
        c += indent + "    BinaryPrimitives.WriteUInt32LittleEndian("
             "buf.Slice(offset), " + var + "[i]);\n";
        c += indent + "    offset += 4;\n";
        break;
      case SszType::Uint64:
        c += indent + "    BinaryPrimitives.WriteUInt64LittleEndian("
             "buf.Slice(offset), " + var + "[i]);\n";
        c += indent + "    offset += 8;\n";
        break;
      default:
        break;
    }
    c += indent + "}\n";
  }

  // -----------------------------------------------------------------------
  // FromSSZBytes generation
  // -----------------------------------------------------------------------

  void GenFromSSZBytes(const StructDef &struct_def,
                       const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    std::string type_name = struct_def.name;

    c += "        public static " + type_name +
         " FromSSZBytes(ReadOnlySpan<byte> bytes)\n";
    c += "        {\n";

    // Validate minimum size
    c += "            if (bytes.Length < " +
         NumToString(container.static_size) + ")\n";
    c += "                throw new SszError(\"buffer too small\");\n\n";
    c += "            var t = new " + type_name + "();\n";
    c += "            int offset = 0;\n\n";

    if (container.dynamic_fields.empty()) {
      // All-fixed container
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "t." + namer_.Field(*field);
        GenUnmarshalField(info, fname, &c, "            ");
      }
    } else {
      // Container with dynamic fields
      int dyn_idx = 0;
      std::vector<std::pair<const FieldDef *, int>> dyn_offset_names;

      // Read static fields and offsets
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "t." + namer_.Field(*field);

        if (info.is_dynamic) {
          c += "            int dynOffset" + NumToString(dyn_idx) +
               " = (int)BinaryPrimitives.ReadUInt32LittleEndian("
               "bytes.Slice(offset));\n";
          c += "            offset += 4;\n";
          dyn_offset_names.push_back({field, dyn_idx});

          if (dyn_idx == 0) {
            c += "            if (dynOffset0 != " +
                 NumToString(container.static_size) + ")\n";
            c += "                throw new SszError(\"invalid offset\");\n";
          } else {
            c += "            if (dynOffset" + NumToString(dyn_idx) +
                 " < dynOffset" + NumToString(dyn_idx - 1) +
                 " || dynOffset" + NumToString(dyn_idx) +
                 " > bytes.Length)\n";
            c += "                throw new SszError(\"invalid offset\");\n";
          }
          dyn_idx++;
        } else {
          GenUnmarshalField(info, fname, &c, "            ");
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

        std::string start_off = "dynOffset" + NumToString(oidx);
        std::string end_off;
        if (i + 1 < dyn_offset_names.size()) {
          end_off = "dynOffset" + NumToString(dyn_offset_names[i + 1].second);
        } else {
          end_off = "bytes.Length";
        }

        c += "            {\n";
        c += "                var dynBuf = bytes.Slice(" + start_off + ", " +
             end_off + " - " + start_off + ");\n";
        GenUnmarshalDynField(info, fname, *field, &c,
                             "                ");
        c += "            }\n";
      }
    }

    c += "\n            return t;\n";
    c += "        }\n";
  }

  void GenUnmarshalField(const SszFieldInfo &info, const std::string &var,
                         std::string *code, const std::string &indent) {
    std::string &c = *code;

    switch (info.ssz_type) {
      case SszType::Bool:
        c += indent + "if (bytes[offset] > 1)\n";
        c += indent + "    throw new SszError(\"invalid bool\");\n";
        c += indent + var + " = bytes[offset] == 1;\n";
        c += indent + "offset += 1;\n";
        break;

      case SszType::Uint8:
        c += indent + var + " = bytes[offset];\n";
        c += indent + "offset += 1;\n";
        break;

      case SszType::Uint16:
        c += indent + var +
             " = BinaryPrimitives.ReadUInt16LittleEndian("
             "bytes.Slice(offset));\n";
        c += indent + "offset += 2;\n";
        break;

      case SszType::Uint32:
        c += indent + var +
             " = BinaryPrimitives.ReadUInt32LittleEndian("
             "bytes.Slice(offset));\n";
        c += indent + "offset += 4;\n";
        break;

      case SszType::Uint64:
        c += indent + var +
             " = BinaryPrimitives.ReadUInt64LittleEndian("
             "bytes.Slice(offset));\n";
        c += indent + "offset += 8;\n";
        break;

      case SszType::Uint128:
        c += indent + "bytes.Slice(offset, 16).CopyTo(" + var +
             ".AsSpan());\n";
        c += indent + "offset += 16;\n";
        break;

      case SszType::Uint256:
        c += indent + "bytes.Slice(offset, 32).CopyTo(" + var +
             ".AsSpan());\n";
        c += indent + "offset += 32;\n";
        break;

      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "bytes.Slice(offset, " +
               NumToString(info.fixed_size) + ").CopyTo(" + var +
               ".AsSpan());\n";
          c += indent + "offset += " + NumToString(info.fixed_size) + ";\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          uint32_t elem_size = info.elem_info->fixed_size;
          c += indent + "for (int i = 0; i < " + var + ".Length; i++)\n";
          c += indent + "{\n";
          c += indent + "    " + var + "[i] = " +
               info.elem_info->struct_def->name +
               ".FromSSZBytes(bytes.Slice(offset, " +
               NumToString(elem_size) + "));\n";
          c += indent + "    offset += " + NumToString(elem_size) + ";\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          GenUnmarshalPrimitiveArray(info, var, code, indent);
        }
        break;
      }

      case SszType::Bitvector:
        c += indent + "bytes.Slice(offset, " +
             NumToString(info.fixed_size) + ").CopyTo(" + var +
             ".AsSpan());\n";
        c += indent + "offset += " + NumToString(info.fixed_size) + ";\n";
        break;

      case SszType::Container: {
        c += indent + var + " = " + info.struct_def->name +
             ".FromSSZBytes(bytes.Slice(offset, " +
             NumToString(info.fixed_size) + "));\n";
        c += indent + "offset += " + NumToString(info.fixed_size) + ";\n";
        break;
      }

      default:
        break;
    }
  }

  void GenUnmarshalPrimitiveArray(const SszFieldInfo &info,
                                  const std::string &var,
                                  std::string *code,
                                  const std::string &indent) {
    std::string &c = *code;
    uint32_t elem_size = info.elem_info->fixed_size;
    c += indent + "for (int i = 0; i < " + var + ".Length; i++)\n";
    c += indent + "{\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "    if (bytes[offset] > 1)\n";
        c += indent + "        throw new SszError(\"invalid bool\");\n";
        c += indent + "    " + var + "[i] = bytes[offset] == 1;\n";
        c += indent + "    offset += 1;\n";
        break;
      case SszType::Uint16:
        c += indent + "    " + var +
             "[i] = BinaryPrimitives.ReadUInt16LittleEndian("
             "bytes.Slice(offset));\n";
        c += indent + "    offset += " + NumToString(elem_size) + ";\n";
        break;
      case SszType::Uint32:
        c += indent + "    " + var +
             "[i] = BinaryPrimitives.ReadUInt32LittleEndian("
             "bytes.Slice(offset));\n";
        c += indent + "    offset += " + NumToString(elem_size) + ";\n";
        break;
      case SszType::Uint64:
        c += indent + "    " + var +
             "[i] = BinaryPrimitives.ReadUInt64LittleEndian("
             "bytes.Slice(offset));\n";
        c += indent + "    offset += " + NumToString(elem_size) + ";\n";
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
          // List of byte lists
          c += indent + "if (dynBuf.Length > 0)\n";
          c += indent + "{\n";
          c += indent + "    int firstOff = (int)BinaryPrimitives."
               "ReadUInt32LittleEndian(dynBuf);\n";
          c += indent + "    if (firstOff % 4 != 0)\n";
          c += indent + "        throw new SszError(\"invalid offset\");\n";
          c += indent + "    int count = firstOff / 4;\n";
          c += indent + "    " + var + " = new List<byte[]>(count);\n";
          c += indent + "    for (int i = 0; i < count; i++)\n";
          c += indent + "    {\n";
          c += indent + "        int start = (int)BinaryPrimitives."
               "ReadUInt32LittleEndian(dynBuf.Slice(i * 4));\n";
          c += indent + "        int end = (i + 1 < count) ? "
               "(int)BinaryPrimitives.ReadUInt32LittleEndian("
               "dynBuf.Slice((i + 1) * 4)) : dynBuf.Length;\n";
          c += indent + "        if (start > end || end > dynBuf.Length)\n";
          c += indent +
               "            throw new SszError(\"invalid offset\");\n";
          c += indent + "        " + var +
               ".Add(dynBuf.Slice(start, end - start).ToArray());\n";
          c += indent + "    }\n";
          c += indent + "}\n";
          c += indent + "else\n";
          c += indent + "{\n";
          c += indent + "    " + var + " = new List<byte[]>();\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + var + " = dynBuf.ToArray();\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          if (info.elem_info->is_dynamic) {
            c += indent + "if (dynBuf.Length > 0)\n";
            c += indent + "{\n";
            c += indent + "    int firstOff = (int)BinaryPrimitives."
                 "ReadUInt32LittleEndian(dynBuf);\n";
            c += indent + "    if (firstOff % 4 != 0)\n";
            c += indent +
                 "        throw new SszError(\"invalid offset\");\n";
            c += indent + "    int count = firstOff / 4;\n";
            c += indent + "    " + var + " = new " +
                 CSharpTypeName(info, field) + "(count);\n";
            c += indent + "    for (int i = 0; i < count; i++)\n";
            c += indent + "    {\n";
            c += indent + "        int start = (int)BinaryPrimitives."
                 "ReadUInt32LittleEndian(dynBuf.Slice(i * 4));\n";
            c += indent + "        int end = (i + 1 < count) ? "
                 "(int)BinaryPrimitives.ReadUInt32LittleEndian("
                 "dynBuf.Slice((i + 1) * 4)) : dynBuf.Length;\n";
            c += indent +
                 "        if (start > end || end > dynBuf.Length)\n";
            c += indent +
                 "            throw new SszError(\"invalid offset\");\n";
            c += indent + "        " + var + ".Add(" +
                 info.elem_info->struct_def->name +
                 ".FromSSZBytes(dynBuf.Slice(start, end - start)));\n";
            c += indent + "    }\n";
            c += indent + "}\n";
            c += indent + "else\n";
            c += indent + "{\n";
            c += indent + "    " + var + " = new " +
                 CSharpTypeName(info, field) + "();\n";
            c += indent + "}\n";
          } else {
            uint32_t elem_size = info.elem_info->fixed_size;
            c += indent + "{\n";
            c += indent + "    int count = dynBuf.Length / " +
                 NumToString(elem_size) + ";\n";
            c += indent + "    " + var + " = new " +
                 CSharpTypeName(info, field) + "(count);\n";
            c += indent + "    for (int i = 0; i < count; i++)\n";
            c += indent + "    {\n";
            c += indent + "        int off = i * " +
                 NumToString(elem_size) + ";\n";
            c += indent + "        " + var + ".Add(" +
                 info.elem_info->struct_def->name +
                 ".FromSSZBytes(dynBuf.Slice(off, " +
                 NumToString(elem_size) + ")));\n";
            c += indent + "    }\n";
            c += indent + "}\n";
          }
        } else if (info.elem_info) {
          GenUnmarshalPrimitiveSlice(info, var, code, indent);
        } else {
          c += indent + var + " = dynBuf.ToArray();\n";
        }
        break;
      }
      case SszType::Bitlist:
        c += indent + var + " = dynBuf.ToArray();\n";
        break;
      case SszType::Container:
        c += indent + var + " = " + info.struct_def->name +
             ".FromSSZBytes(dynBuf);\n";
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
    c += indent + "{\n";
    c += indent + "    int count = dynBuf.Length / " +
         NumToString(elem_size) + ";\n";
    c += indent + "    " + var + " = new " +
         CSharpPrimitiveTypeName(info.elem_info->ssz_type) +
         "[count];\n";
    c += indent + "    for (int i = 0; i < count; i++)\n";
    c += indent + "    {\n";
    c += indent + "        int off = i * " + NumToString(elem_size) + ";\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "        if (dynBuf[off] > 1)\n";
        c += indent +
             "            throw new SszError(\"invalid bool\");\n";
        c += indent + "        " + var + "[i] = dynBuf[off] == 1;\n";
        break;
      case SszType::Uint16:
        c += indent + "        " + var +
             "[i] = BinaryPrimitives.ReadUInt16LittleEndian("
             "dynBuf.Slice(off));\n";
        break;
      case SszType::Uint32:
        c += indent + "        " + var +
             "[i] = BinaryPrimitives.ReadUInt32LittleEndian("
             "dynBuf.Slice(off));\n";
        break;
      case SszType::Uint64:
        c += indent + "        " + var +
             "[i] = BinaryPrimitives.ReadUInt64LittleEndian("
             "dynBuf.Slice(off));\n";
        break;
      default:
        break;
    }
    c += indent + "    }\n";
    c += indent + "}\n";
  }

  // -----------------------------------------------------------------------
  // HashTreeRoot generation
  // -----------------------------------------------------------------------

  void GenHashTreeRoot(const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;

    c += "        public byte[] HashTreeRoot()\n";
    c += "        {\n";
    c += "            var hh = Hasher.New();\n";
    c += "            int idx = hh.Index();\n";
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
          c += "            // Index " + NumToString(i) + ": " +
               field->name + "\n";
          GenHashField(iit->second, namer_.Field(*field), &c,
                       "            ");
        } else {
          c += "            // Index " + NumToString(i) +
               ": gap (inactive)\n";
          c += "            hh.PutUint8(0);\n";
        }
      }
      c += "\n";

      // Emit active_fields bitvector literal
      c += "            hh.MerkleizeProgressiveWithActiveFields(idx, "
           "new byte[] { ";
      for (size_t i = 0; i < container.active_fields_bitvector.size(); i++) {
        if (i > 0) c += ", ";
        char hex[8];
        snprintf(hex, sizeof(hex), "0x%02x",
                 container.active_fields_bitvector[i]);
        c += hex;
      }
      c += " });\n";
    } else {
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = namer_.Field(*field);

        c += "            // Field '" + field->name + "'\n";
        GenHashField(info, fname, &c, "            ");
        c += "\n";
      }

      c += "            hh.Merkleize(idx);\n";
    }

    c += "            return hh.HashRoot();\n";
    c += "        }\n";
  }

  void GenHashField(const SszFieldInfo &info, const std::string &var,
                    std::string *code, const std::string &indent) {
    std::string &c = *code;

    switch (info.ssz_type) {
      case SszType::Bool:
        c += indent + "hh.PutBool(" + var + ");\n";
        break;

      case SszType::Uint8:
        c += indent + "hh.PutUint8(" + var + ");\n";
        break;

      case SszType::Uint16:
        c += indent + "hh.PutUint16(" + var + ");\n";
        break;

      case SszType::Uint32:
        c += indent + "hh.PutUint32(" + var + ");\n";
        break;

      case SszType::Uint64:
        c += indent + "hh.PutUint64(" + var + ");\n";
        break;

      case SszType::Uint128:
      case SszType::Uint256:
        c += indent + "hh.PutBytes(" + var + ");\n";
        break;

      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "hh.PutBytes(" + var + ");\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "{\n";
          c += indent + "    int subIdx = hh.Index();\n";
          c += indent + "    for (int i = 0; i < " + var +
               ".Length; i++)\n";
          c += indent + "        " + var +
               "[i].HashTreeRootWith(hh);\n";
          c += indent + "    hh.Merkleize(subIdx);\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          c += indent + "{\n";
          c += indent + "    int subIdx = hh.Index();\n";
          GenHashPrimitiveArray(info, var, code, indent + "    ");
          c += indent + "    hh.FillUpTo32();\n";
          c += indent + "    hh.Merkleize(subIdx);\n";
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
          c += indent + "    int subIdx = hh.Index();\n";
          c += indent + "    foreach (var item in " + var + ")\n";
          c += indent + "    {\n";
          c += indent + "        int inner = hh.Index();\n";
          c += indent + "        hh.AppendBytes32(item);\n";
          if (inner_chunk_limit > 0) {
            c += indent +
                 "        hh.MerkleizeWithMixin(inner, (ulong)item.Length, " +
                 NumToString(inner_chunk_limit) + ");\n";
          } else {
            c += indent + "        hh.Merkleize(inner);\n";
          }
          c += indent + "    }\n";
          c += indent + "    hh.MerkleizeWithMixin(subIdx, (ulong)" + var +
               ".Count, " + NumToString(limit) + ");\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "{\n";
          c += indent + "    int subIdx = hh.Index();\n";
          c += indent + "    hh.AppendBytes32(" + var + ");\n";
          c += indent + "    hh.MerkleizeWithMixin(subIdx, (ulong)" + var +
               ".Length, " + NumToString(ssz_limit_for_bytes(limit)) +
               ");\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "{\n";
          c += indent + "    int subIdx = hh.Index();\n";
          c += indent + "    foreach (var item in " + var + ")\n";
          c += indent + "        item.HashTreeRootWith(hh);\n";
          c += indent + "    hh.MerkleizeWithMixin(subIdx, (ulong)" + var +
               ".Count, " + NumToString(limit) + ");\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          c += indent + "{\n";
          c += indent + "    int subIdx = hh.Index();\n";
          GenHashPrimitiveSlice(info, var, code, indent + "    ");
          c += indent + "    hh.FillUpTo32();\n";
          uint64_t hash_limit =
              ssz_limit_for_elements(limit, info.elem_info->fixed_size);
          c += indent + "    hh.MerkleizeWithMixin(subIdx, (ulong)" + var +
               ".Length, " + NumToString(hash_limit) + ");\n";
          c += indent + "}\n";
        }
        break;
      }

      case SszType::Bitlist:
        c += indent + "hh.PutBitlist(" + var + ", " +
             NumToString(info.limit) + ");\n";
        break;

      case SszType::Bitvector:
        c += indent + "hh.PutBytes(" + var + ");\n";
        break;

      case SszType::Container:
        c += indent + var + ".HashTreeRootWith(hh);\n";
        break;

      case SszType::ProgressiveContainer:
        c += indent + var + ".HashTreeRootWith(hh);\n";
        break;

      case SszType::ProgressiveList: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "{\n";
          c += indent + "    int subIdx = hh.Index();\n";
          c += indent + "    hh.AppendBytes32(" + var + ");\n";
          c += indent + "    hh.MerkleizeProgressiveWithMixin(subIdx, "
               "(ulong)" + var + ".Length);\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "{\n";
          c += indent + "    int subIdx = hh.Index();\n";
          c += indent + "    foreach (var item in " + var + ")\n";
          c += indent + "        item.HashTreeRootWith(hh);\n";
          c += indent + "    hh.MerkleizeProgressiveWithMixin(subIdx, "
               "(ulong)" + var + ".Count);\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          c += indent + "{\n";
          c += indent + "    int subIdx = hh.Index();\n";
          GenHashPrimitiveSlice(info, var, code, indent + "    ");
          c += indent + "    hh.FillUpTo32();\n";
          c += indent + "    hh.MerkleizeProgressiveWithMixin(subIdx, "
               "(ulong)" + var + ".Length);\n";
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
    c += indent + "for (int i = 0; i < " + var + ".Length; i++)\n";
    GenHashPrimitive(info.elem_info->ssz_type, var + "[i]", code,
                     indent + "    ", true);
  }

  void GenHashPrimitiveSlice(const SszFieldInfo &info, const std::string &var,
                             std::string *code, const std::string &indent) {
    std::string &c = *code;
    c += indent + "for (int i = 0; i < " + var + ".Length; i++)\n";
    GenHashPrimitive(info.elem_info->ssz_type, var + "[i]", code,
                     indent + "    ", true);
  }

  void GenHashPrimitive(SszType ssz_type, const std::string &var,
                        std::string *code, const std::string &indent,
                        bool append) {
    std::string &c = *code;
    std::string method = append ? "Append" : "Put";
    switch (ssz_type) {
      case SszType::Bool:
        c += indent + "hh." + method + "Bool(" + var + ");\n";
        break;
      case SszType::Uint8:
        c += indent + "hh." + method + "Uint8(" + var + ");\n";
        break;
      case SszType::Uint16:
        c += indent + "hh." + method + "Uint16(" + var + ");\n";
        break;
      case SszType::Uint32:
        c += indent + "hh." + method + "Uint32(" + var + ");\n";
        break;
      case SszType::Uint64:
        c += indent + "hh." + method + "Uint64(" + var + ");\n";
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
};

}  // namespace ssz_csharp

// ---------------------------------------------------------------------------
// Code generator wrapper
// ---------------------------------------------------------------------------

static bool GenerateSszCSharp(const Parser &parser, const std::string &path,
                              const std::string &file_name) {
  ssz_csharp::SszCSharpGenerator generator(parser, path, file_name);
  return generator.generate();
}

namespace {

class SszCSharpCodeGenerator : public CodeGenerator {
 public:
  Status GenerateCode(const Parser &parser, const std::string &path,
                      const std::string &filename) override {
    if (!GenerateSszCSharp(parser, path, filename)) { return Status::ERROR; }
    return Status::OK;
  }

  Status GenerateCode(const uint8_t * /*data*/, int64_t /*length*/,
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
    return IDLOptions::kSszCSharp;
  }

  std::string LanguageName() const override { return "SszCSharp"; }
};

}  // namespace

std::unique_ptr<CodeGenerator> NewSszCSharpCodeGenerator() {
  return std::unique_ptr<SszCSharpCodeGenerator>(
      new SszCSharpCodeGenerator());
}

}  // namespace flatbuffers
