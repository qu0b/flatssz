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

#include "idl_gen_ssz_java.h"

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

namespace ssz_java {

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

static std::set<std::string> JavaKeywords() {
  return {
      "abstract",   "assert",       "boolean",    "break",      "byte",
      "case",       "catch",        "char",       "class",      "const",
      "continue",   "default",      "do",         "double",     "else",
      "enum",       "extends",      "final",      "finally",    "float",
      "for",        "goto",         "if",         "implements", "import",
      "instanceof", "int",          "interface",  "long",       "native",
      "new",        "package",      "private",    "protected",  "public",
      "return",     "short",        "static",     "strictfp",   "super",
      "switch",     "synchronized", "this",       "throw",      "throws",
      "transient",  "try",          "void",       "volatile",   "while",
  };
}

static Namer::Config SszJavaDefaultConfig() {
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
          /*namespace_seperator=*/"/",
          /*object_prefix=*/"",
          /*object_suffix=*/"",
          /*keyword_prefix=*/"",
          /*keyword_suffix=*/"_",
          /*keywords_casing=*/Namer::Config::KeywordsCasing::CaseSensitive,
          /*filenames=*/Case::kKeep,
          /*directories=*/Case::kKeep,
          /*output_path=*/"",
          /*filename_suffix=*/"",
          /*filename_extension=*/".java"};
}

// Convert a CamelCase name to lowerCamelCase
static std::string ToLowerCamel(const std::string &input) {
  if (input.empty()) return input;
  std::string result = input;
  result[0] = static_cast<char>(tolower(result[0]));
  return result;
}

// ---------------------------------------------------------------------------
// Main Generator
// ---------------------------------------------------------------------------

class SszJavaGenerator : public BaseGenerator {
 public:
  SszJavaGenerator(const Parser &parser, const std::string &path,
                   const std::string &file_name)
      : BaseGenerator(parser, path, file_name, "", "", "java"),
        namer_(WithFlagOptions(SszJavaDefaultConfig(), parser.opts, path),
               JavaKeywords()) {}

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
  // Java type helpers
  // -----------------------------------------------------------------------

  std::string JavaTypeName(const SszFieldInfo &info, const FieldDef &field) {
    switch (info.ssz_type) {
      case SszType::Bool:
        return "boolean";
      case SszType::Uint8:
        return "byte";
      case SszType::Uint16:
        return "short";
      case SszType::Uint32:
        return "int";
      case SszType::Uint64:
        return "long";
      case SszType::Uint128:
        return "byte[]";
      case SszType::Uint256:
        return "byte[]";
      case SszType::Container:
        if (info.struct_def) { return info.struct_def->name; }
        return "Object";
      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          return "byte[]";
        }
        if (info.elem_info) {
          return JavaElemTypeName(*info.elem_info) + "[]";
        }
        return "Object";
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
        if (info.elem_info && info.elem_info->ssz_type == SszType::Container) {
          if (info.elem_info->struct_def) {
            return "List<" + info.elem_info->struct_def->name + ">";
          }
          return "List<Object>";
        }
        if (info.elem_info) {
          return JavaBoxedListType(*info.elem_info);
        }
        return "byte[]";
      }
      case SszType::Bitlist:
        return "byte[]";
      case SszType::Bitvector:
        return "byte[]";
      case SszType::ProgressiveContainer:
        if (info.struct_def) { return info.struct_def->name; }
        return "Object";
      case SszType::Union:
        return "Object";
    }
    return "Object";
  }

  static std::string JavaElemTypeName(const SszFieldInfo &info) {
    switch (info.ssz_type) {
      case SszType::Bool:
        return "boolean";
      case SszType::Uint8:
        return "byte";
      case SszType::Uint16:
        return "short";
      case SszType::Uint32:
        return "int";
      case SszType::Uint64:
        return "long";
      case SszType::Container:
        if (info.struct_def) {
          return info.struct_def->name;
        }
        return "Object";
      case SszType::List:
        return "byte[]";
      default:
        return "Object";
    }
  }

  static std::string JavaBoxedListType(const SszFieldInfo &info) {
    switch (info.ssz_type) {
      case SszType::Bool:
        return "List<Boolean>";
      case SszType::Uint16:
        return "List<Short>";
      case SszType::Uint32:
        return "List<Integer>";
      case SszType::Uint64:
        return "List<Long>";
      default:
        return "List<Object>";
    }
  }

  static std::string JavaBoxedName(const SszFieldInfo &info) {
    switch (info.ssz_type) {
      case SszType::Bool:
        return "Boolean";
      case SszType::Uint16:
        return "Short";
      case SszType::Uint32:
        return "Integer";
      case SszType::Uint64:
        return "Long";
      default:
        return "Object";
    }
  }

  static std::string FieldName(const FieldDef &field) {
    return ToLowerCamel(field.name);
  }

  // -----------------------------------------------------------------------
  // Code generation entry point
  // -----------------------------------------------------------------------

  bool GenerateType(const StructDef &struct_def,
                    const SszContainerInfo &container, std::string *code) {
    GenJavaClass(struct_def, container, code);
    return true;
  }

  // -----------------------------------------------------------------------
  // Java class generation
  // -----------------------------------------------------------------------

  void GenJavaClass(const StructDef &struct_def,
                    const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    std::string type_name = struct_def.name;

    c += "public class " + type_name + " {\n";

    // Fields
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      auto &info = it->second;
      std::string jtype = JavaTypeName(info, *field);
      c += "    public " + jtype + " " + FieldName(*field) + ";\n";
    }
    c += "\n";

    // Default constructor
    c += "    public " + type_name + "() {\n";
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      auto &info = it->second;
      std::string fname = FieldName(*field);
      GenFieldDefault(info, fname, *field, &c, "        ");
    }
    c += "    }\n\n";

    // sszBytesLen
    GenSszBytesLen(container, &c);
    c += "\n";

    // marshalSSZ
    GenMarshalSSZ(&c);
    c += "\n";

    // marshalSSZTo
    GenMarshalSSZTo(container, &c);
    c += "\n";

    // fromSSZBytes
    GenFromSSZBytes(struct_def, container, &c);
    c += "\n";

    // hashTreeRoot
    GenHashTreeRoot(container, &c);

    c += "}\n";
  }

  // -----------------------------------------------------------------------
  // Default field initialization
  // -----------------------------------------------------------------------

  void GenFieldDefault(const SszFieldInfo &info, const std::string &fname,
                       const FieldDef & /*field*/, std::string *code,
                       const std::string &indent) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::Bool:
        // boolean defaults to false in Java
        break;
      case SszType::Uint8:
      case SszType::Uint16:
      case SszType::Uint32:
      case SszType::Uint64:
        // numeric primitives default to 0
        break;
      case SszType::Uint128:
        c += indent + "this." + fname + " = new byte[16];\n";
        break;
      case SszType::Uint256:
        c += indent + "this." + fname + " = new byte[32];\n";
        break;
      case SszType::Container:
      case SszType::ProgressiveContainer:
        if (info.struct_def) {
          c += indent + "this." + fname + " = new " +
               info.struct_def->name + "();\n";
        }
        break;
      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "this." + fname + " = new byte[" +
               NumToString(info.limit) + "];\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "this." + fname + " = new " +
               info.elem_info->struct_def->name + "[" +
               NumToString(info.limit) + "];\n";
          c += indent + "for (int i = 0; i < " + NumToString(info.limit) +
               "; i++) {\n";
          c += indent + "    this." + fname + "[i] = new " +
               info.elem_info->struct_def->name + "();\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          c += indent + "this." + fname + " = new " +
               JavaElemTypeName(*info.elem_info) + "[" +
               NumToString(info.limit) + "];\n";
        }
        break;
      }
      case SszType::List:
      case SszType::ProgressiveList: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "this." + fname + " = new byte[0];\n";
        } else if (info.elem_info &&
                   (info.elem_info->ssz_type == SszType::List ||
                    info.elem_info->ssz_type == SszType::Container)) {
          c += indent + "this." + fname + " = new ArrayList<>();\n";
        } else if (info.elem_info) {
          c += indent + "this." + fname + " = new ArrayList<>();\n";
        } else {
          c += indent + "this." + fname + " = new byte[0];\n";
        }
        break;
      }
      case SszType::Bitlist:
        c += indent + "this." + fname + " = new byte[0];\n";
        break;
      case SszType::Bitvector:
        c += indent + "this." + fname + " = new byte[" +
             NumToString((info.bitsize + 7) / 8) + "];\n";
        break;
      case SszType::Union:
        break;
    }
  }

  // -----------------------------------------------------------------------
  // sszBytesLen generation
  // -----------------------------------------------------------------------

  void GenSszBytesLen(const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;

    c += "    public int sszBytesLen() {\n";

    if (container.is_fixed) {
      c += "        return " + NumToString(container.static_size) + ";\n";
    } else {
      c += "        int size = " + NumToString(container.static_size) + ";\n";
      for (auto *field : container.dynamic_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "this." + FieldName(*field);
        GenSizeField(info, fname, &c, "        ");
      }
      c += "        return size;\n";
    }
    c += "    }\n";
  }

  void GenSizeField(const SszFieldInfo &info, const std::string &var,
                    std::string *code, const std::string &indent) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::ProgressiveList:
      case SszType::List: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          c += indent + "size += " + var + ".size() * 4;\n";
          c += indent + "for (byte[] item : " + var + ") {\n";
          c += indent + "    size += item.length;\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
            info.elem_info->ssz_type == SszType::Container &&
            info.elem_info->is_dynamic) {
          c += indent + "size += " + var + ".size() * 4;\n";
          c += indent + "for (" + info.elem_info->struct_def->name +
               " item : " + var + ") {\n";
          c += indent + "    size += item.sszBytesLen();\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "size += " + var + ".size() * " +
               NumToString(info.elem_info->fixed_size) + ";\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "size += " + var + ".length;\n";
        } else if (info.elem_info) {
          c += indent + "size += " + var + ".size() * " +
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
      case SszType::ProgressiveContainer:
        c += indent + "size += " + var + ".sszBytesLen();\n";
        break;
      default:
        break;
    }
  }

  // -----------------------------------------------------------------------
  // marshalSSZ generation
  // -----------------------------------------------------------------------

  void GenMarshalSSZ(std::string *code) {
    std::string &c = *code;
    c += "    public byte[] marshalSSZ() {\n";
    c += "        byte[] buf = new byte[sszBytesLen()];\n";
    c += "        marshalSSZTo(buf, 0);\n";
    c += "        return buf;\n";
    c += "    }\n";
  }

  void GenMarshalSSZTo(const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;

    c += "    public void marshalSSZTo(byte[] buf, int offset) {\n";
    c += "        ByteBuffer bb = ByteBuffer.wrap(buf);\n";
    c += "        bb.order(ByteOrder.LITTLE_ENDIAN);\n";
    c += "        int pos = offset;\n";

    if (container.dynamic_fields.empty()) {
      // All-fixed container: marshal fields sequentially
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        GenMarshalField(it->second, "this." + FieldName(*field),
                        &c, "        ");
      }
    } else {
      c += "\n";

      // Static section
      int offset_idx = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "this." + FieldName(*field);

        if (info.is_dynamic) {
          c += "        // Offset placeholder for '" + field->name + "'\n";
          c += "        int offsetPos" + NumToString(offset_idx) +
               " = pos;\n";
          c += "        pos += 4;\n";
          offset_idx++;
        } else {
          c += "        // Field '" + field->name + "'\n";
          GenMarshalField(info, fname, &c, "        ");
        }
      }

      c += "\n";

      // Dynamic section: backpatch offsets and marshal dynamic fields
      offset_idx = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "this." + FieldName(*field);

        if (info.is_dynamic) {
          c += "        // Dynamic field '" + field->name + "'\n";
          c += "        bb.putInt(offsetPos" + NumToString(offset_idx) +
               ", pos - offset);\n";
          GenMarshalField(info, fname, &c, "        ");
          offset_idx++;
        }
      }
    }

    c += "    }\n";
  }

  void GenMarshalField(const SszFieldInfo &info, const std::string &var,
                       std::string *code, const std::string &indent) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::Bool:
        c += indent + "buf[pos] = (byte) (" + var + " ? 1 : 0);\n";
        c += indent + "pos += 1;\n";
        break;

      case SszType::Uint8:
        c += indent + "buf[pos] = " + var + ";\n";
        c += indent + "pos += 1;\n";
        break;

      case SszType::Uint16:
        c += indent + "bb.putShort(pos, " + var + ");\n";
        c += indent + "pos += 2;\n";
        break;

      case SszType::Uint32:
        c += indent + "bb.putInt(pos, " + var + ");\n";
        c += indent + "pos += 4;\n";
        break;

      case SszType::Uint64:
        c += indent + "bb.putLong(pos, " + var + ");\n";
        c += indent + "pos += 8;\n";
        break;

      case SszType::Uint128:
        c += indent + "System.arraycopy(" + var + ", 0, buf, pos, 16);\n";
        c += indent + "pos += 16;\n";
        break;

      case SszType::Uint256:
        c += indent + "System.arraycopy(" + var + ", 0, buf, pos, 32);\n";
        c += indent + "pos += 32;\n";
        break;

      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "System.arraycopy(" + var + ", 0, buf, pos, " +
               NumToString(info.fixed_size) + ");\n";
          c += indent + "pos += " + NumToString(info.fixed_size) + ";\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          uint32_t elem_size = info.elem_info->fixed_size;
          c += indent + "for (int i = 0; i < " + var + ".length; i++) {\n";
          if (info.elem_info->is_dynamic) {
            c += indent + "    " + var + "[i].marshalSSZTo(buf, pos);\n";
            c += indent + "    pos += " + var +
                 "[i].sszBytesLen();\n";
          } else {
            c += indent + "    " + var + "[i].marshalSSZTo(buf, pos);\n";
            c += indent + "    pos += " + NumToString(elem_size) + ";\n";
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
          c += indent + "{\n";
          c += indent + "    int listStart = pos;\n";
          c += indent + "    pos += " + var + ".size() * 4;\n";
          c += indent + "    for (int i = 0; i < " + var +
               ".size(); i++) {\n";
          c += indent + "        bb.putInt(listStart + i * 4, "
               "pos - listStart);\n";
          c += indent + "        byte[] item = " + var + ".get(i);\n";
          c += indent + "        System.arraycopy(item, 0, buf, pos, "
               "item.length);\n";
          c += indent + "        pos += item.length;\n";
          c += indent + "    }\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "System.arraycopy(" + var +
               ", 0, buf, pos, " + var + ".length);\n";
          c += indent + "pos += " + var + ".length;\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          if (info.elem_info->is_dynamic) {
            c += indent + "{\n";
            c += indent + "    int listStart = pos;\n";
            c += indent + "    pos += " + var + ".size() * 4;\n";
            c += indent + "    for (int i = 0; i < " + var +
                 ".size(); i++) {\n";
            c += indent + "        bb.putInt(listStart + i * 4, "
                 "pos - listStart);\n";
            c += indent + "        " + var +
                 ".get(i).marshalSSZTo(buf, pos);\n";
            c += indent + "        pos += " + var +
                 ".get(i).sszBytesLen();\n";
            c += indent + "    }\n";
            c += indent + "}\n";
          } else {
            uint32_t elem_size = info.elem_info->fixed_size;
            c += indent + "for (" + info.elem_info->struct_def->name +
                 " item : " + var + ") {\n";
            c += indent + "    item.marshalSSZTo(buf, pos);\n";
            c += indent + "    pos += " + NumToString(elem_size) + ";\n";
            c += indent + "}\n";
          }
        } else if (info.elem_info) {
          GenMarshalPrimitiveList(info, var, code, indent);
        } else {
          c += indent + "System.arraycopy(" + var +
               ", 0, buf, pos, " + var + ".length);\n";
          c += indent + "pos += " + var + ".length;\n";
        }
        break;
      }

      case SszType::Bitlist:
        c += indent + "System.arraycopy(" + var +
             ", 0, buf, pos, " + var + ".length);\n";
        c += indent + "pos += " + var + ".length;\n";
        break;

      case SszType::Bitvector:
        c += indent + "System.arraycopy(" + var + ", 0, buf, pos, " +
             NumToString(info.fixed_size) + ");\n";
        c += indent + "pos += " + NumToString(info.fixed_size) + ";\n";
        break;

      case SszType::ProgressiveContainer:
      case SszType::Container:
        c += indent + var + ".marshalSSZTo(buf, pos);\n";
        c += indent + "pos += " + var + ".sszBytesLen();\n";
        break;

      case SszType::Union:
        // Union marshaling placeholder
        break;
    }
  }

  void GenMarshalPrimitiveArray(const SszFieldInfo &info,
                                const std::string &var, std::string *code,
                                const std::string &indent) {
    std::string &c = *code;
    c += indent + "for (int i = 0; i < " + var + ".length; i++) {\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "    buf[pos] = (byte) (" + var +
             "[i] ? 1 : 0);\n";
        c += indent + "    pos += 1;\n";
        break;
      case SszType::Uint16:
        c += indent + "    bb.putShort(pos, " + var + "[i]);\n";
        c += indent + "    pos += 2;\n";
        break;
      case SszType::Uint32:
        c += indent + "    bb.putInt(pos, " + var + "[i]);\n";
        c += indent + "    pos += 4;\n";
        break;
      case SszType::Uint64:
        c += indent + "    bb.putLong(pos, " + var + "[i]);\n";
        c += indent + "    pos += 8;\n";
        break;
      default:
        break;
    }
    c += indent + "}\n";
  }

  void GenMarshalPrimitiveList(const SszFieldInfo &info,
                               const std::string &var, std::string *code,
                               const std::string &indent) {
    std::string &c = *code;
    std::string boxed = JavaBoxedName(*info.elem_info);
    c += indent + "for (" + boxed + " v : " + var + ") {\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "    buf[pos] = (byte) (v ? 1 : 0);\n";
        c += indent + "    pos += 1;\n";
        break;
      case SszType::Uint16:
        c += indent + "    bb.putShort(pos, v);\n";
        c += indent + "    pos += 2;\n";
        break;
      case SszType::Uint32:
        c += indent + "    bb.putInt(pos, v);\n";
        c += indent + "    pos += 4;\n";
        break;
      case SszType::Uint64:
        c += indent + "    bb.putLong(pos, v);\n";
        c += indent + "    pos += 8;\n";
        break;
      default:
        break;
    }
    c += indent + "}\n";
  }

  // -----------------------------------------------------------------------
  // fromSSZBytes generation
  // -----------------------------------------------------------------------

  void GenFromSSZBytes(const StructDef &struct_def,
                       const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    std::string type_name = struct_def.name;

    c += "    public static " + type_name +
         " fromSSZBytes(byte[] bytes) {\n";

    // Validate minimum size
    c += "        if (bytes.length < " +
         NumToString(container.static_size) + ") {\n";
    c += "            throw new SszError(\"buffer too small\");\n";
    c += "        }\n\n";

    c += "        " + type_name + " t = new " + type_name + "();\n";
    c += "        ByteBuffer bb = ByteBuffer.wrap(bytes);\n";
    c += "        bb.order(ByteOrder.LITTLE_ENDIAN);\n\n";

    if (container.dynamic_fields.empty()) {
      // All-fixed container
      uint32_t offset = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "t." + FieldName(*field);
        GenUnmarshalField(info, fname, offset, &c, "        ");
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
        std::string fname = "t." + FieldName(*field);

        if (info.is_dynamic) {
          c += "        int offset" + NumToString(dyn_idx) +
               " = bb.getInt(" + NumToString(offset) + ");\n";
          dyn_offset_names.push_back({field, dyn_idx});

          if (dyn_idx == 0) {
            c += "        if (offset0 != " +
                 NumToString(container.static_size) + ") {\n";
            c += "            throw new SszError(\"invalid offset\");\n";
            c += "        }\n";
          } else {
            c += "        if (offset" + NumToString(dyn_idx) +
                 " < offset" + NumToString(dyn_idx - 1) +
                 " || offset" + NumToString(dyn_idx) +
                 " > bytes.length) {\n";
            c += "            throw new SszError(\"invalid offset\");\n";
            c += "        }\n";
          }
          offset += 4;
          dyn_idx++;
        } else {
          GenUnmarshalField(info, fname, offset, &c, "        ");
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
        std::string fname = "t." + FieldName(*field);

        std::string start_off = "offset" + NumToString(oidx);
        std::string end_off;
        if (i + 1 < dyn_offset_names.size()) {
          end_off = "offset" + NumToString(dyn_offset_names[i + 1].second);
        } else {
          end_off = "bytes.length";
        }

        c += "        {\n";
        c += "            int sliceStart = " + start_off + ";\n";
        c += "            int sliceEnd = " + end_off + ";\n";
        c += "            byte[] slice = new byte[sliceEnd - sliceStart];\n";
        c += "            System.arraycopy(bytes, sliceStart, slice, 0, "
             "sliceEnd - sliceStart);\n";
        GenUnmarshalDynField(info, fname, *field, &c, "            ");
        c += "        }\n";
      }
    }

    c += "        return t;\n";
    c += "    }\n";
  }

  void GenUnmarshalField(const SszFieldInfo &info, const std::string &var,
                         uint32_t offset, std::string *code,
                         const std::string &indent) {
    std::string &c = *code;
    std::string off_str = NumToString(offset);

    switch (info.ssz_type) {
      case SszType::Bool:
        c += indent + "if (bytes[" + off_str + "] > 1) {\n";
        c += indent + "    throw new SszError(\"invalid bool\");\n";
        c += indent + "}\n";
        c += indent + var + " = bytes[" + off_str + "] == 1;\n";
        break;

      case SszType::Uint8:
        c += indent + var + " = bytes[" + off_str + "];\n";
        break;

      case SszType::Uint16:
        c += indent + var + " = bb.getShort(" + off_str + ");\n";
        break;

      case SszType::Uint32:
        c += indent + var + " = bb.getInt(" + off_str + ");\n";
        break;

      case SszType::Uint64:
        c += indent + var + " = bb.getLong(" + off_str + ");\n";
        break;

      case SszType::Uint128:
        c += indent + var + " = new byte[16];\n";
        c += indent + "System.arraycopy(bytes, " + off_str + ", " +
             var + ", 0, 16);\n";
        break;

      case SszType::Uint256:
        c += indent + var + " = new byte[32];\n";
        c += indent + "System.arraycopy(bytes, " + off_str + ", " +
             var + ", 0, 32);\n";
        break;

      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + var + " = new byte[" +
               NumToString(info.fixed_size) + "];\n";
          c += indent + "System.arraycopy(bytes, " + off_str + ", " +
               var + ", 0, " + NumToString(info.fixed_size) + ");\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          uint32_t elem_size = info.elem_info->fixed_size;
          c += indent + "for (int i = 0; i < " + var + ".length; i++) {\n";
          c += indent + "    int eStart = " + off_str + " + i * " +
               NumToString(elem_size) + ";\n";
          c += indent + "    byte[] elemBytes = new byte[" +
               NumToString(elem_size) + "];\n";
          c += indent + "    System.arraycopy(bytes, eStart, elemBytes,"
               " 0, " + NumToString(elem_size) + ");\n";
          c += indent + "    " + var + "[i] = " +
               info.elem_info->struct_def->name +
               ".fromSSZBytes(elemBytes);\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          GenUnmarshalPrimitiveArray(info, var, offset, code, indent);
        }
        break;
      }

      case SszType::Bitvector:
        c += indent + var + " = new byte[" +
             NumToString(info.fixed_size) + "];\n";
        c += indent + "System.arraycopy(bytes, " + off_str + ", " +
             var + ", 0, " + NumToString(info.fixed_size) + ");\n";
        break;

      case SszType::Container: {
        c += indent + "{\n";
        c += indent + "    byte[] nested = new byte[" +
             NumToString(info.fixed_size) + "];\n";
        c += indent + "    System.arraycopy(bytes, " + off_str +
             ", nested, 0, " + NumToString(info.fixed_size) + ");\n";
        c += indent + "    " + var + " = " + info.struct_def->name +
             ".fromSSZBytes(nested);\n";
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
    c += indent + "for (int i = 0; i < " + var + ".length; i++) {\n";
    c += indent + "    int off = " + NumToString(offset) + " + i * " +
         NumToString(elem_size) + ";\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "    if (bytes[off] > 1) {\n";
        c += indent + "        throw new SszError(\"invalid bool\");\n";
        c += indent + "    }\n";
        c += indent + "    " + var + "[i] = bytes[off] == 1;\n";
        break;
      case SszType::Uint16:
        c += indent + "    " + var + "[i] = bb.getShort(off);\n";
        break;
      case SszType::Uint32:
        c += indent + "    " + var + "[i] = bb.getInt(off);\n";
        break;
      case SszType::Uint64:
        c += indent + "    " + var + "[i] = bb.getLong(off);\n";
        break;
      default:
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
          // List of byte lists
          c += indent + "if (slice.length > 0) {\n";
          c += indent + "    ByteBuffer sliceBB = "
               "ByteBuffer.wrap(slice);\n";
          c += indent + "    sliceBB.order(ByteOrder.LITTLE_ENDIAN);\n";
          c += indent + "    int firstOff = sliceBB.getInt(0);\n";
          c += indent + "    if (firstOff % 4 != 0) {\n";
          c += indent + "        throw new SszError("
               "\"invalid offset\");\n";
          c += indent + "    }\n";
          c += indent + "    int count = firstOff / 4;\n";
          c += indent + "    " + var + " = new ArrayList<>(count);\n";
          c += indent + "    for (int i = 0; i < count; i++) {\n";
          c += indent + "        int s = sliceBB.getInt(i * 4);\n";
          c += indent + "        int e = (i + 1 < count) ? "
               "sliceBB.getInt((i + 1) * 4) : slice.length;\n";
          c += indent + "        if (s > e || e > slice.length) {\n";
          c += indent + "            throw new SszError("
               "\"invalid offset\");\n";
          c += indent + "        }\n";
          c += indent + "        byte[] elem = new byte[e - s];\n";
          c += indent + "        System.arraycopy(slice, s, elem, "
               "0, e - s);\n";
          c += indent + "        " + var + ".add(elem);\n";
          c += indent + "    }\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + var + " = new byte[slice.length];\n";
          c += indent + "System.arraycopy(slice, 0, " + var +
               ", 0, slice.length);\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          if (info.elem_info->is_dynamic) {
            c += indent + "if (slice.length > 0) {\n";
            c += indent + "    ByteBuffer sliceBB = "
                 "ByteBuffer.wrap(slice);\n";
            c += indent + "    sliceBB.order(ByteOrder.LITTLE_ENDIAN);\n";
            c += indent + "    int firstOff = sliceBB.getInt(0);\n";
            c += indent + "    if (firstOff % 4 != 0) {\n";
            c += indent + "        throw new SszError("
                 "\"invalid offset\");\n";
            c += indent + "    }\n";
            c += indent + "    int count = firstOff / 4;\n";
            c += indent + "    " + var + " = new ArrayList<>(count);\n";
            c += indent + "    for (int i = 0; i < count; i++) {\n";
            c += indent + "        int s = sliceBB.getInt(i * 4);\n";
            c += indent + "        int e = (i + 1 < count) ? "
                 "sliceBB.getInt((i + 1) * 4) : slice.length;\n";
            c += indent + "        if (s > e || e > slice.length) {\n";
            c += indent + "            throw new SszError("
                 "\"invalid offset\");\n";
            c += indent + "        }\n";
            c += indent + "        byte[] elemBytes = "
                 "new byte[e - s];\n";
            c += indent + "        System.arraycopy(slice, s, "
                 "elemBytes, 0, e - s);\n";
            c += indent + "        " + var + ".add(" +
                 info.elem_info->struct_def->name +
                 ".fromSSZBytes(elemBytes));\n";
            c += indent + "    }\n";
            c += indent + "}\n";
          } else {
            uint32_t elem_size = info.elem_info->fixed_size;
            c += indent + "int count = slice.length / " +
                 NumToString(elem_size) + ";\n";
            c += indent + var + " = new ArrayList<>(count);\n";
            c += indent + "for (int i = 0; i < count; i++) {\n";
            c += indent + "    int off = i * " +
                 NumToString(elem_size) + ";\n";
            c += indent + "    byte[] elemBytes = new byte[" +
                 NumToString(elem_size) + "];\n";
            c += indent + "    System.arraycopy(slice, off, "
                 "elemBytes, 0, " + NumToString(elem_size) + ");\n";
            c += indent + "    " + var + ".add(" +
                 info.elem_info->struct_def->name +
                 ".fromSSZBytes(elemBytes));\n";
            c += indent + "}\n";
          }
        } else if (info.elem_info) {
          GenUnmarshalPrimitiveList(info, var, code, indent);
        } else {
          c += indent + var + " = new byte[slice.length];\n";
          c += indent + "System.arraycopy(slice, 0, " + var +
               ", 0, slice.length);\n";
        }
        break;
      }
      case SszType::Bitlist:
        c += indent + var + " = new byte[slice.length];\n";
        c += indent + "System.arraycopy(slice, 0, " + var +
             ", 0, slice.length);\n";
        break;
      case SszType::ProgressiveContainer:
      case SszType::Container:
        c += indent + var + " = " + info.struct_def->name +
             ".fromSSZBytes(slice);\n";
        break;
      default:
        break;
    }
  }

  void GenUnmarshalPrimitiveList(const SszFieldInfo &info,
                                 const std::string &var, std::string *code,
                                 const std::string &indent) {
    std::string &c = *code;
    uint32_t elem_size = info.elem_info->fixed_size;
    c += indent + "int count = slice.length / " + NumToString(elem_size) +
         ";\n";
    c += indent + var + " = new ArrayList<>(count);\n";
    c += indent + "ByteBuffer sliceBB = ByteBuffer.wrap(slice);\n";
    c += indent + "sliceBB.order(ByteOrder.LITTLE_ENDIAN);\n";
    c += indent + "for (int i = 0; i < count; i++) {\n";
    c += indent + "    int off = i * " + NumToString(elem_size) + ";\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += indent + "    if (slice[off] > 1) {\n";
        c += indent + "        throw new SszError(\"invalid bool\");\n";
        c += indent + "    }\n";
        c += indent + "    " + var + ".add(slice[off] == 1);\n";
        break;
      case SszType::Uint16:
        c += indent + "    " + var + ".add(sliceBB.getShort(off));\n";
        break;
      case SszType::Uint32:
        c += indent + "    " + var + ".add(sliceBB.getInt(off));\n";
        break;
      case SszType::Uint64:
        c += indent + "    " + var + ".add(sliceBB.getLong(off));\n";
        break;
      default:
        break;
    }
    c += indent + "}\n";
  }

  // -----------------------------------------------------------------------
  // hashTreeRoot generation
  // -----------------------------------------------------------------------

  void GenHashTreeRoot(const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;

    c += "    public byte[] hashTreeRoot() {\n";
    c += "        Hasher hh = new Hasher();\n";
    c += "        int idx = hh.index();\n\n";

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
          c += "        // Index " + NumToString(i) + ": " +
               field->name + "\n";
          GenHashField(iit->second, "this." + FieldName(*field),
                       &c, "        ");
        } else {
          c += "        // Index " + NumToString(i) +
               ": gap (inactive)\n";
          c += "        hh.putUint8((byte) 0);\n";
        }
      }
      c += "\n";

      c += "        hh.merkleizeProgressiveWithActiveFields(idx, "
           "new byte[]{";
      for (size_t i = 0; i < container.active_fields_bitvector.size(); i++) {
        if (i > 0) c += ", ";
        char hex[16];
        snprintf(hex, sizeof(hex), "(byte) 0x%02x",
                 container.active_fields_bitvector[i]);
        c += hex;
      }
      c += "});\n";
    } else {
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "this." + FieldName(*field);

        c += "        // Field '" + field->name + "'\n";
        GenHashField(info, fname, &c, "        ");
        c += "\n";
      }

      c += "        hh.merkleize(idx);\n";
    }

    c += "        return hh.hashRoot();\n";
    c += "    }\n";
  }

  void GenHashField(const SszFieldInfo &info, const std::string &var,
                    std::string *code, const std::string &indent) {
    std::string &c = *code;

    switch (info.ssz_type) {
      case SszType::Bool:
        c += indent + "hh.putBool(" + var + ");\n";
        break;

      case SszType::Uint8:
        c += indent + "hh.putUint8(" + var + ");\n";
        break;

      case SszType::Uint16:
        c += indent + "hh.putUint16(" + var + ");\n";
        break;

      case SszType::Uint32:
        c += indent + "hh.putUint32(" + var + ");\n";
        break;

      case SszType::Uint64:
        c += indent + "hh.putUint64(" + var + ");\n";
        break;

      case SszType::Uint128:
      case SszType::Uint256:
        c += indent + "hh.putBytes(" + var + ");\n";
        break;

      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "hh.putBytes(" + var + ");\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "{\n";
          c += indent + "    int subIdx = hh.index();\n";
          c += indent + "    for (int i = 0; i < " + var +
               ".length; i++) {\n";
          c += indent + "        " + var +
               "[i].hashTreeRootWith(hh);\n";
          c += indent + "    }\n";
          c += indent + "    hh.merkleize(subIdx);\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          c += indent + "{\n";
          c += indent + "    int subIdx = hh.index();\n";
          GenHashPrimitiveArray(info, var, code, indent + "    ");
          c += indent + "    hh.fillUpTo32();\n";
          c += indent + "    hh.merkleize(subIdx);\n";
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
          c += indent + "    int subIdx = hh.index();\n";
          c += indent + "    for (byte[] item : " + var + ") {\n";
          c += indent + "        int inner = hh.index();\n";
          c += indent + "        hh.appendBytes32(item);\n";
          if (inner_chunk_limit > 0) {
            c += indent + "        hh.merkleizeWithMixin(inner, "
                 "item.length, " + NumToString(inner_chunk_limit) +
                 ");\n";
          } else {
            c += indent + "        hh.merkleize(inner);\n";
          }
          c += indent + "    }\n";
          c += indent + "    hh.merkleizeWithMixin(subIdx, " + var +
               ".size(), " + NumToString(limit) + ");\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "{\n";
          c += indent + "    int subIdx = hh.index();\n";
          c += indent + "    hh.appendBytes32(" + var + ");\n";
          c += indent + "    hh.merkleizeWithMixin(subIdx, " + var +
               ".length, " + NumToString(ssz_limit_for_bytes(limit)) +
               ");\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "{\n";
          c += indent + "    int subIdx = hh.index();\n";
          c += indent + "    for (" +
               info.elem_info->struct_def->name + " item : " + var +
               ") {\n";
          c += indent + "        item.hashTreeRootWith(hh);\n";
          c += indent + "    }\n";
          c += indent + "    hh.merkleizeWithMixin(subIdx, " + var +
               ".size(), " + NumToString(limit) + ");\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          c += indent + "{\n";
          c += indent + "    int subIdx = hh.index();\n";
          GenHashPrimitiveList(info, var, code, indent + "    ");
          c += indent + "    hh.fillUpTo32();\n";
          uint64_t hash_limit =
              ssz_limit_for_elements(limit, info.elem_info->fixed_size);
          c += indent + "    hh.merkleizeWithMixin(subIdx, " + var +
               ".size(), " + NumToString(hash_limit) + ");\n";
          c += indent + "}\n";
        }
        break;
      }

      case SszType::Bitlist:
        c += indent + "hh.putBitlist(" + var + ", " +
             NumToString(info.limit) + ");\n";
        break;

      case SszType::Bitvector:
        c += indent + "hh.putBytes(" + var + ");\n";
        break;

      case SszType::Container:
        c += indent + var + ".hashTreeRootWith(hh);\n";
        break;

      case SszType::ProgressiveContainer:
        c += indent + var + ".hashTreeRootWith(hh);\n";
        break;

      case SszType::ProgressiveList: {
        if (info.elem_info &&
            info.elem_info->ssz_type == SszType::Uint8) {
          c += indent + "{\n";
          c += indent + "    int subIdx = hh.index();\n";
          c += indent + "    hh.appendBytes32(" + var + ");\n";
          c += indent + "    hh.merkleizeProgressiveWithMixin(subIdx, " +
               var + ".length);\n";
          c += indent + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += indent + "{\n";
          c += indent + "    int subIdx = hh.index();\n";
          c += indent + "    for (" +
               info.elem_info->struct_def->name + " item : " + var +
               ") {\n";
          c += indent + "        item.hashTreeRootWith(hh);\n";
          c += indent + "    }\n";
          c += indent + "    hh.merkleizeProgressiveWithMixin(subIdx, " +
               var + ".size());\n";
          c += indent + "}\n";
        } else if (info.elem_info) {
          c += indent + "{\n";
          c += indent + "    int subIdx = hh.index();\n";
          GenHashPrimitiveList(info, var, code, indent + "    ");
          c += indent + "    hh.fillUpTo32();\n";
          c += indent + "    hh.merkleizeProgressiveWithMixin(subIdx, " +
               var + ".size());\n";
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
    c += indent + "for (int i = 0; i < " + var + ".length; i++) {\n";
    GenHashPrimitive(info.elem_info->ssz_type, var + "[i]", code,
                     indent + "    ");
    c += indent + "}\n";
  }

  void GenHashPrimitiveList(const SszFieldInfo &info, const std::string &var,
                            std::string *code, const std::string &indent) {
    std::string &c = *code;
    std::string boxed = JavaBoxedName(*info.elem_info);
    c += indent + "for (" + boxed + " v : " + var + ") {\n";
    GenHashPrimitive(info.elem_info->ssz_type, "v", code, indent + "    ");
    c += indent + "}\n";
  }

  void GenHashPrimitive(SszType ssz_type, const std::string &var,
                        std::string *code, const std::string &indent) {
    std::string &c = *code;
    switch (ssz_type) {
      case SszType::Bool:
        c += indent + "hh.appendBool(" + var + ");\n";
        break;
      case SszType::Uint8:
        c += indent + "hh.appendUint8(" + var + ");\n";
        break;
      case SszType::Uint16:
        c += indent + "hh.appendUint16(" + var + ");\n";
        break;
      case SszType::Uint32:
        c += indent + "hh.appendUint32(" + var + ");\n";
        break;
      case SszType::Uint64:
        c += indent + "hh.appendUint64(" + var + ");\n";
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
    std::string pkg_name = FullNamespace(".", ns);
    if (pkg_name.empty()) {
      pkg_name = def.name;
      std::transform(pkg_name.begin(), pkg_name.end(), pkg_name.begin(),
                     [](unsigned char ch) {
                       return static_cast<char>(tolower(ch));
                     });
    }

    std::string code;
    code += "// Code generated by the FlatBuffers compiler. DO NOT EDIT.\n\n";
    code += "package " + pkg_name + ";\n\n";

    code += "import java.nio.ByteBuffer;\n";
    code += "import java.nio.ByteOrder;\n";
    code += "import java.util.ArrayList;\n";
    code += "import java.util.List;\n";
    code += "import ssz.Hasher;\n";
    code += "import ssz.SszError;\n";
    code += "\n";

    code += classcode;

    // Strip trailing double newlines
    while (code.length() > 2 && code.substr(code.length() - 2) == "\n\n") {
      code.pop_back();
    }

    std::string directory = namer_.Directories(ns);
    EnsureDirExists(directory);
    std::string filename = directory + def.name + ".java";
    return parser_.opts.file_saver->SaveFile(filename.c_str(), code, false);
  }
};

}  // namespace ssz_java

// ---------------------------------------------------------------------------
// Code generator wrapper
// ---------------------------------------------------------------------------

static bool GenerateSszJava(const Parser &parser, const std::string &path,
                            const std::string &file_name) {
  ssz_java::SszJavaGenerator generator(parser, path, file_name);
  return generator.generate();
}

namespace {

class SszJavaCodeGenerator : public CodeGenerator {
 public:
  Status GenerateCode(const Parser &parser, const std::string &path,
                      const std::string &filename) override {
    if (!GenerateSszJava(parser, path, filename)) { return Status::ERROR; }
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
    return IDLOptions::kSszJava;
  }

  std::string LanguageName() const override { return "SszJava"; }
};

}  // namespace

std::unique_ptr<CodeGenerator> NewSszJavaCodeGenerator() {
  return std::unique_ptr<SszJavaCodeGenerator>(new SszJavaCodeGenerator());
}

}  // namespace flatbuffers
