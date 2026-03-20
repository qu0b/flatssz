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

#include "idl_gen_ssz_rust.h"

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

namespace ssz_rust {

// ---------------------------------------------------------------------------
// SSZ Type Resolution (shared logic with Go backend)
// ---------------------------------------------------------------------------

enum class SszType {
  Bool, Uint8, Uint16, Uint32, Uint64, Uint128, Uint256,
  Container, Vector, List, Bitlist, Bitvector, Union,
  ProgressiveContainer, ProgressiveList,
};

struct SszFieldInfo {
  SszType ssz_type;
  uint32_t fixed_size;
  uint64_t limit;
  uint32_t bitsize;
  bool is_dynamic;
  const StructDef *struct_def;
  std::unique_ptr<SszFieldInfo> elem_info;

  SszFieldInfo()
      : ssz_type(SszType::Uint8), fixed_size(0), limit(0), bitsize(0),
        is_dynamic(false), struct_def(nullptr) {}
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

static std::set<std::string> RustKeywords() {
  return {
      "as",       "break",    "const",    "continue", "crate",  "else",
      "enum",     "extern",   "false",    "fn",       "for",    "if",
      "impl",     "in",       "let",      "loop",     "match",  "mod",
      "move",     "mut",      "pub",      "ref",      "return", "Self",
      "self",     "static",   "struct",   "super",    "trait",  "true",
      "type",     "unsafe",   "use",      "where",    "while",  "async",
      "await",    "dyn",
  };
}

static Namer::Config SszRustDefaultConfig() {
  return {/*types=*/Case::kUpperCamel,
          /*constants=*/Case::kScreamingSnake,
          /*methods=*/Case::kSnake,
          /*functions=*/Case::kSnake,
          /*fields=*/Case::kSnake,
          /*variables=*/Case::kSnake,
          /*variants=*/Case::kUpperCamel,
          /*enum_variant_seperator=*/"::",
          /*escape_keywords=*/Namer::Config::Escape::AfterConvertingCase,
          /*namespaces=*/Case::kSnake,
          /*namespace_seperator=*/"::",
          /*object_prefix=*/"",
          /*object_suffix=*/"",
          /*keyword_prefix=*/"",
          /*keyword_suffix=*/"_",
          /*keywords_casing=*/Namer::Config::KeywordsCasing::CaseSensitive,
          /*filenames=*/Case::kSnake,
          /*directories=*/Case::kSnake,
          /*output_path=*/"",
          /*filename_suffix=*/"_ssz",
          /*filename_extension=*/".rs"};
}

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

class SszRustGenerator : public BaseGenerator {
 public:
  SszRustGenerator(const Parser &parser, const std::string &path,
                   const std::string &file_name)
      : BaseGenerator(parser, path, file_name, "", "", "rs"),
        namer_(WithFlagOptions(SszRustDefaultConfig(), parser.opts, path),
               RustKeywords()) {}

  bool generate() override {
    std::string all_code;

    // File header
    all_code += "// Code generated by the FlatBuffers compiler. DO NOT EDIT.\n";
    all_code += "#![allow(unused_imports, dead_code, clippy::all)]\n\n";
    all_code += "use ssz_flatbuffers::{SszError, Hasher, HasherPool};\n\n";

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
    std::string filename = path_ + ToSnakeCase(file_name_) + "_ssz.rs";
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
      case BASE_TYPE_UCHAR: case BASE_TYPE_CHAR:
        info.ssz_type = SszType::Uint8;
        info.fixed_size = 1;
        info.is_dynamic = false;
        break;
      case BASE_TYPE_USHORT: case BASE_TYPE_SHORT:
        info.ssz_type = SszType::Uint16;
        info.fixed_size = 2;
        info.is_dynamic = false;
        break;
      case BASE_TYPE_UINT: case BASE_TYPE_INT:
        info.ssz_type = SszType::Uint32;
        info.fixed_size = 4;
        info.is_dynamic = false;
        break;
      case BASE_TYPE_ULONG: case BASE_TYPE_LONG:
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
      case BASE_TYPE_FLOAT: case BASE_TYPE_DOUBLE:
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
    auto max_attr = attrs.Lookup("ssz_max");
    if (!max_attr) {
      flatbuffers::LogCompilerError(
          "field '" + field.name + "': vector requires ssz_max");
      return false;
    }
    std::string max_str = max_attr->constant;
    uint64_t outer_max = 0, inner_max = 0;
    auto comma = max_str.find(',');
    if (comma != std::string::npos) {
      outer_max = StringToUInt(max_str.substr(0, comma).c_str());
      inner_max = StringToUInt(max_str.substr(comma + 1).c_str());
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
    if (attrs.Lookup("ssz_bitvector")) {
      info.ssz_type = SszType::Bitvector;
      auto bitsize_attr = attrs.Lookup("ssz_bitsize");
      info.bitsize = bitsize_attr
                         ? static_cast<uint32_t>(
                               StringToUInt(bitsize_attr->constant.c_str()))
                         : array_len * 8;
      info.fixed_size = (info.bitsize + 7) / 8;
      info.is_dynamic = false;
      return true;
    }
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
        info.ssz_type = SszType::Bool; info.fixed_size = 1; break;
      case BASE_TYPE_UCHAR: case BASE_TYPE_CHAR:
        info.ssz_type = SszType::Uint8; info.fixed_size = 1; break;
      case BASE_TYPE_USHORT: case BASE_TYPE_SHORT:
        info.ssz_type = SszType::Uint16; info.fixed_size = 2; break;
      case BASE_TYPE_UINT: case BASE_TYPE_INT:
        info.ssz_type = SszType::Uint32; info.fixed_size = 4; break;
      case BASE_TYPE_ULONG: case BASE_TYPE_LONG:
        info.ssz_type = SszType::Uint64; info.fixed_size = 8; break;
      case BASE_TYPE_STRING:
        info.ssz_type = SszType::List;
        info.fixed_size = 0;
        info.is_dynamic = true;
        info.elem_info.reset(new SszFieldInfo());
        info.elem_info->ssz_type = SszType::Uint8;
        info.elem_info->fixed_size = 1;
        break;
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
            "field '" + field.name + "': unsupported element type");
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
        container.static_size += 4;
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
  // Rust type helpers
  // -----------------------------------------------------------------------

  std::string RustTypeName(const SszFieldInfo &info,
                           const FieldDef & /*field*/) {
    switch (info.ssz_type) {
      case SszType::Bool: return "bool";
      case SszType::Uint8: return "u8";
      case SszType::Uint16: return "u16";
      case SszType::Uint32: return "u32";
      case SszType::Uint64: return "u64";
      case SszType::Uint128: return "[u8; 16]";
      case SszType::Uint256: return "[u8; 32]";
      case SszType::Container:
        return info.struct_def ? info.struct_def->name : "()";
      case SszType::Vector: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8)
          return "[u8; " + NumToString(info.limit) + "]";
        if (info.elem_info)
          return "[" + RustElemTypeName(*info.elem_info) + "; " +
                 NumToString(info.limit) + "]";
        return "()";
      }
      case SszType::ProgressiveList:
      case SszType::List: {
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8)
          return "Vec<u8>";
        if (info.elem_info && info.elem_info->ssz_type == SszType::List)
          return "Vec<Vec<u8>>";
        if (info.elem_info)
          return "Vec<" + RustElemTypeName(*info.elem_info) + ">";
        return "Vec<u8>";
      }
      case SszType::Bitlist: return "Vec<u8>";
      case SszType::Bitvector:
        return "[u8; " + NumToString((info.bitsize + 7) / 8) + "]";
      case SszType::ProgressiveContainer:
        return info.struct_def ? info.struct_def->name : "()";
      case SszType::Union: return "()";
    }
    return "()";
  }

  std::string RustElemTypeName(const SszFieldInfo &info) {
    switch (info.ssz_type) {
      case SszType::Bool: return "bool";
      case SszType::Uint8: return "u8";
      case SszType::Uint16: return "u16";
      case SszType::Uint32: return "u32";
      case SszType::Uint64: return "u64";
      case SszType::Container:
        return info.struct_def ? info.struct_def->name : "()";
      case SszType::ProgressiveList:
      case SszType::List: return "Vec<u8>";
      default: return "()";
    }
  }

  std::string RustDefault(const SszFieldInfo &info) {
    switch (info.ssz_type) {
      case SszType::Bool: return "false";
      case SszType::Uint8: case SszType::Uint16:
      case SszType::Uint32: case SszType::Uint64: return "0";
      case SszType::Uint128: return "[0u8; 16]";
      case SszType::Uint256: return "[0u8; 32]";
      case SszType::Container:
        return info.struct_def ? info.struct_def->name + "::default()" : "()";
      case SszType::Vector:
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8)
          return "[0u8; " + NumToString(info.limit) + "]";
        return "Default::default()";
      case SszType::ProgressiveList:
      case SszType::List: case SszType::Bitlist:
        return "Vec::new()";
      case SszType::Bitvector:
        return "[0u8; " + NumToString((info.bitsize + 7) / 8) + "]";
      case SszType::ProgressiveContainer:
        return info.struct_def ? info.struct_def->name + "::default()" : "()";
      default: return "Default::default()";
    }
  }

  // -----------------------------------------------------------------------
  // Code generation entry
  // -----------------------------------------------------------------------

  bool GenerateType(const StructDef &struct_def,
                    const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    // Owned type
    GenRustStruct(struct_def, container, &c);
    c += "\n";
    GenImpl(struct_def, container, &c);
    c += "\n";
    // Zero-copy view type
    GenViewStruct(struct_def, container, &c);
    c += "\n";
    GenViewImpl(struct_def, container, &c);
    return true;
  }

  // -----------------------------------------------------------------------
  // Struct definition
  // -----------------------------------------------------------------------

  void GenRustStruct(const StructDef &struct_def,
                     const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    c += "#[derive(Clone, Debug, PartialEq)]\n";
    c += "pub struct " + struct_def.name + " {\n";
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      c += "    pub " + ToSnakeCase(field->name) + ": " +
           RustTypeName(it->second, *field) + ",\n";
    }
    c += "}\n\n";

    // Default impl
    c += "impl Default for " + struct_def.name + " {\n";
    c += "    fn default() -> Self {\n";
    c += "        Self {\n";
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      c += "            " + ToSnakeCase(field->name) + ": " +
           RustDefault(it->second) + ",\n";
    }
    c += "        }\n";
    c += "    }\n";
    c += "}\n";
  }

  // -----------------------------------------------------------------------
  // impl block with all SSZ methods
  // -----------------------------------------------------------------------

  void GenImpl(const StructDef &struct_def, const SszContainerInfo &container,
               std::string *code) {
    std::string &c = *code;
    std::string name = struct_def.name;

    c += "impl " + name + " {\n";

    // --- ssz_bytes_len ---
    GenSizeMethod(container, &c);

    // --- ssz_append ---
    GenMarshalMethod(container, &c);

    // --- as_ssz_bytes ---
    c += "    pub fn as_ssz_bytes(&self) -> Vec<u8> {\n";
    c += "        let mut buf = Vec::with_capacity(self.ssz_bytes_len());\n";
    c += "        self.ssz_append(&mut buf);\n";
    c += "        buf\n";
    c += "    }\n\n";

    // --- from_ssz_bytes ---
    GenUnmarshalMethod(name, container, &c);

    // --- tree_hash_root ---
    c += "    pub fn tree_hash_root(&self) -> [u8; 32] {\n";
    c += "        let mut h = HasherPool::get();\n";
    c += "        self.tree_hash_with(&mut h);\n";
    c += "        let root = h.finish();\n";
    c += "        HasherPool::put(h);\n";
    c += "        root\n";
    c += "    }\n\n";

    // --- tree_hash_with ---
    GenHashMethod(container, &c);

    c += "}\n";
  }

  // -----------------------------------------------------------------------
  // SizeSSZ
  // -----------------------------------------------------------------------

  void GenSizeMethod(const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    c += "    pub fn ssz_bytes_len(&self) -> usize {\n";
    if (container.is_fixed) {
      c += "        " + NumToString(container.static_size) + "\n";
    } else {
      c += "        let mut size = " +
           NumToString(container.static_size) + "usize;\n";
      for (auto *field : container.dynamic_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "self." + ToSnakeCase(field->name);
        GenSizeField(info, fname, &c);
      }
      c += "        size\n";
    }
    c += "    }\n\n";
  }

  void GenSizeField(const SszFieldInfo &info, const std::string &var,
                    std::string *code) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::ProgressiveList:
      case SszType::List:
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          c += "        size += " + var + ".len() * 4;\n";
          c += "        for item in &" + var + " { size += item.len(); }\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container &&
                   info.elem_info->is_dynamic) {
          c += "        size += " + var + ".len() * 4;\n";
          c += "        for item in &" + var +
               " { size += item.ssz_bytes_len(); }\n";
        } else if (info.elem_info) {
          c += "        size += " + var + ".len() * " +
               NumToString(info.elem_info->fixed_size) + ";\n";
        } else {
          c += "        size += " + var + ".len();\n";
        }
        break;
      case SszType::Bitlist:
        c += "        size += " + var + ".len();\n";
        break;
      case SszType::ProgressiveContainer:
      case SszType::Container:
        c += "        size += " + var + ".ssz_bytes_len();\n";
        break;
      default: break;
    }
  }

  // -----------------------------------------------------------------------
  // Marshal
  // -----------------------------------------------------------------------

  void GenMarshalMethod(const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    c += "    pub fn ssz_append(&self, buf: &mut Vec<u8>) {\n";

    if (container.dynamic_fields.empty()) {
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        GenMarshalField(it->second, "self." + ToSnakeCase(field->name), &c,
                        "        ");
      }
    } else {
      c += "        let dstlen = buf.len();\n";
      int oidx = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "self." + ToSnakeCase(field->name);
        if (info.is_dynamic) {
          c += "        let offset" + NumToString(oidx) + " = buf.len();\n";
          c += "        buf.extend_from_slice(&[0u8; 4]);\n";
          oidx++;
        } else {
          GenMarshalField(info, fname, &c, "        ");
        }
      }
      oidx = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = "self." + ToSnakeCase(field->name);
        if (info.is_dynamic) {
          c += "        let off = ((buf.len() - dstlen) as u32).to_le_bytes();\n";
          c += "        buf[offset" + NumToString(oidx) +
               "..offset" + NumToString(oidx) +
               " + 4].copy_from_slice(&off);\n";
          GenMarshalField(info, fname, &c, "        ");
          oidx++;
        }
      }
    }
    c += "    }\n\n";
  }

  void GenMarshalField(const SszFieldInfo &info, const std::string &var,
                       std::string *code, const std::string &ind) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::Bool:
        c += ind + "buf.push(if " + var + " { 1 } else { 0 });\n";
        break;
      case SszType::Uint8:
        c += ind + "buf.push(" + var + ");\n";
        break;
      case SszType::Uint16:
        c += ind + "buf.extend_from_slice(&" + var + ".to_le_bytes());\n";
        break;
      case SszType::Uint32:
        c += ind + "buf.extend_from_slice(&" + var + ".to_le_bytes());\n";
        break;
      case SszType::Uint64:
        c += ind + "buf.extend_from_slice(&" + var + ".to_le_bytes());\n";
        break;
      case SszType::Uint128: case SszType::Uint256:
        c += ind + "buf.extend_from_slice(&" + var + ");\n";
        break;
      case SszType::Vector:
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += ind + "buf.extend_from_slice(&" + var + ");\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += ind + "for item in &" + var + " { item.ssz_append(buf); }\n";
        } else if (info.elem_info) {
          GenMarshalPrimitiveArray(info, var, code, ind);
        }
        break;
      case SszType::ProgressiveList:
      case SszType::List:
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          // [][]u8 — offset-based
          c += ind + "{\n";
          c += ind + "    let list_start = buf.len();\n";
          c += ind + "    for _ in &" + var +
               " { buf.extend_from_slice(&[0u8; 4]); }\n";
          c += ind + "    for (i, item) in " + var + ".iter().enumerate() {\n";
          c += ind + "        let off = ((buf.len() - list_start) as "
               "u32).to_le_bytes();\n";
          c += ind + "        buf[list_start + i * 4..list_start + i * 4 + "
               "4].copy_from_slice(&off);\n";
          c += ind + "        buf.extend_from_slice(item);\n";
          c += ind + "    }\n";
          c += ind + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += ind + "buf.extend_from_slice(&" + var + ");\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          if (info.elem_info->is_dynamic) {
            c += ind + "{\n";
            c += ind + "    let list_start = buf.len();\n";
            c += ind + "    for _ in &" + var +
                 " { buf.extend_from_slice(&[0u8; 4]); }\n";
            c += ind + "    for (i, item) in " + var +
                 ".iter().enumerate() {\n";
            c += ind + "        let off = ((buf.len() - list_start) as "
                 "u32).to_le_bytes();\n";
            c += ind +
                 "        buf[list_start + i * 4..list_start + i * 4 + "
                 "4].copy_from_slice(&off);\n";
            c += ind + "        item.ssz_append(buf);\n";
            c += ind + "    }\n";
            c += ind + "}\n";
          } else {
            c += ind + "for item in &" + var + " { item.ssz_append(buf); }\n";
          }
        } else if (info.elem_info) {
          GenMarshalPrimitiveSlice(info, var, code, ind);
        } else {
          c += ind + "buf.extend_from_slice(&" + var + ");\n";
        }
        break;
      case SszType::Bitlist:
        c += ind + "buf.extend_from_slice(&" + var + ");\n";
        break;
      case SszType::Bitvector:
        c += ind + "buf.extend_from_slice(&" + var + ");\n";
        break;
      case SszType::ProgressiveContainer:
      case SszType::Container:
        c += ind + var + ".ssz_append(buf);\n";
        break;
      case SszType::Union: break;
    }
  }

  void GenMarshalPrimitiveArray(const SszFieldInfo &info,
                                const std::string &var, std::string *code,
                                const std::string &ind) {
    std::string &c = *code;
    c += ind + "for v in &" + var + " {\n";
    switch (info.elem_info->ssz_type) {
      case SszType::Bool:
        c += ind + "    buf.push(if *v { 1 } else { 0 });\n"; break;
      case SszType::Uint16: case SszType::Uint32: case SszType::Uint64:
        c += ind + "    buf.extend_from_slice(&v.to_le_bytes());\n"; break;
      default: break;
    }
    c += ind + "}\n";
  }

  void GenMarshalPrimitiveSlice(const SszFieldInfo &info,
                                const std::string &var, std::string *code,
                                const std::string &ind) {
    // Same as array version for Rust
    GenMarshalPrimitiveArray(info, var, code, ind);
  }

  // -----------------------------------------------------------------------
  // Unmarshal
  // -----------------------------------------------------------------------

  void GenUnmarshalMethod(const std::string & /*name*/,
                          const SszContainerInfo &container,
                          std::string *code) {
    std::string &c = *code;
    c += "    pub fn from_ssz_bytes(bytes: &[u8]) -> Result<Self, SszError> {\n";
    c += "        if bytes.len() < " +
         NumToString(container.static_size) + " {\n";
    c += "            return Err(SszError::BufferTooSmall);\n";
    c += "        }\n";

    if (container.dynamic_fields.empty()) {
      c += "        Ok(Self {\n";
      uint32_t offset = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        c += "            " + ToSnakeCase(field->name) + ": " +
             GenUnmarshalExpr(info, "bytes", offset) + ",\n";
        offset += info.fixed_size;
      }
      c += "        })\n";
    } else {
      c += "        let mut result = Self::default();\n";
      uint32_t offset = 0;
      int dyn_idx = 0;
      std::vector<std::pair<const FieldDef *, int>> dyn_fields;

      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = ToSnakeCase(field->name);

        if (info.is_dynamic) {
          c += "        let offset" + NumToString(dyn_idx) +
               " = u32::from_le_bytes(bytes[" + NumToString(offset) +
               ".." + NumToString(offset + 4) +
               "].try_into().unwrap()) as usize;\n";
          if (dyn_idx == 0) {
            c += "        if offset0 != " +
                 NumToString(container.static_size) +
                 " { return Err(SszError::InvalidOffset); }\n";
          } else {
            c += "        if offset" + NumToString(dyn_idx) +
                 " < offset" + NumToString(dyn_idx - 1) +
                 " || offset" + NumToString(dyn_idx) +
                 " > bytes.len() { return Err(SszError::InvalidOffset); }\n";
          }
          dyn_fields.push_back({field, dyn_idx});
          offset += 4;
          dyn_idx++;
        } else {
          c += "        result." + fname + " = " +
               GenUnmarshalExpr(info, "bytes", offset) + ";\n";
          offset += info.fixed_size;
        }
      }

      for (size_t i = 0; i < dyn_fields.size(); i++) {
        auto *field = dyn_fields[i].first;
        int oidx = dyn_fields[i].second;
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = ToSnakeCase(field->name);
        std::string start = "offset" + NumToString(oidx);
        std::string end = (i + 1 < dyn_fields.size())
                              ? "offset" + NumToString(dyn_fields[i + 1].second)
                              : "bytes.len()";

        c += "        result." + fname + " = " +
             GenUnmarshalDynExpr(info, "bytes", start, end) + ";\n";
      }

      c += "        Ok(result)\n";
    }
    c += "    }\n\n";
  }

  std::string GenUnmarshalExpr(const SszFieldInfo &info,
                               const std::string &buf, uint32_t offset) {
    std::string s = NumToString(offset);
    std::string e = NumToString(offset + info.fixed_size);
    std::string slice = buf + "[" + s + ".." + e + "]";

    switch (info.ssz_type) {
      case SszType::Bool:
        return "(" + buf + "[" + s + "] == 1)";
      case SszType::Uint8:
        return buf + "[" + s + "]";
      case SszType::Uint16:
        return "u16::from_le_bytes(" + slice + ".try_into().unwrap())";
      case SszType::Uint32:
        return "u32::from_le_bytes(" + slice + ".try_into().unwrap())";
      case SszType::Uint64:
        return "u64::from_le_bytes(" + slice + ".try_into().unwrap())";
      case SszType::Uint128: case SszType::Uint256:
      case SszType::Vector: case SszType::Bitvector: {
        // Fixed byte arrays — use try_into
        return slice + ".try_into().unwrap()";
      }
      case SszType::ProgressiveContainer:
      case SszType::Container:
        return info.struct_def->name + "::from_ssz_bytes(&" + slice + ")?";
      default:
        return "Default::default()";
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
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          // Vec<Vec<u8>> — offset-based
          return "{\n            let sl = &" + slice + ";\n"
                 "            if sl.is_empty() { Vec::new() } else {\n"
                 "                let first_off = u32::from_le_bytes("
                 "sl[0..4].try_into().unwrap()) as usize;\n"
                 "                let count = first_off / 4;\n"
                 "                let mut out = Vec::with_capacity(count);\n"
                 "                for i in 0..count {\n"
                 "                    let s = u32::from_le_bytes(sl[i*4..i*4+4]"
                 ".try_into().unwrap()) as usize;\n"
                 "                    let e = if i + 1 < count { "
                 "u32::from_le_bytes(sl[(i+1)*4..(i+1)*4+4]"
                 ".try_into().unwrap()) as usize } else { sl.len() };\n"
                 "                    out.push(sl[s..e].to_vec());\n"
                 "                }\n"
                 "                out\n"
                 "            }\n"
                 "        }";
        }
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8)
          return slice + ".to_vec()";
        if (info.elem_info &&
            info.elem_info->ssz_type == SszType::Container) {
          if (info.elem_info->is_dynamic) {
            return "{\n            let sl = &" + slice + ";\n"
                   "            if sl.is_empty() { Vec::new() } else {\n"
                   "                let first_off = u32::from_le_bytes("
                   "sl[0..4].try_into().unwrap()) as usize;\n"
                   "                let count = first_off / 4;\n"
                   "                let mut out = Vec::with_capacity(count);\n"
                   "                for i in 0..count {\n"
                   "                    let s = u32::from_le_bytes("
                   "sl[i*4..i*4+4].try_into().unwrap()) as usize;\n"
                   "                    let e = if i + 1 < count { "
                   "u32::from_le_bytes(sl[(i+1)*4..(i+1)*4+4]"
                   ".try_into().unwrap()) as usize } else { sl.len() };\n"
                   "                    out.push(" +
                   info.elem_info->struct_def->name +
                   "::from_ssz_bytes(&sl[s..e])?);\n"
                   "                }\n"
                   "                out\n"
                   "            }\n"
                   "        }";
          }
          uint32_t esize = info.elem_info->fixed_size;
          return "{\n            let sl = &" + slice + ";\n"
                 "            let count = sl.len() / " +
                 NumToString(esize) + ";\n"
                 "            let mut out = Vec::with_capacity(count);\n"
                 "            for i in 0..count {\n"
                 "                out.push(" +
                 info.elem_info->struct_def->name +
                 "::from_ssz_bytes(&sl[i*" + NumToString(esize) +
                 "..(i+1)*" + NumToString(esize) + "])?);\n"
                 "            }\n"
                 "            out\n"
                 "        }";
        }
        if (info.elem_info) {
          uint32_t esize = info.elem_info->fixed_size;
          return GenUnmarshalPrimitiveSlice(*info.elem_info, slice, esize);
        }
        return slice + ".to_vec()";
      case SszType::Bitlist:
        return slice + ".to_vec()";
      case SszType::ProgressiveContainer:
      case SszType::Container:
        return info.struct_def->name + "::from_ssz_bytes(&" + slice + ")?";
      default:
        return "Default::default()";
    }
  }

  std::string GenUnmarshalPrimitiveSlice(const SszFieldInfo &elem,
                                         const std::string &slice,
                                         uint32_t esize) {
    std::string typ;
    switch (elem.ssz_type) {
      case SszType::Bool: typ = "bool"; break;
      case SszType::Uint16: typ = "u16"; break;
      case SszType::Uint32: typ = "u32"; break;
      case SszType::Uint64: typ = "u64"; break;
      default: return slice + ".to_vec()";
    }
    std::string body = "{\n            let sl = &" + slice + ";\n"
                       "            let count = sl.len() / " +
                       NumToString(esize) + ";\n"
                       "            let mut out = Vec::with_capacity(count);\n"
                       "            for i in 0..count {\n";
    if (elem.ssz_type == SszType::Bool) {
      body += "                out.push(sl[i] == 1);\n";
    } else {
      body += "                out.push(" + typ +
              "::from_le_bytes(sl[i*" + NumToString(esize) + "..(i+1)*" +
              NumToString(esize) + "].try_into().unwrap()));\n";
    }
    body += "            }\n            out\n        }";
    return body;
  }

  // -----------------------------------------------------------------------
  // HashTreeRoot
  // -----------------------------------------------------------------------

  void GenHashMethod(const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    c += "    pub fn tree_hash_with(&self, h: &mut Hasher) {\n";
    c += "        let idx = h.index();\n";

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
          c += "        h.put_zero_hash();\n";
        }
      }

      // Emit active_fields bitvector literal
      c += "        h.merkleize_progressive_with_active_fields(idx, &[";
      for (size_t i = 0; i < container.active_fields_bitvector.size(); i++) {
        if (i > 0) c += ", ";
        char hex[8];
        snprintf(hex, sizeof(hex), "0x%02x",
                 container.active_fields_bitvector[i]);
        c += hex;
      }
      c += "]);\n";
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
        c += ind + "h.put_bool(" + var + ");\n"; break;
      case SszType::Uint8:
        c += ind + "h.put_u8(" + var + ");\n"; break;
      case SszType::Uint16:
        c += ind + "h.put_u16(" + var + ");\n"; break;
      case SszType::Uint32:
        c += ind + "h.put_u32(" + var + ");\n"; break;
      case SszType::Uint64:
        c += ind + "h.put_u64(" + var + ");\n"; break;
      case SszType::Uint128: case SszType::Uint256:
        c += ind + "h.put_bytes(&" + var + ");\n"; break;
      case SszType::Vector:
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += ind + "h.put_bytes(&" + var + ");\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += ind + "{\n";
          c += ind + "    let sub = h.index();\n";
          c += ind + "    for item in &" + var +
               " { item.tree_hash_with(h); }\n";
          c += ind + "    h.merkleize(sub);\n";
          c += ind + "}\n";
        } else if (info.elem_info) {
          c += ind + "{\n";
          c += ind + "    let sub = h.index();\n";
          c += ind + "    for v in &" + var + " {\n";
          GenAppendPrimitive(info.elem_info->ssz_type, "*v", code, ind + "        ");
          c += ind + "    }\n";
          c += ind + "    h.fill_up_to_32();\n";
          c += ind + "    h.merkleize(sub);\n";
          c += ind + "}\n";
        }
        break;
      case SszType::List: {
        uint64_t limit = info.limit;
        if (info.elem_info && info.elem_info->ssz_type == SszType::List) {
          uint64_t inner_limit = info.elem_info->limit;
          uint64_t inner_chunk = inner_limit > 0 ? (inner_limit + 31) / 32 : 0;
          c += ind + "{\n";
          c += ind + "    let sub = h.index();\n";
          c += ind + "    for item in &" + var + " {\n";
          c += ind + "        let inner = h.index();\n";
          c += ind + "        h.append_bytes32(item);\n";
          if (inner_chunk > 0) {
            c += ind + "        h.merkleize_with_mixin(inner, item.len() as "
                 "u64, " + NumToString(inner_chunk) + ");\n";
          } else {
            c += ind + "        h.merkleize(inner);\n";
          }
          c += ind + "    }\n";
          c += ind + "    h.merkleize_with_mixin(sub, " + var +
               ".len() as u64, " + NumToString(limit) + ");\n";
          c += ind + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Uint8) {
          c += ind + "{\n";
          c += ind + "    let sub = h.index();\n";
          c += ind + "    h.append_bytes32(&" + var + ");\n";
          c += ind + "    h.merkleize_with_mixin(sub, " + var +
               ".len() as u64, " +
               NumToString((limit + 31) / 32) + ");\n";
          c += ind + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += ind + "{\n";
          c += ind + "    let sub = h.index();\n";
          c += ind + "    for item in &" + var +
               " { item.tree_hash_with(h); }\n";
          c += ind + "    h.merkleize_with_mixin(sub, " + var +
               ".len() as u64, " + NumToString(limit) + ");\n";
          c += ind + "}\n";
        } else if (info.elem_info) {
          c += ind + "{\n";
          c += ind + "    let sub = h.index();\n";
          c += ind + "    for v in &" + var + " {\n";
          GenAppendPrimitive(info.elem_info->ssz_type, "*v", code,
                             ind + "        ");
          c += ind + "    }\n";
          c += ind + "    h.fill_up_to_32();\n";
          uint64_t hash_limit =
              (limit * info.elem_info->fixed_size + 31) / 32;
          c += ind + "    h.merkleize_with_mixin(sub, " + var +
               ".len() as u64, " + NumToString(hash_limit) + ");\n";
          c += ind + "}\n";
        }
        break;
      }
      case SszType::Bitlist:
        c += ind + "h.put_bitlist(&" + var + ", " +
             NumToString(info.limit) + ");\n";
        break;
      case SszType::Bitvector:
        c += ind + "h.put_bytes(&" + var + ");\n";
        break;
      case SszType::Container:
        c += ind + var + ".tree_hash_with(h);\n";
        break;
      case SszType::ProgressiveContainer:
        c += ind + var + ".tree_hash_with(h);\n";
        break;
      case SszType::ProgressiveList: {
        // EIP-7916: use merkleize_progressive_with_mixin
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += ind + "{\n";
          c += ind + "    let sub = h.index();\n";
          c += ind + "    h.append_bytes32(&" + var + ");\n";
          c += ind + "    h.merkleize_progressive_with_mixin(sub, " + var +
               ".len() as u64);\n";
          c += ind + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          c += ind + "{\n";
          c += ind + "    let sub = h.index();\n";
          c += ind + "    for item in &" + var +
               " { item.tree_hash_with(h); }\n";
          c += ind + "    h.merkleize_progressive_with_mixin(sub, " + var +
               ".len() as u64);\n";
          c += ind + "}\n";
        } else if (info.elem_info) {
          c += ind + "{\n";
          c += ind + "    let sub = h.index();\n";
          c += ind + "    for v in &" + var + " {\n";
          GenAppendPrimitive(info.elem_info->ssz_type, "*v", code,
                             ind + "        ");
          c += ind + "    }\n";
          c += ind + "    h.fill_up_to_32();\n";
          c += ind + "    h.merkleize_progressive_with_mixin(sub, " + var +
               ".len() as u64);\n";
          c += ind + "}\n";
        }
        break;
      }
      case SszType::Union: break;
    }
  }

  void GenAppendPrimitive(SszType t, const std::string &var, std::string *code,
                          const std::string &ind) {
    std::string &c = *code;
    switch (t) {
      case SszType::Bool: c += ind + "h.append_bool(" + var + ");\n"; break;
      case SszType::Uint8: c += ind + "h.append_u8(" + var + ");\n"; break;
      case SszType::Uint16: c += ind + "h.append_u16(" + var + ");\n"; break;
      case SszType::Uint32: c += ind + "h.append_u32(" + var + ");\n"; break;
      case SszType::Uint64: c += ind + "h.append_u64(" + var + ");\n"; break;
      default: break;
    }
  }

  // =======================================================================
  // Zero-copy view types
  // =======================================================================

  // --- Validation for fixed containers ---
  void GenViewValidateFixed(const SszContainerInfo &container,
                            std::string *code) {
    std::string &c = *code;
    // Validate bool fields are 0 or 1
    uint32_t offset = 0;
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      auto &info = it->second;
      if (info.ssz_type == SszType::Bool) {
        c += "        if buf[" + NumToString(offset) +
             "] > 1 { return Err(SszError::InvalidBool); }\n";
      }
      // Recursively validate nested fixed containers
      if (info.ssz_type == SszType::Container && info.struct_def) {
        c += "        " + info.struct_def->name +
             "View::from_ssz_bytes(&buf[" + NumToString(offset) + ".." +
             NumToString(offset + info.fixed_size) + "])?;\n";
      }
      offset += info.fixed_size;
    }
  }

  // --- Validation for dynamic containers ---
  void GenViewValidateDynamic(const SszContainerInfo &container,
                              std::string *code) {
    std::string &c = *code;
    uint32_t ss = container.static_size;

    // Read and validate all offsets
    uint32_t offset = 0;
    int dyn_idx = 0;
    std::vector<int> dyn_indices;
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      auto &info = it->second;

      if (info.is_dynamic) {
        c += "        let _voff" + NumToString(dyn_idx) +
             " = u32::from_le_bytes(buf[" + NumToString(offset) + ".." +
             NumToString(offset + 4) + "].try_into().unwrap()) as usize;\n";
        if (dyn_idx == 0) {
          c += "        if _voff0 != " + NumToString(ss) +
               " { return Err(SszError::InvalidOffset); }\n";
        } else {
          c += "        if _voff" + NumToString(dyn_idx) + " < _voff" +
               NumToString(dyn_idx - 1) + " || _voff" +
               NumToString(dyn_idx) +
               " > buf.len() { return Err(SszError::InvalidOffset); }\n";
        }
        dyn_indices.push_back(dyn_idx);
        dyn_idx++;
        offset += 4;
      } else {
        // Validate fixed fields inline
        if (info.ssz_type == SszType::Bool) {
          c += "        if buf[" + NumToString(offset) +
               "] > 1 { return Err(SszError::InvalidBool); }\n";
        }
        if (info.ssz_type == SszType::Container && info.struct_def) {
          c += "        " + info.struct_def->name +
               "View::from_ssz_bytes(&buf[" + NumToString(offset) + ".." +
               NumToString(offset + info.fixed_size) + "])?;\n";
        }
        offset += info.fixed_size;
      }
    }

    // Validate dynamic field contents
    for (size_t i = 0; i < dyn_indices.size(); i++) {
      int di = dyn_indices[i];
      std::string start = "_voff" + NumToString(di);
      std::string end = (i + 1 < dyn_indices.size())
                            ? "_voff" + NumToString(dyn_indices[i + 1])
                            : std::string("buf.len()");

      auto *field = container.dynamic_fields[i];
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      auto &info = it->second;

      // For lists of dynamic elements, validate inner offsets
      if (info.ssz_type == SszType::List && info.elem_info &&
          info.elem_info->ssz_type == SszType::Container &&
          info.elem_info->is_dynamic) {
        c += "        {\n";
        c += "            let sl = &buf[" + start + ".." + end + "];\n";
        c += "            if !sl.is_empty() {\n";
        c += "                if sl.len() < 4 { return Err(SszError::InvalidOffset); }\n";
        c += "                let fo = u32::from_le_bytes(sl[0..4].try_into().unwrap()) as usize;\n";
        c += "                if fo % 4 != 0 || fo > sl.len() { return Err(SszError::InvalidOffset); }\n";
        c += "                let cnt = fo / 4;\n";
        c += "                let mut prev = fo;\n";
        c += "                for j in 1..cnt {\n";
        c += "                    let o = u32::from_le_bytes(sl[j*4..j*4+4].try_into().unwrap()) as usize;\n";
        c += "                    if o < prev || o > sl.len() { return Err(SszError::InvalidOffset); }\n";
        c += "                    prev = o;\n";
        c += "                }\n";
        c += "            }\n";
        c += "        }\n";
      }
      // For lists of fixed structs, validate length is multiple of elem size
      if (info.ssz_type == SszType::List && info.elem_info &&
          info.elem_info->ssz_type == SszType::Container &&
          !info.elem_info->is_dynamic && info.elem_info->fixed_size > 0) {
        c += "        if (" + end + " - " + start + ") % " +
             NumToString(info.elem_info->fixed_size) +
             " != 0 { return Err(SszError::InvalidOffset); }\n";
      }
    }
  }

  void GenViewStruct(const StructDef &struct_def,
                     const SszContainerInfo & /*container*/, std::string *code) {
    std::string &c = *code;
    std::string name = struct_def.name + "View";
    c += "/// Zero-copy view into SSZ-encoded `" + struct_def.name + "`.\n";
    c += "/// Reads fields directly from the underlying buffer.\n";
    c += "#[derive(Clone, Copy, Debug)]\n";
    c += "pub struct " + name + "<'a>(pub &'a [u8]);\n";
  }

  void GenViewImpl(const StructDef &struct_def,
                   const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    std::string name = struct_def.name + "View";
    std::string owned = struct_def.name;

    c += "impl<'a> " + name + "<'a> {\n";

    // --- from_ssz_bytes (with full validation) ---
    c += "    pub fn from_ssz_bytes(buf: &'a [u8]) -> Result<Self, SszError> {\n";
    c += "        if buf.len() < " + NumToString(container.static_size) +
         " { return Err(SszError::BufferTooSmall); }\n";
    if (container.is_fixed) {
      // Fixed container: length check is sufficient — all offsets are
      // compile-time constants within [0, static_size).
      GenViewValidateFixed(container, code);
      c += "        Ok(Self(&buf[.." + NumToString(container.static_size) +
           "]))\n";
    } else {
      // Dynamic container: validate offset table
      GenViewValidateDynamic(container, code);
      c += "        Ok(Self(buf))\n";
    }
    c += "    }\n\n";

    // --- as_ssz_bytes ---
    c += "    pub fn as_ssz_bytes(&self) -> &'a [u8] { self.0 }\n\n";

    // --- field accessors ---
    if (container.dynamic_fields.empty()) {
      GenViewFixedAccessors(container, code);
    } else {
      GenViewDynamicAccessors(container, code);
    }

    // --- to_owned ---
    c += "    pub fn to_owned_type(&self) -> " + owned + " {\n";
    c += "        " + owned + "::from_ssz_bytes(self.0).unwrap()\n";
    c += "    }\n\n";

    // --- tree_hash_root ---
    c += "    pub fn tree_hash_root(&self) -> [u8; 32] {\n";
    c += "        let mut h = HasherPool::get();\n";
    c += "        self.tree_hash_with(&mut h);\n";
    c += "        let root = h.finish();\n";
    c += "        HasherPool::put(h);\n";
    c += "        root\n";
    c += "    }\n\n";

    // --- tree_hash_with ---
    GenViewHashMethod(container, code);

    c += "}\n";
  }

  // --- Fixed container accessors ---
  void GenViewFixedAccessors(const SszContainerInfo &container,
                             std::string *code) {
    std::string &c = *code;
    uint32_t offset = 0;
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      auto &info = it->second;
      std::string fname = ToSnakeCase(field->name);
      GenViewAccessor(info, fname, offset, code);
      offset += info.fixed_size;
    }
  }

  void GenViewAccessor(const SszFieldInfo &info, const std::string &fname,
                       uint32_t offset, std::string *code) {
    std::string &c = *code;
    std::string s = NumToString(offset);
    switch (info.ssz_type) {
      case SszType::Bool:
        c += "    pub fn " + fname + "(&self) -> bool { self.0[" + s +
             "] == 1 }\n";
        break;
      case SszType::Uint8:
        c += "    pub fn " + fname + "(&self) -> u8 { self.0[" + s + "] }\n";
        break;
      case SszType::Uint16:
        c += "    pub fn " + fname +
             "(&self) -> u16 { u16::from_le_bytes(self.0[" + s + ".." +
             NumToString(offset + 2) + "].try_into().unwrap()) }\n";
        break;
      case SszType::Uint32:
        c += "    pub fn " + fname +
             "(&self) -> u32 { u32::from_le_bytes(self.0[" + s + ".." +
             NumToString(offset + 4) + "].try_into().unwrap()) }\n";
        break;
      case SszType::Uint64:
        c += "    pub fn " + fname +
             "(&self) -> u64 { u64::from_le_bytes(self.0[" + s + ".." +
             NumToString(offset + 8) + "].try_into().unwrap()) }\n";
        break;
      case SszType::Uint128:
      case SszType::Uint256:
      case SszType::Bitvector:
        c += "    pub fn " + fname + "(&self) -> &'a [u8] { &self.0[" + s +
             ".." + NumToString(offset + info.fixed_size) + "] }\n";
        break;
      case SszType::Vector:
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += "    pub fn " + fname + "(&self) -> &'a [u8] { &self.0[" + s +
               ".." + NumToString(offset + info.fixed_size) + "] }\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container &&
                   info.elem_info->struct_def) {
          uint32_t esize = info.elem_info->fixed_size;
          std::string vname = info.elem_info->struct_def->name + "View";
          c += "    pub fn " + fname + "_len(&self) -> usize { " +
               NumToString(info.limit) + " }\n";
          c += "    pub fn " + fname + "_at(&self, i: usize) -> " + vname +
               "<'a> { " + vname + "(&self.0[" + s + " + i * " +
               NumToString(esize) + ".." + s + " + (i + 1) * " +
               NumToString(esize) + "]) }\n";
        } else {
          c += "    pub fn " + fname + "_bytes(&self) -> &'a [u8] { &self.0[" +
               s + ".." + NumToString(offset + info.fixed_size) + "] }\n";
        }
        break;
      case SszType::Container:
        if (info.struct_def) {
          c += "    pub fn " + fname + "(&self) -> " +
               info.struct_def->name + "View<'a> { " +
               info.struct_def->name + "View(&self.0[" + s + ".." +
               NumToString(offset + info.fixed_size) + "]) }\n";
        }
        break;
      default:
        break;
    }
  }

  // --- Dynamic container accessors ---
  void GenViewDynamicAccessors(const SszContainerInfo &container,
                               std::string *code) {
    std::string &c = *code;

    // Helper to get offset of dynamic field i
    // We generate private offset helpers
    int dyn_count = 0;
    uint32_t offset = 0;
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      auto &info = it->second;
      if (info.is_dynamic) {
        c += "    fn _off" + NumToString(dyn_count) +
             "(&self) -> usize { u32::from_le_bytes(self.0[" +
             NumToString(offset) + ".." + NumToString(offset + 4) +
             "].try_into().unwrap()) as usize }\n";
        dyn_count++;
        offset += 4;
      } else {
        offset += info.fixed_size;
      }
    }
    if (dyn_count > 0) c += "\n";

    // Generate accessors
    offset = 0;
    int dyn_idx = 0;
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      auto &info = it->second;
      std::string fname = ToSnakeCase(field->name);

      if (info.is_dynamic) {
        std::string start = "self._off" + NumToString(dyn_idx) + "()";
        std::string end;
        if (dyn_idx + 1 < dyn_count) {
          end = "self._off" + NumToString(dyn_idx + 1) + "()";
        } else {
          end = "self.0.len()";
        }
        GenViewDynAccessor(info, fname, start, end, code);
        dyn_idx++;
        offset += 4;
      } else {
        GenViewAccessor(info, fname, offset, code);
        offset += info.fixed_size;
      }
    }
  }

  void GenViewDynAccessor(const SszFieldInfo &info, const std::string &fname,
                          const std::string &start, const std::string &end,
                          std::string *code) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::List:
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += "    pub fn " + fname +
               "(&self) -> &'a [u8] { &self.0[" + start + ".." + end +
               "] }\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container &&
                   info.elem_info->struct_def && !info.elem_info->is_dynamic) {
          uint32_t esize = info.elem_info->fixed_size;
          std::string vname = info.elem_info->struct_def->name + "View";
          c += "    pub fn " + fname +
               "_bytes(&self) -> &'a [u8] { &self.0[" + start + ".." + end +
               "] }\n";
          c += "    pub fn " + fname + "_len(&self) -> usize { (" + end +
               " - " + start + ") / " + NumToString(esize) + " }\n";
          c += "    pub fn " + fname + "_at(&self, i: usize) -> " + vname +
               "<'a> { let s = " + start + "; " + vname +
               "(&self.0[s + i * " + NumToString(esize) + "..s + (i + 1) * " +
               NumToString(esize) + "]) }\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container &&
                   info.elem_info->struct_def && info.elem_info->is_dynamic) {
          // Dynamic element list — offset-based
          std::string vname = info.elem_info->struct_def->name + "View";
          c += "    pub fn " + fname +
               "_bytes(&self) -> &'a [u8] { &self.0[" + start + ".." + end +
               "] }\n";
          c += "    pub fn " + fname +
               "_len(&self) -> usize { let sl = self." + fname +
               "_bytes(); if sl.is_empty() { 0 } else { "
               "u32::from_le_bytes(sl[0..4].try_into().unwrap()) as usize / "
               "4 } }\n";
          c += "    pub fn " + fname + "_at(&self, i: usize) -> " + vname +
               "<'a> {\n";
          c += "        let sl = self." + fname + "_bytes();\n";
          c += "        let s = u32::from_le_bytes(sl[i*4..i*4+4]"
               ".try_into().unwrap()) as usize;\n";
          c += "        let e = if i + 1 < self." + fname +
               "_len() { u32::from_le_bytes(sl[(i+1)*4..(i+1)*4+4]"
               ".try_into().unwrap()) as usize } else { sl.len() };\n";
          c += "        " + vname + "(&sl[s..e])\n";
          c += "    }\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::List) {
          // List of byte lists ([][]u8)
          c += "    pub fn " + fname +
               "_bytes(&self) -> &'a [u8] { &self.0[" + start + ".." + end +
               "] }\n";
        } else {
          c += "    pub fn " + fname +
               "(&self) -> &'a [u8] { &self.0[" + start + ".." + end +
               "] }\n";
        }
        break;
      case SszType::Bitlist:
        c += "    pub fn " + fname +
             "(&self) -> &'a [u8] { &self.0[" + start + ".." + end + "] }\n";
        break;
      case SszType::Container:
        if (info.struct_def) {
          c += "    pub fn " + fname + "(&self) -> " +
               info.struct_def->name + "View<'a> { " +
               info.struct_def->name + "View(&self.0[" + start + ".." + end +
               "]) }\n";
        }
        break;
      default:
        c += "    pub fn " + fname +
             "_bytes(&self) -> &'a [u8] { &self.0[" + start + ".." + end +
             "] }\n";
        break;
    }
  }

  // --- View tree_hash_with ---
  void GenViewHashMethod(const SszContainerInfo &container, std::string *code) {
    std::string &c = *code;
    c += "    pub fn tree_hash_with(&self, h: &mut Hasher) {\n";

    // Check if any field requires owned-type delegation (lists, bitlists,
    // dynamic containers, progressive lists). If so, delegate the entire
    // hash to avoid partial merkleization issues.
    bool has_complex = false;
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      if (it == container.field_infos.end()) continue;
      auto &info = it->second;
      if (info.ssz_type == SszType::List ||
          info.ssz_type == SszType::ProgressiveList ||
          info.ssz_type == SszType::Bitlist ||
          (info.ssz_type == SszType::Container && info.is_dynamic)) {
        has_complex = true;
        break;
      }
    }

    if (container.is_progressive || has_complex) {
      // Delegate to owned type for progressive or complex containers
      c += "        self.to_owned_type().tree_hash_with(h);\n";
    } else {
      // Pure fixed container — hash directly from buffer
      c += "        let idx = h.index();\n";
      uint32_t offset = 0;
      for (auto *field : container.all_fields) {
        auto it = container.field_infos.find(field);
        if (it == container.field_infos.end()) continue;
        auto &info = it->second;
        std::string fname = ToSnakeCase(field->name);
        GenViewHashField(info, "self." + fname + "()", fname, offset, code,
                         "        ");
        offset += info.fixed_size;
      }
      c += "        h.merkleize(idx);\n";
    }
    c += "    }\n";
  }

  void GenViewHashField(const SszFieldInfo &info, const std::string &accessor,
                        const std::string & /*fname*/, uint32_t /*offset*/,
                        std::string *code, const std::string &ind) {
    std::string &c = *code;
    switch (info.ssz_type) {
      case SszType::Bool:
        c += ind + "h.put_bool(" + accessor + ");\n"; break;
      case SszType::Uint8:
        c += ind + "h.put_u8(" + accessor + ");\n"; break;
      case SszType::Uint16:
        c += ind + "h.put_u16(" + accessor + ");\n"; break;
      case SszType::Uint32:
        c += ind + "h.put_u32(" + accessor + ");\n"; break;
      case SszType::Uint64:
        c += ind + "h.put_u64(" + accessor + ");\n"; break;
      case SszType::Uint128: case SszType::Uint256:
        c += ind + "h.put_bytes(" + accessor + ");\n"; break;
      case SszType::Vector:
      case SszType::Bitvector:
        c += ind + "h.put_bytes(" + accessor + ");\n"; break;
      case SszType::Container:
        c += ind + accessor + ".tree_hash_with(h);\n"; break;
      case SszType::List: {
        uint64_t limit = info.limit;
        if (info.elem_info && info.elem_info->ssz_type == SszType::Uint8) {
          c += ind + "{\n";
          c += ind + "    let sub = h.index();\n";
          c += ind + "    h.append_bytes32(" + accessor + ");\n";
          c += ind + "    h.merkleize_with_mixin(sub, " + accessor +
               ".len() as u64, " + NumToString((limit + 31) / 32) + ");\n";
          c += ind + "}\n";
        } else if (info.elem_info &&
                   info.elem_info->ssz_type == SszType::Container) {
          // Delegate to owned-type hash pattern
          c += ind + "// list hash delegated to to_owned_type()\n";
          c += ind + "self.to_owned_type().tree_hash_with(h);\n";
          c += ind + "return;\n";
        } else {
          c += ind + "// complex list — delegate\n";
          c += ind + "self.to_owned_type().tree_hash_with(h);\n";
          c += ind + "return;\n";
        }
        break;
      }
      case SszType::Bitlist:
        c += ind + "h.put_bitlist(" + accessor + ", " +
             NumToString(info.limit) + ");\n";
        break;
      case SszType::ProgressiveContainer:
        c += ind + accessor + ".tree_hash_with(h);\n"; break;
      case SszType::ProgressiveList:
        // Progressive lists always delegate to owned type
        c += ind + "self.to_owned_type().tree_hash_with(h);\n";
        c += ind + "return;\n";
        break;
      default: break;
    }
  }
};

}  // namespace ssz_rust

// ---------------------------------------------------------------------------
// Wrapper
// ---------------------------------------------------------------------------

static bool GenerateSszRust(const Parser &parser, const std::string &path,
                            const std::string &file_name) {
  ssz_rust::SszRustGenerator generator(parser, path, file_name);
  return generator.generate();
}

namespace {

class SszRustCodeGenerator : public CodeGenerator {
 public:
  Status GenerateCode(const Parser &parser, const std::string &path,
                      const std::string &filename) override {
    if (!GenerateSszRust(parser, path, filename)) return Status::ERROR;
    return Status::OK;
  }
  Status GenerateCode(const uint8_t *, int64_t,
                      const CodeGenOptions &) override {
    return Status::NOT_IMPLEMENTED;
  }
  Status GenerateMakeRule(const Parser &, const std::string &,
                          const std::string &, std::string &) override {
    return Status::NOT_IMPLEMENTED;
  }
  Status GenerateGrpcCode(const Parser &, const std::string &,
                          const std::string &) override {
    return Status::NOT_IMPLEMENTED;
  }
  Status GenerateRootFile(const Parser &, const std::string &) override {
    return Status::NOT_IMPLEMENTED;
  }
  bool IsSchemaOnly() const override { return true; }
  bool SupportsBfbsGeneration() const override { return false; }
  bool SupportsRootFileGeneration() const override { return false; }
  IDLOptions::Language Language() const override { return IDLOptions::kSszRust; }
  std::string LanguageName() const override { return "SszRust"; }
};

}  // namespace

std::unique_ptr<CodeGenerator> NewSszRustCodeGenerator() {
  return std::unique_ptr<SszRustCodeGenerator>(new SszRustCodeGenerator());
}

}  // namespace flatbuffers
