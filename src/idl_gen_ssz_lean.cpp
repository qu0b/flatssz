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

#include "idl_gen_ssz_lean.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "flatbuffers/base.h"
#include "flatbuffers/code_generators.h"
#include "flatbuffers/flatbuffers.h"
#include "flatbuffers/flatc.h"
#include "flatbuffers/idl.h"
#include "flatbuffers/util.h"

namespace flatbuffers {

namespace ssz_lean {

// ---------------------------------------------------------------------------
// SSZ type resolution (copied exactly from ssz_go / ssz_nim)
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
  const StructDef *struct_def;              // for container elements
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
      : static_size(0),
        is_fixed(true),
        is_progressive(false),
        max_ssz_index(0) {}
};

// ---------------------------------------------------------------------------
// Lean identifier helpers
// ---------------------------------------------------------------------------

static std::set<std::string> LeanKeywords() {
  return {
      "abbrev",   "at",       "attribute", "axiom",    "begin",
      "by",       "calc",     "class",     "deriving", "def",
      "do",       "else",     "end",       "example",  "extends",
      "extern",   "from",     "fun",       "have",     "if",
      "import",   "in",       "inductive", "infix",    "infixl",
      "infixr",   "instance", "let",       "macro",    "match",
      "mutual",   "namespace","noncomputable", "notation", "open",
      "opaque",   "partial",  "prefix",    "private",  "prop",
      "protected","rec",      "section",   "set_option", "show",
      "sorry",    "structure","then",      "theorem",  "this",
      "type",     "universe", "unsafe",    "variable", "where",
      "with",
  };
}

// snake_case (or mixedCase) FlatBuffers name -> lowerCamelCase Lean field.
static std::string ToLowerCamel(const std::string &in) {
  std::string out;
  bool upper_next = false;
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c == '_') {
      upper_next = true;
      continue;
    }
    if (out.empty()) {
      out += static_cast<char>(tolower(c));
    } else if (upper_next) {
      out += static_cast<char>(toupper(c));
      upper_next = false;
    } else {
      out += c;
    }
  }
  return out;
}

// Wrap an identifier in guillemets if it collides with a Lean keyword.
static std::string EscapeIdent(const std::string &name) {
  static const std::set<std::string> kw = LeanKeywords();
  if (kw.count(name)) return "\xc2\xab" + name + "\xc2\xbb";  // «name»
  return name;
}

// ---------------------------------------------------------------------------
// Generator
// ---------------------------------------------------------------------------

class SszLeanGenerator : public BaseGenerator {
 public:
  SszLeanGenerator(const Parser &parser, const std::string &path,
                   const std::string &file_name)
      : BaseGenerator(parser, path, file_name, "", "", "lean") {}

  bool generate() override {
    std::vector<std::pair<const StructDef *, SszContainerInfo>> containers;

    for (auto it = parser_.structs_.vec.begin();
         it != parser_.structs_.vec.end(); ++it) {
      auto &struct_def = **it;
      if (struct_def.generated) continue;

      SszContainerInfo container;
      if (!AnalyzeContainer(struct_def, container)) return false;
      containers.push_back({ &struct_def, std::move(container) });
    }

    if (containers.empty()) return true;

    std::string code;
    for (size_t i = 0; i < containers.size(); i++) {
      if (!GenerateType(*containers[i].first, containers[i].second, &code)) {
        return false;
      }
      code += "\n";
    }

    return SaveFile(containers[0].first->defined_namespace, code);
  }

 private:
  // -----------------------------------------------------------------------
  // Type resolution (copied exactly from ssz_go / ssz_nim)
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
              "field '" + field.name +
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
            "field '" + field.name +
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

    if (attrs.Lookup("ssz_progressive_bitlist")) {
      info.ssz_type = SszType::Bitlist;
      info.limit = 0;
      info.fixed_size = 0;
      info.is_dynamic = true;
      return true;
    }

    if (attrs.Lookup("ssz_bitlist")) {
      auto max_attr = attrs.Lookup("ssz_max");
      if (!max_attr) {
        flatbuffers::LogCompilerError(
            "field '" + field.name +
            "': ssz_bitlist requires ssz_max attribute");
        return false;
      }
      info.ssz_type = SszType::Bitlist;
      info.limit = StringToUInt(max_attr->constant.c_str());
      info.fixed_size = 0;
      info.is_dynamic = true;
      return true;
    }

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
          "field '" + field.name +
          "': vector type requires ssz_max attribute");
      return false;
    }

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

    if (attrs.Lookup("ssz_bitvector")) {
      info.ssz_type = SszType::Bitvector;
      auto bitsize_attr = attrs.Lookup("ssz_bitsize");
      if (bitsize_attr) {
        info.bitsize =
            static_cast<uint32_t>(StringToUInt(bitsize_attr->constant.c_str()));
      } else {
        info.bitsize = array_len * 8;
      }
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
            "field '" + field.name +
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
        container.static_size += 4;
      } else {
        container.static_size += info.fixed_size;
      }

      container.field_infos.emplace(&field, std::move(info));
    }

    if (struct_def.attributes.Lookup("ssz_progressive")) {
      container.is_progressive = true;
    }

    return true;
  }

  // -----------------------------------------------------------------------
  // Lean type mapping
  // -----------------------------------------------------------------------

  // A FlatBuffers `struct` wrapping a single fixed byte array (the schema
  // idiom for embedding `[ubyte:N]` in a table) is an SSZ `Vector[uint8, N]`.
  // We emit it as an `abbrev` rather than a 1-field container: the two are
  // serialization- and hash-tree-root-identical (a single-field container's
  // root *is* its only field's root), and the alias matches how SizzLean's
  // own consensus types write `Bytes32`, `BLSPubkey`, etc.
  static bool IsByteVectorStruct(const StructDef &sd, uint32_t &len) {
    if (!sd.fixed) return false;
    if (sd.fields.vec.size() != 1) return false;
    const FieldDef &f = *sd.fields.vec[0];
    if (f.deprecated) return false;
    const Type &t = f.value.type;
    if (t.base_type != BASE_TYPE_ARRAY) return false;
    if (f.attributes.Lookup("ssz_bitvector")) return false;
    const Type elem = t.VectorType();
    if (elem.base_type != BASE_TYPE_UCHAR && elem.base_type != BASE_TYPE_CHAR)
      return false;
    len = t.fixed_length;
    return true;
  }

  // The Lean type for a field, e.g. "UInt64", "Vector UInt8 32",
  // "SSZList Withdrawal 16", "Bitlist 2048". Returns "" on an unsupported
  // shape (error already logged).
  std::string LeanTypeName(const SszFieldInfo &info) {
    switch (info.ssz_type) {
      case SszType::Bool: return "Bool";
      case SszType::Uint8: return "UInt8";
      case SszType::Uint16: return "UInt16";
      case SszType::Uint32: return "UInt32";
      case SszType::Uint64: return "UInt64";
      case SszType::Uint128: return "BitVec 128";
      case SszType::Uint256: return "BitVec 256";
      case SszType::Container:
        if (info.struct_def) return info.struct_def->name;
        flatbuffers::LogCompilerError("internal: container without struct_def");
        return "";
      case SszType::Vector:
        return "Vector " + LeanElemType(*info.elem_info) + " " +
               NumToString(info.limit);
      case SszType::List:
        return "SSZList " + LeanElemType(*info.elem_info) + " " +
               NumToString(info.limit);
      case SszType::Bitlist: return "Bitlist " + NumToString(info.limit);
      case SszType::Bitvector: return "Bitvector " + NumToString(info.bitsize);
      case SszType::Union:
        flatbuffers::LogCompilerError(
            "SSZ unions are not supported by --ssz-lean (SizzLean omits "
            "unions)");
        return "";
      case SszType::ProgressiveContainer:
      case SszType::ProgressiveList:
        flatbuffers::LogCompilerError(
            "progressive types (EIP-7495/7916) are not supported by "
            "--ssz-lean (SizzLean omits them)");
        return "";
    }
    return "";
  }

  // Parenthesize a compound type when it is nested as an element, e.g.
  // `SSZList (SSZList UInt8 1073741824) 1048576`.
  std::string LeanElemType(const SszFieldInfo &info) {
    std::string t = LeanTypeName(info);
    if (t.find(' ') != std::string::npos) return "(" + t + ")";
    return t;
  }

  bool GenerateType(const StructDef &sd, const SszContainerInfo &container,
                    std::string *code) {
    std::string &c = *code;

    uint32_t blen = 0;
    if (IsByteVectorStruct(sd, blen)) {
      c += "abbrev " + sd.name + " : Type := Vector UInt8 " +
           NumToString(blen) + "\n";
      return true;
    }

    if (container.is_progressive) {
      flatbuffers::LogCompilerError(
          "type '" + sd.name +
          "': ssz_progressive (EIP-7495) is not supported by --ssz-lean "
          "(SizzLean omits progressive containers)");
      return false;
    }

    // Align field types on the longest field name, SizzLean-style.
    size_t name_w = 0;
    std::vector<std::pair<std::string, std::string>> rows;
    for (auto *field : container.all_fields) {
      auto it = container.field_infos.find(field);
      const SszFieldInfo &info = it->second;
      std::string fname = EscapeIdent(ToLowerCamel(field->name));
      std::string ftype = LeanTypeName(info);
      if (ftype.empty()) return false;
      name_w = std::max(name_w, fname.size());
      rows.push_back({ fname, ftype });
    }

    c += "structure " + sd.name + " where\n";
    for (auto &row : rows) {
      c += "  " + row.first +
           std::string(name_w - row.first.size(), ' ') + " : " + row.second +
           "\n";
    }
    c += "  deriving SSZRepr\n";
    return true;
  }

  bool SaveFile(const Namespace *ns, const std::string &types) {
    std::string code =
        "-- Code generated by the FlatBuffers compiler. DO NOT EDIT.\n\n";
    code += "import SizzLean.Repr.Class\n";
    code += "import SizzLean.Repr.Instances\n";
    code += "import SizzLean.Repr.Deriving\n\n";
    code += "set_option autoImplicit false\n\n";
    code += "open SizzLean\n";
    code += "open SizzLean.Repr\n\n";

    std::string ns_name;
    if (ns && !ns->components.empty()) {
      for (size_t i = 0; i < ns->components.size(); ++i) {
        if (i) ns_name += ".";
        ns_name += ns->components[i];
      }
      code += "namespace " + ns_name + "\n\n";
    }

    code += types;

    if (!ns_name.empty()) code += "end " + ns_name + "\n";

    // Strip trailing blank lines down to a single newline.
    while (code.length() > 1 &&
           code.substr(code.length() - 2) == "\n\n") {
      code.pop_back();
    }

    EnsureDirExists(path_);
    std::string filename = path_ + file_name_ + "_ssz.lean";
    return parser_.opts.file_saver->SaveFile(filename.c_str(), code, false);
  }
};

}  // namespace ssz_lean

// ---------------------------------------------------------------------------
// Code generator wrapper
// ---------------------------------------------------------------------------

static bool GenerateSszLean(const Parser &parser, const std::string &path,
                            const std::string &file_name) {
  ssz_lean::SszLeanGenerator generator(parser, path, file_name);
  return generator.generate();
}

namespace {

class SszLeanCodeGenerator : public CodeGenerator {
 public:
  Status GenerateCode(const Parser &parser, const std::string &path,
                      const std::string &filename) override {
    if (!GenerateSszLean(parser, path, filename)) { return Status::ERROR; }
    return Status::OK;
  }

  Status GenerateCode(const uint8_t * /*buffer*/, int64_t /*length*/,
                      const CodeGenOptions & /*options*/) override {
    return Status::NOT_IMPLEMENTED;
  }

  Status GenerateMakeRule(const Parser & /*parser*/, const std::string & /*path*/,
                          const std::string & /*filename*/,
                          std::string & /*output*/) override {
    return Status::NOT_IMPLEMENTED;
  }

  Status GenerateGrpcCode(const Parser & /*parser*/, const std::string & /*path*/,
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

  IDLOptions::Language Language() const override { return IDLOptions::kSszLean; }

  std::string LanguageName() const override { return "SszLean"; }
};

}  // namespace

std::unique_ptr<CodeGenerator> NewSszLeanCodeGenerator() {
  return std::unique_ptr<SszLeanCodeGenerator>(new SszLeanCodeGenerator());
}

}  // namespace flatbuffers
