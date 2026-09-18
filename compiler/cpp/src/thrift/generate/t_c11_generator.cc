/* SPDX-License-Identifier: Apache-2.0 */
#include "thrift/generate/t_generator.h"
#include "thrift/platform.h"

#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <sys/stat.h>

/** Dependency-free C11 types and synchronous Binary/Compact RPC bindings.
 * Serialization is descriptor-driven so generated code shares runtime bounds,
 * ownership and wire validation with all other generated programs.
 */
class t_c11_generator : public t_generator {
public:
  t_c11_generator(t_program* program, const std::map<std::string, std::string>& options,
                  const std::string& option_string)
      : t_generator(program) {
    (void)option_string;
    out_dir_base_ = "gen-c11";
    for (const auto& option : options) {
      if (!option.second.empty())
        throw std::string("Unknown c11 option: ") + option.first;
      if (option.first == "server_stubs") server_stubs_ = true;
      else if (option.first == "flat_calls") flat_calls_ = true;
      else if (option.first == "manifest") manifest_ = true;
      else throw std::string("Unknown c11 option: ") + option.first;
    }
  }

  std::string display_name() const override { return "C11"; }

private:
  std::ostringstream header_;
  std::ostringstream source_;
  std::ostringstream client_header_;
  std::ostringstream client_source_;
  std::ostringstream server_header_;
  std::ostringstream server_source_;
  bool server_stubs_ = false;
  bool flat_calls_ = false;
  bool manifest_ = false;
  bool validation_only_ = false;
  std::set<std::string> public_symbols_;
  std::map<std::string, std::string> graph_symbols_;
  std::map<t_type*, std::string> containers_;
  std::set<std::string> symbols_;
  size_t next_type_ = 0;
  std::ostringstream manifest_source_;
  bool manifest_first_ = true;

  static std::string json_string(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
      switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20)
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c) << std::dec;
        else
          out << static_cast<char>(c);
      }
    }
    out << '"';
    return out.str();
  }

  // Machine-readable index from IDL names to generated C symbols: a tool (or an
  // LLM without the generated header in context) can look this up instead of
  // re-deriving the acronym-aware snake_case/prefix rules by hand. One flat JSON
  // array of objects; "idl" is a dotted IDL-name path, "kind" selects which of
  // the remaining keys are present.
  void manifest_entry(const std::string& idl, const std::string& kind,
                      const std::vector<std::pair<std::string, std::string>>& fields) {
    if (!manifest_ || validation_only_) return;
    manifest_source_ << (manifest_first_ ? "  {" : ",\n  {") << "\"idl\": " << json_string(idl)
                     << ", \"kind\": " << json_string(kind);
    for (const auto& field : fields)
      manifest_source_ << ", " << json_string(field.first) << ": " << json_string(field.second);
    manifest_source_ << "}";
    manifest_first_ = false;
  }

  static std::string snake(const std::string& input) {
    std::string result;
    for (size_t i = 0; i < input.size(); ++i) {
      const unsigned char c = static_cast<unsigned char>(input[i]);
      if (std::isupper(c) && i &&
          (std::islower(static_cast<unsigned char>(input[i - 1])) ||
           (i + 1 < input.size() && std::islower(static_cast<unsigned char>(input[i + 1])))))
        result += '_';
      result += std::isalnum(c) ? static_cast<char>(std::tolower(c)) : '_';
    }
    return result;
  }

  static std::string upper(const std::string& input) {
    std::string result;
    for (unsigned char c : input) result += static_cast<char>(std::toupper(c));
    return result;
  }

  static std::string prefix(t_program* program) {
    std::string ns = program->get_namespace("c11");
    return snake(ns.empty() ? program->get_name() : ns) + "_";
  }

  static std::string name(t_type* type) {
    return prefix(type->get_program()) + snake(type->get_name());
  }

  void reserve(const std::string& symbol, bool public_symbol = true) {
    if (!symbols_.insert(symbol).second)
      throw std::string("c11 identifier collision after normalization: ") + symbol;
    if (public_symbol) public_symbols_.insert(symbol);
    const auto external = graph_symbols_.find(symbol);
    if (external != graph_symbols_.end() && external->second != program_->get_path())
      throw std::string("c11 identifier collision with included program: ") + symbol;
  }

  static std::string literal(const std::string& value) {
    std::ostringstream out;
    out << '"';
    // Fixed-width octal escapes prevent adjacent digits from extending escapes.
    for (unsigned char c : value)
      out << '\\' << std::oct << std::setw(3) << std::setfill('0') << static_cast<unsigned>(c);
    out << '"';
    return out.str();
  }

  void documentation(const std::string& text, const std::string& indent = "", std::ostream* output = nullptr) {
    if (text.empty()) return;
    std::ostream& out = output ? *output : header_;
    // Never let documentation terminate the generated C comment.
    std::string safe = text;
    size_t position = 0;
    while ((position = safe.find("/*", position)) != std::string::npos) {
      safe.replace(position, 2, "/ *");
      position += 3;
    }
    position = 0;
    while ((position = safe.find("*/", position)) != std::string::npos) {
      safe.replace(position, 2, "* /");
      position += 3;
    }
    out << indent << "/**\n";
    std::istringstream lines(safe);
    std::string line;
    while (std::getline(lines, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      out << indent << " * " << line << '\n';
    }
    out << indent << " */\n";
  }

  static std::string integer(int64_t value) {
    if (value == std::numeric_limits<int64_t>::min())
      return "(-INT64_C(9223372036854775807) - 1)";
    if (value < 0)
      return "(-INT64_C(" + std::to_string(-value) + "))";
    return "INT64_C(" + std::to_string(value) + ")";
  }

  static std::string base_name(t_type* type) {
    switch (static_cast<t_base_type*>(type)->get_base()) {
    case t_base_type::TYPE_BOOL: return "bool";
    case t_base_type::TYPE_I8: return "byte";
    case t_base_type::TYPE_I16: return "i16";
    case t_base_type::TYPE_I32: return "i32";
    case t_base_type::TYPE_I64: return "i64";
    case t_base_type::TYPE_DOUBLE: return "double";
    case t_base_type::TYPE_STRING: return "string";
    case t_base_type::TYPE_UUID: return "uuid";
    default: throw std::string("c11: void is only supported as a method return type");
    }
  }

  static std::string c_type(t_type* type, bool preserve_alias = true) {
    // Forward references are parser placeholders, not emitted C typedefs.
    while (type->is_typedef() && static_cast<t_typedef*>(type)->is_forward_typedef())
      type = static_cast<t_typedef*>(type)->get_type();
    if (preserve_alias && type->is_typedef()) return name(type);
    type = type->get_true_type();
    if (type->is_enum()) return "enum " + name(type);
    if (type->is_struct() || type->is_xception()) return "struct " + name(type) + " *";
    if (type->is_map()) return "struct thrift_map";
    if (type->is_list() || type->is_set()) return "struct thrift_list";
    if (type->is_base_type()) {
      const std::string base = base_name(type);
      if (base == "string") return "struct thrift_bytes";
      if (base == "uuid") return "struct thrift_uuid";
      if (base == "byte") return "int8_t";
      if (base == "i16") return "int16_t";
      if (base == "i32") return "int32_t";
      if (base == "i64") return "int64_t";
      return base;
    }
    throw std::string("c11: unsupported type ") + type->get_name();
  }

  static t_type* element_type(t_type* type) {
    if (type->is_map()) return static_cast<t_map*>(type)->get_val_type();
    if (type->is_set()) return static_cast<t_set*>(type)->get_elem_type();
    return static_cast<t_list*>(type)->get_elem_type();
  }

  std::string descriptor(t_type* type) {
    type = type->get_true_type();
    if (type->is_enum()) return "thrift_type_i32";
    if (type->is_base_type()) return "thrift_type_" + base_name(type);
    if (type->is_struct() || type->is_xception()) return name(type) + "_type_descriptor";
    auto found = containers_.find(type);
    if (found != containers_.end()) return found->second;
    const std::string symbol = prefix(program_) + "container_" + std::to_string(next_type_++);
    reserve(symbol, false);
    const std::string element = descriptor(element_type(type));
    const std::string key = type->is_map() ? "&" + descriptor(static_cast<t_map*>(type)->get_key_type()) : "NULL";
    source_ << "static const struct thrift_type " << symbol << " = {\n    "
            << (type->is_map() ? "THRIFT_MAP" : type->is_set() ? "THRIFT_SET" : "THRIFT_LIST")
            << ", sizeof(" << c_type(type) << "), NULL, " << key << ", &" << element << "\n};\n\n";
    containers_[type] = symbol;
    return symbol;
  }

  static void validate_integer(t_type* type, int64_t value) {
    int64_t minimum = INT64_MIN, maximum = INT64_MAX;
    if (type->is_enum()) {
      minimum = INT32_MIN; maximum = INT32_MAX;
    } else {
      switch (static_cast<t_base_type*>(type)->get_base()) {
      case t_base_type::TYPE_BOOL: minimum = 0; maximum = 1; break;
      case t_base_type::TYPE_I8: minimum = INT8_MIN; maximum = INT8_MAX; break;
      case t_base_type::TYPE_I16: minimum = INT16_MIN; maximum = INT16_MAX; break;
      case t_base_type::TYPE_I32: minimum = INT32_MIN; maximum = INT32_MAX; break;
      default: break;
      }
    }
    if (value < minimum || value > maximum)
      throw std::string("c11: integer constant outside the range of ") + type->get_name();
  }

  static void validate_output_names(t_program* program, std::map<std::string, std::string>& outputs) {
    const std::string stem = snake(program->get_name());
    const auto inserted = outputs.emplace(stem, program->get_path());
    if (!inserted.second) {
      if (inserted.first->second != program->get_path())
        throw std::string("c11 output filename collision: ") + stem + "_types";
      return;
    }
    for (auto* included : program->get_includes())
      validate_output_names(included, outputs);
  }

  void collect_graph_symbols(t_program* program, std::set<std::string>& visited) {
    if (!visited.insert(program->get_path()).second) return;
    for (auto* included : program->get_includes()) collect_graph_symbols(included, visited);
    t_c11_generator check(program, {}, "");
    check.validation_only_ = true;
    check.server_stubs_ = server_stubs_;
    check.flat_calls_ = flat_calls_;
    check.generate_program();
    for (const auto& symbol : check.public_symbols_) {
      const auto inserted = graph_symbols_.emplace(symbol, program->get_path());
      if (!inserted.second)
        throw std::string("c11 identifier collision across included programs: ") + symbol;
    }
  }

  void init_generator() override {
    // Keep output in memory until all names and features have been validated.
    std::map<std::string, std::string> outputs;
    validate_output_names(program_, outputs);
    const std::string file_name = snake(program_->get_name());
    const std::string namespace_prefix = prefix(program_);
    if (file_name.empty() || !std::isalpha(static_cast<unsigned char>(file_name.front())) ||
        namespace_prefix.empty() || !std::isalpha(static_cast<unsigned char>(namespace_prefix.front())))
      throw std::string("c11: program and namespace must start with a letter: ") + program_->get_name();
    if (!validation_only_) {
      std::set<std::string> visited;
      collect_graph_symbols(program_, visited);
    }
    const std::string guard = upper(snake(program_->get_name()) + "_types_h");
    reserve(guard);
    if (!program_->get_services().empty()) {
      reserve(upper(file_name + "_client_h"));
      if (server_stubs_) reserve(upper(file_name + "_server_h"));
    }
    header_ << "/* Generated by the Apache Thrift C11 compiler. */\n#ifndef "
            << guard << "\n#define " << guard << "\n\n"
            << "#include <thrift/c11/processor/thrift_processor.h>\n";
    for (auto* included : program_->get_includes())
      header_ << "#include \"" << snake(included->get_name()) << "_types.h\"\n";
    header_ << '\n';
    // Doxygen needs a documented file to expose global functions and typedefs.
    documentation("@file\n" + program_->get_doc());
    for (auto* object : program_->get_objects())
      header_ << "struct " << name(object) << ";\n";
    header_ << '\n';
    source_ << "/* Generated by the Apache Thrift C11 compiler. */\n#include \""
            << snake(program_->get_name()) << "_types.h\"\n#include <stdlib.h>\n#include <string.h>\n\n";
  }

  void close_generator() override {
    if (validation_only_) return;
    header_ << "#endif\n";
    MKDIR(get_out_dir().c_str());
    const std::string stem = get_out_dir() + snake(program_->get_name()) + "_types";
    write_file(stem + ".h", header_.str());
    write_file(stem + ".c", source_.str());
    if (!program_->get_services().empty()) {
      const std::string base = snake(program_->get_name());
      const std::string client_stem = get_out_dir() + base + "_client";
      const std::string guard = upper(base + "_client_h");
      write_file(client_stem + ".h", "/** @file Generated synchronous clients. */\n#ifndef " + guard +
          "\n#define " + guard + "\n#include \"" + base + "_types.h\"\n\n" +
          client_header_.str() + "#endif\n");
      write_file(client_stem + ".c", "/* Generated clients; do not edit. */\n#include \"" +
          base + "_client.h\"\n\n" + client_source_.str());
    }
    if (server_stubs_ && !program_->get_services().empty()) {
      const std::string base = snake(program_->get_name());
      const std::string server_stem = get_out_dir() + base + "_server";
      const std::string guard = upper(base + "_server_h");
      write_file(server_stem + ".h", "/** @file Generated server declarations. */\n#ifndef " + guard +
          "\n#define " + guard + "\n#include \"" + base + "_types.h\"\n\n" +
          server_header_.str() + "#endif\n");
      // A skeleton becomes application-owned after its first generation.
      struct stat info;
      if (stat((server_stem + ".c").c_str(), &info) != 0) {
        if (errno != ENOENT) throw std::string("c11: cannot inspect ") + server_stem + ".c";
        write_file(server_stem + ".c", "/* Generated server skeleton; edit to implement your service.\n"
            " * Regeneration preserves this file. Merge IDL changes manually. */\n#include \"" +
            base + "_server.h\"\n\n" + server_source_.str());
      }
    }
    if (manifest_) {
      const std::string manifest_stem = get_out_dir() + snake(program_->get_name()) + "_manifest";
      write_file(manifest_stem + ".json", "[\n" + manifest_source_.str() + "\n]\n");
    }
  }

  static void write_file(const std::string& path, const std::string& contents) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << contents;
    file.close();
    if (!file)
      throw std::string("c11: cannot write ") + path;
  }

  void generate_typedef(t_typedef* type) override {
    reserve(name(type));
    const std::string desc = descriptor(type->get_type());
    reserve(name(type) + "_clear");
    // Flatten alias definitions so forward alias chains need no declaration ordering.
    documentation(type->get_doc());
    header_ << "typedef " << c_type(type->get_type(), false) << " " << name(type) << ";\n"
            << "void " << name(type) << "_clear(" << name(type) << " *value);\n\n";
    source_ << "void " << name(type) << "_clear(" << name(type) << " *value)\n{\n"
            << "    thrift_value_clear(&" << desc << ", value);\n}\n\n";
    manifest_entry(type->get_name(), "typedef", {{"c_type", c_type(type, false)}, {"c_name", name(type)}});
  }

  void generate_enum(t_enum* type) override {
    const std::string symbol = name(type);
    const std::string width = upper(symbol + "_thrift_reserved_width");
    reserve(symbol);
    reserve(width);
    documentation(type->get_doc());
    manifest_entry(type->get_name(), "enum", {{"c_name", "enum " + symbol}});
    header_ << "enum " << symbol << " {\n";
    for (auto* value : type->get_constants()) {
      const std::string member = upper(symbol + "_" + snake(value->get_name()));
      reserve(member);
      documentation(value->get_doc(), "    ");
      manifest_entry(type->get_name() + "." + value->get_name(), "enum_value", {{"c_name", member}});
      // MSVC treats the positive magnitude of INT32_MIN as unsigned.
      const std::string number = value->get_value() == std::numeric_limits<int32_t>::min()
          ? "(-2147483647 - 1)" : std::to_string(value->get_value());
      header_ << "    " << member << " = " << number << ",\n";
    }
    header_ << "    " << width << " = 0x7fffffff /**< Reserved width sentinel; do not use. */\n};\n"
            << "_Static_assert(sizeof(enum " << symbol << ") == sizeof(int32_t),\n"
            << "    \"c11: enum must be 4 bytes wide\");\n\n";
  }

  // Emit initialization into a zeroed destination. The enclosing function owns
  // cleanup; every fallible operation returns immediately on failure.
  void constant_value(std::ostream& out, t_type* type, t_const_value* value, const std::string& target,
                      const std::string& budget = "NULL") {
    type = type->get_true_type();
    if (type->is_enum() || (type->is_base_type() && !type->is_string() && !type->is_uuid())) {
      std::string text;
      if (type->is_base_type() && static_cast<t_base_type*>(type)->get_base() == t_base_type::TYPE_DOUBLE) {
        const double numeric = value->get_type() == t_const_value::CV_INTEGER
            ? static_cast<double>(value->get_integer()) : value->get_double();
        if (!std::isfinite(numeric))
          throw std::string("c11: non-finite floating point constant");
        std::ostringstream number;
        number << std::setprecision(std::numeric_limits<double>::max_digits10)
               << numeric;
        text = number.str();
        if (text.find_first_of(".eE") == std::string::npos) text += ".0";
      } else {
        validate_integer(type, value->get_integer());
        text = integer(value->get_integer());
      }
      out << "    " << target << " = " << text << ";\n";
    } else if (type->is_string()) {
      out << "    status = thrift_bytes_set_limited(&(" << target << "), " << literal(value->get_string())
          << ", " << value->get_string().size() << ", " << budget << ");\n"
          << "    if (status != THRIFT_OK) return status;\n";
    } else if (type->is_uuid()) {
      std::string hex;
      for (char c : value->get_uuid()) if (c != '-') hex += c;
      for (size_t i = 0; i < 16; ++i)
        out << "    (" << target << ").data[" << i << "] = 0x" << hex.substr(i * 2, 2) << ";\n";
    } else if (type->is_struct() || type->is_xception()) {
      allocation(out, target, 1, "struct " + name(type), budget);
      out << "    status = thrift_record_init_limited(&" << name(type) << "_record, " << target << ", " << budget << ");\n"
          << "    if (status != THRIFT_OK) return status;\n";
      auto* record = static_cast<t_struct*>(type);
      if (record->is_union())
        out << "    " << name(type) << "_clear(" << target << ");\n";
      for (const auto& entry : value->get_map()) {
        auto* field = record->get_field_by_name(entry.first->get_string());
        if (!field) throw std::string("c11: unknown constant field");
        const std::string member = "(" + target + ")->f_" + snake(field->get_name());
        out << "    thrift_value_clear(&" << descriptor(field->get_type()) << ", &(" << member << "));\n";
        constant_value(out, field->get_type(), entry.second, member, budget);
        out << "    (" << target << ")->has_" << snake(field->get_name()) << " = true;\n";
      }
    } else {
      const bool is_map = type->is_map();
      const size_t count = is_map ? value->get_map().size() : value->get_list().size();
      t_type* element = element_type(type);
      out << "    (" << target << ").size = " << count << ";\n";
      if (!count) return;
      if (is_map) {
        auto* key = static_cast<t_map*>(type)->get_key_type();
        allocation(out, target + ".keys", count, c_type(key), budget);
        allocation(out, target + ".values", count, c_type(element), budget);
        size_t i = 0;
        for (const auto& entry : value->get_map()) {
          constant_value(out, key, entry.first, "((" + c_type(key) + " *)(" + target + ").keys)[" + std::to_string(i) + "]", budget);
          constant_value(out, element, entry.second, "((" + c_type(element) + " *)(" + target + ").values)[" + std::to_string(i++) + "]", budget);
        }
      } else {
        allocation(out, target + ".data", count, c_type(element), budget);
        out << "    (" << target << ").capacity = " << count << ";\n";
        size_t i = 0;
        for (auto* entry : value->get_list())
          constant_value(out, element, entry, "((" + c_type(element) + " *)(" + target + ").data)[" + std::to_string(i++) + "]", budget);
      }
    }
  }

  static void allocation(std::ostream& out, const std::string& target, size_t count,
                         const std::string& type, const std::string& budget) {
    out << "    {\n        void *allocation = NULL;\n"
        << "        status = thrift_alloc(" << count << ", sizeof(" << type << "), " << budget << ", &allocation);\n"
        << "        if (status != THRIFT_OK) return status;\n"
        << "        " << target << " = allocation;\n    }\n";
  }

  void generate_const(t_const* constant) override {
    const std::string symbol = prefix(program_) + "const_" + snake(constant->get_name());
    reserve(symbol);
    reserve(symbol + "_get");
    reserve(symbol + "_build", false);
    reserve(symbol + "_clear");
    const std::string type = c_type(constant->get_type());
    const std::string desc = descriptor(constant->get_type());
    std::ostringstream body;
    constant_value(body, constant->get_type(), constant->get_value(), "(*value)");
    documentation(constant->get_doc() + "\nReplace an initialized value with an owned copy of this constant.");
    manifest_entry(constant->get_name(), "const",
        {{"stem", symbol}, {"getter", symbol + "_get"}, {"clear", symbol + "_clear"}});
    header_ << "enum thrift_status " << symbol << "_get(" << type << " *value);\n"
            << "void " << symbol << "_clear(" << type << " *value);\n\n";
    source_ << "void " << symbol << "_clear(" << type << " *value)\n{\n"
            << "    thrift_value_clear(&" << desc << ", value);\n}\n\n";
    source_ << "static enum thrift_status " << symbol << "_build(" << type << " *value)\n{\n"
            << "    enum thrift_status status = THRIFT_OK;\n" << body.str()
            << "    return status;\n}\n\n"
            << "enum thrift_status " << symbol << "_get(" << type << " *value)\n{\n"
            << "    " << type << " temporary = {0};\n    enum thrift_status status;\n"
            << "    if (!value) return THRIFT_NULL_VALUE;\n"
            << "    status = " << symbol << "_build(&temporary);\n"
            << "    if (status != THRIFT_OK) {\n        thrift_value_clear(&" << desc << ", &temporary);\n"
            << "        return status;\n    }\n    thrift_value_clear(&" << desc << ", value);\n"
            << "    *value = temporary;\n    return THRIFT_OK;\n}\n\n";
  }

  void generate_struct(t_struct* object) override {
    documentation(object->get_doc());
    const std::string kind = object->is_xception() ? "exception" : object->is_union() ? "union" : "struct";
    manifest_entry(object->get_name(), kind, {{"c_name", name(object)}});
    for (auto* field : object->get_members())
      manifest_entry(object->get_name() + "." + field->get_name(), "field",
          {{"parent", name(object)}, {"member", "f_" + snake(field->get_name())},
           {"setter", name(object) + "_set_" + snake(field->get_name())}});
    record(name(object), object->get_members(), object->is_union());
  }

  void record_helpers(const std::string& symbol, const std::vector<t_field*>& fields,
                      const std::vector<std::string>& descriptors, bool exclusive) {
    documentation("Allocate and initialize an owned record, charging all allocations to the optional budget.\n"
                  "out must point to NULL; failure preserves it, but consumed budget is not refunded.\n"
                  "Returns THRIFT_NULL_VALUE, THRIFT_OUTPUT_NOT_EMPTY, THRIFT_LIMIT or THRIFT_NOMEM on failure.\n"
                  "Release with _clear() followed by free().");
    header_ << "enum thrift_status " << symbol << "_create(struct " << symbol << " **out, size_t *budget);\n";
    source_ << "enum thrift_status " << symbol << "_create(struct " << symbol << " **out, size_t *budget)\n{\n"
            << "    void *allocation = NULL;\n    enum thrift_status status;\n"
            << "    if (!out) return THRIFT_NULL_VALUE;\n"
            << "    if (*out) return THRIFT_OUTPUT_NOT_EMPTY;\n"
            << "    status = thrift_alloc(1, sizeof(struct " << symbol << "), budget, &allocation);\n"
            << "    if (status != THRIFT_OK) return status;\n"
            << "    status = thrift_record_init_limited(&" << symbol << "_record, allocation, budget);\n"
            << "    if (status != THRIFT_OK) {\n        free(allocation);\n        return status;\n    }\n"
            << "    *out = allocation;\n    return THRIFT_OK;\n}\n\n";
    documentation("Allocate with defaults; returns NULL on failure. Use _create() for an exact status/budget.\n"
                  "Release with _clear() followed by free().");
    header_ << "struct " << symbol << " *" << symbol << "_new(void);\n";
    source_ << "struct " << symbol << " *" << symbol << "_new(void)\n{\n"
            << "    struct " << symbol << " *value = NULL;\n"
            << "    if (" << symbol << "_create(&value, NULL) != THRIFT_OK) return NULL;\n"
            << "    return value;\n}\n\n";
    if (exclusive) {
      reserve(symbol + "_case");
      reserve(upper(symbol + "_case_none"));
      reserve(upper(symbol + "_case_invalid"));
      documentation("Active alternative. INVALID denotes NULL or multiple manually activated fields.");
      header_ << "enum " << symbol << "_case {\n    " << upper(symbol) << "_CASE_INVALID = -1,\n    "
              << upper(symbol) << "_CASE_NONE = 0";
      for (size_t i = 0; i < fields.size(); ++i) {
        const std::string alternative = upper(symbol + "_case_field_" + snake(fields[i]->get_name()));
        reserve(alternative);
        header_ << ",\n    " << alternative << " = " << i + 1;
      }
      header_ << "\n};\n";
      documentation("Return the active alternative without allocating or modifying the record.");
      header_ << "enum " << symbol << "_case " << symbol << "_case(const struct " << symbol << " *value);\n";
      source_ << "enum " << symbol << "_case " << symbol << "_case(const struct " << symbol << " *value)\n{\n"
              << "    enum " << symbol << "_case selected = " << upper(symbol) << "_CASE_NONE;\n"
              << "    if (!value) return " << upper(symbol) << "_CASE_INVALID;\n";
      for (auto* field : fields) {
        const std::string member = snake(field->get_name());
        source_ << "    if (value->has_" << member << ") {\n        if (selected != " << upper(symbol)
                << "_CASE_NONE) return " << upper(symbol) << "_CASE_INVALID;\n        selected = "
                << upper(symbol + "_case_field_" + member) << ";\n    }\n";
      }
      source_ << "    return selected;\n}\n\n";
    }
    for (size_t i = 0; i < fields.size(); ++i) {
      auto* field = fields[i];
      auto* type = field->get_type()->get_true_type();
      const bool bytes = type->is_string();
      const bool pointer = type->is_struct() || type->is_xception();
      const bool container = type->is_list() || type->is_set() || type->is_map();
      const bool inline_setter = !exclusive && !bytes && !pointer && !container;
      const std::string member = snake(field->get_name());
      const std::string setter = symbol + "_set_" + member;
      const std::string target = "value->f_" + member;
      reserve(setter);
      std::string doc = "Assign the field and mark it present. THRIFT_NULL_VALUE denotes a NULL destination.\n";
      if (bytes)
        doc += "Copies size bytes; nonempty NULL input returns THRIFT_NULL_DATA. May return THRIFT_LIMIT or THRIFT_NOMEM.\n"
               "Failure preserves the record; source bytes may overlap its owned storage.\n";
      if (pointer)
        doc += "Takes ownership of an unshared, free()-compatible pointer on success. NULL returns THRIFT_NULL_FIELD.\n"
               "Direct self-ownership returns THRIFT_INVALID_OWNERSHIP; releases the previous value on success.\n"
               "Reassigning the same pointer is safe. Descendant aliases are not allowed.\n";
      if (container)
        doc += "Moves an owned container and zeroes the source on success; releases the previous value.\n"
               "NULL source returns THRIFT_NULL_FIELD; invalid storage/capacity/ownership has a specific status.\n"
               "Self-move is safe. Shared storage and descendant aliases are not allowed.\n";
      if (exclusive) doc += "On success clears every other alternative without reapplying defaults.\n";
      documentation(doc);
      const std::string signature = "enum thrift_status " + setter + "(struct " + symbol + " *value, " +
          (bytes ? "const void *data, size_t size" : c_type(field->get_type()) + (container ? " *" : " ") + "field") + ")";
      if (!inline_setter) header_ << signature << ";\n";
      std::ostream& out = inline_setter ? static_cast<std::ostream&>(header_) : static_cast<std::ostream&>(source_);
      if (inline_setter) out << "static inline ";
      out << signature << "\n{\n";
      if (bytes) {
        out << "    enum thrift_status status;\n";
        if (exclusive) out << "    struct thrift_bytes copy = {0};\n";
        out << "    if (!value) return THRIFT_NULL_VALUE;\n"
            << "    if (!data && size) return THRIFT_NULL_DATA;\n"
            << "    status = thrift_bytes_set(" << (exclusive ? "&copy" : "&" + target) << ", data, size);\n"
            << "    if (status != THRIFT_OK) return status;\n";
        if (exclusive)
          out << "    " << symbol << "_clear(value);\n    " << target << " = copy;\n";
      } else {
        if (container) out << "    " << c_type(field->get_type()) << " moved;\n";
        out << "    if (!value) return THRIFT_NULL_VALUE;\n";
        if (pointer || container) out << "    if (!field) return THRIFT_NULL_FIELD;\n";
        if (pointer)
          out << "    if ((const void *)field == (const void *)value) return THRIFT_INVALID_OWNERSHIP;\n";
        if (container) {
          const std::string slot = "sizeof(" + c_type(element_type(type)) + ")";
          out << "    if (field->size > INT32_MAX || field->size > SIZE_MAX / " << slot << ") return THRIFT_LIMIT;\n";
          if (type->is_map()) {
            out << "    if (field->size > SIZE_MAX / sizeof(" << c_type(static_cast<t_map*>(type)->get_key_type())
                << ")) return THRIFT_LIMIT;\n"
                << "    if (field->size && !field->keys) return THRIFT_NULL_MAP_KEYS;\n"
                << "    if (field->size && !field->values) return THRIFT_NULL_MAP_VALUES;\n"
                << "    if (field->keys && field->keys == field->values) return THRIFT_INVALID_OWNERSHIP;\n"
                << "    if (field != &" << target << " && ((field->keys && (field->keys == " << target
                << ".keys || field->keys == " << target << ".values)) || (field->values && (field->values == "
                << target << ".keys || field->values == " << target << ".values)))) return THRIFT_INVALID_OWNERSHIP;\n";
          } else {
            out << "    if (field->capacity > INT32_MAX || field->capacity > SIZE_MAX / " << slot << ") return THRIFT_LIMIT;\n"
                << "    if (field->capacity && field->size > field->capacity) return THRIFT_INVALID_CAPACITY;\n"
                << "    if ((field->size || field->capacity) && !field->data) return THRIFT_NULL_DATA;\n"
                << "    if (field != &" << target << " && field->data && field->data == " << target << ".data) return THRIFT_INVALID_OWNERSHIP;\n";
          }
        }
        if (container)
          out << "    moved = *field;\n    memset(field, 0, sizeof(*field));\n";
        if (exclusive) {
          if (pointer) out << "    if (" << target << " == field) " << target << " = NULL;\n";
          out << "    " << symbol << "_clear(value);\n";
        } else if (pointer) {
          out << "    if (" << target << " != field) thrift_value_clear(&" << descriptors[i] << ", &" << target << ");\n";
        } else if (container) {
          out << "    thrift_value_clear(&" << descriptors[i] << ", &" << target << ");\n";
        }
        out << "    " << target << " = " << (container ? "moved" : "field") << ";\n";
      }
      out << "    value->has_" << member << " = true;\n    return THRIFT_OK;\n}\n\n";
    }
  }

  void record(const std::string& symbol, const std::vector<t_field*>& fields, bool exclusive) {
    reserve(symbol);
    for (const auto& suffix : {"_record", "_type_descriptor", "_new", "_create", "_init", "_clear", "_read", "_write"})
      reserve(symbol + suffix);
    reserve(symbol + "_fields", false);
    reserve(symbol + "_defaults", false);
    std::set<std::string> members;
    std::set<int32_t> field_ids;
    header_ << "struct " << symbol << " {\n";
    if (fields.empty()) header_ << "    uint8_t unused;\n";
    for (auto* field : fields) {
      if (!field_ids.insert(field->get_key()).second)
        throw std::string("c11: duplicate field ID in ") + symbol + ": " + std::to_string(field->get_key());
      if (field->get_key() < std::numeric_limits<int16_t>::min() ||
          field->get_key() > std::numeric_limits<int16_t>::max())
        throw std::string("c11: field ID outside int16 range in ") + symbol + "." +
            field->get_name() + ": " + std::to_string(field->get_key());
      const std::string member = snake(field->get_name());
      if (!members.insert(member).second)
        throw std::string("c11 field collision in ") + symbol + ": " + member;
      documentation(field->get_doc(), "    ");
      header_ << "    " << c_type(field->get_type()) << " f_" << member << ";\n"
              << "    bool has_" << member << ";\n";
    }
    header_ << "};\nextern const struct thrift_record " << symbol << "_record;\n"
            << "extern const struct thrift_type " << symbol << "_type_descriptor;\n"
            << "enum thrift_status " << symbol << "_init(struct " << symbol << " *value);\n"
            << "void " << symbol << "_clear(struct " << symbol << " *value);\n"
            << "enum thrift_status " << symbol << "_read(struct thrift_protocol *protocol, struct " << symbol << " *value);\n"
            << "enum thrift_status " << symbol << "_write(struct thrift_protocol *protocol, const struct " << symbol << " *value);\n\n";
    std::vector<std::string> descriptors;
    std::ostringstream defaults;
    for (auto* field : fields) {
      descriptors.push_back(descriptor(field->get_type()));
      if (field->get_value()) {
        constant_value(defaults, field->get_type(), field->get_value(), "value->f_" + snake(field->get_name()), "allocation_budget");
        defaults << "    value->has_" << snake(field->get_name()) << " = true;\n";
      }
    }
    record_helpers(symbol, fields, descriptors, exclusive);
    if (!defaults.str().empty())
      source_ << "static enum thrift_status " << symbol << "_defaults(void *object, size_t *allocation_budget)\n{\n"
              << "    struct " << symbol << " *value = object;\n"
              << "    enum thrift_status status = THRIFT_OK;\n    (void)allocation_budget;\n" << defaults.str()
              << "    return status;\n}\n\n";
    if (!fields.empty()) {
      source_ << "static const struct thrift_field " << symbol << "_fields[] = {\n";
      for (size_t i = 0; i < fields.size(); ++i) {
        auto* field = fields[i];
        const std::string member = snake(field->get_name());
        source_ << "    {" << field->get_key() << ", offsetof(struct " << symbol << ", f_" << member
                << "), offsetof(struct " << symbol << ", has_" << member << "), "
                << (!exclusive && field->get_req() == t_field::T_REQUIRED ? "true" : "false") << ", "
                << (exclusive || field->get_req() == t_field::T_OPTIONAL ? "true" : "false")
                << ", &" << descriptors[i] << "},\n";
      }
      source_ << "};\n";
    }
    source_ << "const struct thrift_record " << symbol << "_record = {\n    sizeof(struct " << symbol << "), "
            << fields.size() << ", " << (fields.empty() ? "NULL" : symbol + "_fields") << ", "
            << (exclusive ? "true" : "false") << ", " << (defaults.str().empty() ? "NULL" : symbol + "_defaults") << "\n};\n"
            << "const struct thrift_type " << symbol << "_type_descriptor = {\n    THRIFT_STRUCT, sizeof(struct " << symbol
            << " *), &" << symbol << "_record, NULL, NULL\n};\n\n"
            << "enum thrift_status " << symbol << "_init(struct " << symbol << " *value)\n{\n"
            << "    if (!value) return THRIFT_NULL_VALUE;\n"
            << "    return thrift_record_init(&" << symbol << "_record, value);\n}\n"
            << "void " << symbol << "_clear(struct " << symbol << " *value)\n{\n"
            << "    thrift_record_clear(&" << symbol << "_record, value);\n}\n"
            << "enum thrift_status " << symbol << "_read(struct thrift_protocol *protocol, struct " << symbol << " *value)\n{\n"
            << "    if (!protocol) return THRIFT_NULL_PROTOCOL;\n"
            << "    if (!protocol->ops) return THRIFT_PROTOCOL_UNINITIALIZED;\n"
            << "    if (!value) return THRIFT_NULL_VALUE;\n"
            << "    return thrift_record_read(protocol, &" << symbol << "_record, value);\n}\n"
            << "enum thrift_status " << symbol << "_write(struct thrift_protocol *protocol, const struct " << symbol << " *value)\n{\n"
            << "    if (!protocol) return THRIFT_NULL_PROTOCOL;\n"
            << "    if (!protocol->ops) return THRIFT_PROTOCOL_UNINITIALIZED;\n"
            << "    if (!value) return THRIFT_NULL_VALUE;\n"
            << "    return thrift_record_write(protocol, &" << symbol << "_record, value);\n}\n\n";
  }

  struct method_entry { t_service* owner; t_function* method; };
  static void methods(t_service* service, std::vector<method_entry>& result) {
    if (service->get_extends()) methods(service->get_extends(), result);
    for (auto* method : service->get_functions()) result.push_back({service, method});
  }

  void client_api(t_service* service, const std::vector<method_entry>& entries) {
    const std::string symbol = name(service);
    const std::string client = symbol + "_client";
    reserve(client);
    reserve(client + "_init");
    documentation(service->get_doc() + "\nSynchronous stack-allocated client. Borrows its protocol and transport.\n"
                  "Keep both alive for all calls; serialize access to a shared client/protocol.\n"
                  "The client neither connects nor closes the transport. Reconnect after transport/protocol errors.",
                  "", &client_header_);
    client_header_ << "struct " << client << " {\n"
                   << "    struct thrift_protocol *protocol; /**< Borrowed initialized protocol. */\n"
                   << "    int32_t next_sequence; /**< Internal counter; do not modify. */\n};\n"
                   << "/** Bind an initialized protocol and start sequence IDs at 1.\n"
                   << " * Returns THRIFT_NULL_CLIENT, THRIFT_NULL_PROTOCOL or THRIFT_PROTOCOL_UNINITIALIZED; failure leaves the client unchanged. */\n"
                   << "enum thrift_status " << client << "_init(struct " << client << " *client, struct thrift_protocol *protocol);\n\n";
    client_source_ << "enum thrift_status " << client << "_init(struct " << client << " *client, struct thrift_protocol *protocol)\n{\n"
                   << "    if (!client) return THRIFT_NULL_CLIENT;\n"
                   << "    if (!protocol) return THRIFT_NULL_PROTOCOL;\n"
                   << "    if (!protocol->ops) return THRIFT_PROTOCOL_UNINITIALIZED;\n"
                   << "    client->protocol = protocol;\n    client->next_sequence = 1;\n    return THRIFT_OK;\n}\n\n";
    for (const auto& entry : entries) {
      const std::string member = snake(entry.method->get_name());
      const std::string call = client + "_" + member + "_call";
      const std::string method = name(entry.owner) + "_" + member;
      reserve(call);
      documentation(entry.method->get_doc() + "\nPerform a synchronous RPC with an automatic sequence ID (wraps INT32_MAX to 1).\n"
                    "Args are borrowed. Initialize result to zero or a valid owned value; clear it after use.\n"
                    "A declared exception returns THRIFT_OK: inspect the result case.\n"
                    "Returns the RPC status; after I/O or protocol failure discard the connection.\n"
                    "Validation returns THRIFT_NULL_CLIENT, THRIFT_NULL_PROTOCOL, THRIFT_PROTOCOL_UNINITIALIZED,\n"
                    "THRIFT_NULL_ARGS, THRIFT_NULL_RESULT or THRIFT_INVALID_SEQUENCE for the first failed check.\n"
                    "Invalid arguments do not consume a sequence; each attempted RPC does.\n" +
                    (entry.method->is_oneway() ? "Oneway: result may be NULL; no response is read." : "A non-NULL result is required."),
                    "", &client_header_);
      const std::string signature = "enum thrift_status " + call + "(struct " + client + " *client,\n    const struct " +
          method + "_args *args, struct " + method + "_result *result)";
      client_header_ << signature << ";\n\n";
      client_source_ << signature << "\n{\n    int32_t sequence;\n"
                     << "    if (!client) return THRIFT_NULL_CLIENT;\n"
                     << "    if (!client->protocol) return THRIFT_NULL_PROTOCOL;\n"
                     << "    if (!client->protocol->ops) return THRIFT_PROTOCOL_UNINITIALIZED;\n"
                     << "    if (!args) return THRIFT_NULL_ARGS;\n"
                     << (entry.method->is_oneway() ? "" : "    if (!result) return THRIFT_NULL_RESULT;\n")
                     << "    if (client->next_sequence < 1) return THRIFT_INVALID_SEQUENCE;\n"
                     << "    sequence = client->next_sequence;\n"
                     << "    client->next_sequence = sequence == INT32_MAX ? 1 : sequence + 1;\n"
                     << "    return " << symbol << "_" << member << "_call(client->protocol, sequence, args, result);\n}\n\n";
    }
  }

  // Shared eligibility rule for flat_calls, client and server sides alike:
  // no optional argument (wire presence has no positional representation) and
  // no string/list/set/map success type (transferring ownership out of those
  // without a full record_clear() needs per-field zeroing neither side attempts).
  // Both are narrowing, not correctness, limits; ineligible methods keep using
  // the args/result record functions, which flat_calls never removes.
  // Empty string means eligible; otherwise the reason, for flat_ineligible_warning().
  static std::string flat_ineligibility(t_function* method) {
    for (auto* field : method->get_arglist()->get_members())
      if (field->get_req() == t_field::T_OPTIONAL)
        return "optional argument '" + field->get_name() + "' has no positional presence representation";
    t_type* returntype = method->get_returntype();
    if (!returntype->is_void()) {
      auto* true_type = returntype->get_true_type();
      if (true_type->is_string() || true_type->is_list() || true_type->is_set() || true_type->is_map())
        return "success type is string/list/set/map";
    }
    return "";
  }

  static bool flat_eligible(t_function* method) {
    return flat_ineligibility(method).empty();
  }

  void flat_client_api(t_service* service, const std::vector<method_entry>& entries) {
    const std::string symbol = name(service);
    const std::string client = symbol + "_client";
    for (const auto& entry : entries) {
      auto* method = entry.method;
      if (!flat_eligible(method)) continue;
      const std::vector<t_field*>& arg_fields = method->get_arglist()->get_members();
      const bool oneway = method->is_oneway();
      t_type* returntype = method->get_returntype();
      const bool has_return = !returntype->is_void();
      t_type* return_true = has_return ? returntype->get_true_type() : nullptr;
      const std::vector<t_field*>& exceptions = method->get_xceptions()->get_members();
      const std::string member = snake(method->get_name());
      const std::string flat = client + "_" + member;
      const std::string record_symbol = name(entry.owner) + "_" + member;
      reserve(flat);
      // A pointer c_type() (struct/xception) already ends in " *"; appending a
      // bare "*" avoids "struct X * *out" double-pointer spacing.
      auto out_param = [](const std::string& type_str) {
        return type_str + (type_str.back() == '*' ? "*" : " *");
      };
      // Index-qualified argument names cannot collide with keywords, locals,
      // outputs, or another argument's generated data/size suffix.
      std::vector<std::string> argument_names;
      std::vector<std::string> outputs;
      std::vector<std::string> owned_outputs;
      std::vector<std::string> parameters = {"struct " + client + " *client"};
      for (size_t i = 0; i < arg_fields.size(); ++i) {
        auto* field = arg_fields[i];
        auto* type = field->get_type()->get_true_type();
        const std::string argument = "arg_" + std::to_string(i + 1) + "_" + snake(field->get_name());
        argument_names.push_back(argument);
        if (type->is_string()) {
          parameters.push_back("const void *" + argument + "_data");
          parameters.push_back("size_t " + argument + "_size");
        } else {
          parameters.push_back(c_type(field->get_type()) + " " + argument);
        }
      }
      if (!oneway) {
        if (has_return) {
          parameters.push_back(out_param(c_type(returntype)) + "out_success");
          outputs.push_back("out_success");
          if (return_true->is_struct() || return_true->is_xception())
            owned_outputs.push_back("out_success");
        }
        for (auto* field : exceptions) {
          const std::string output = "out_" + snake(field->get_name());
          parameters.push_back(out_param(c_type(field->get_type())) + output);
          outputs.push_back(output);
          owned_outputs.push_back(output);
        }
      }
      std::ostringstream signature;
      signature << "enum thrift_status " << flat << "(";
      for (size_t i = 0; i < parameters.size(); ++i)
        signature << (i ? ",\n    " : "") << parameters[i];
      signature << ")";
      std::string doc = "Flattened synchronous call: one parameter per IDL argument instead of\n"
          "an args record, one output pointer per success/declared exception instead\n"
          "of a result record. Not generated for methods with an optional argument or\n"
          "a string/list/set/map success type; use " + flat + "_call for those.\n"
          "Arguments are borrowed for the duration of the call; no ownership is taken.\n";
      if (!oneway)
        doc += "Outputs must be non-NULL, nonoverlapping, and written only on THRIFT_OK.\n"
               "Struct/exception output slots must initially contain NULL; otherwise returns\n"
               "THRIFT_OUTPUT_NOT_EMPTY before I/O. Equal output addresses return THRIFT_INVALID_OWNERSHIP.\n"
               "On THRIFT_OK, inactive outputs are zero/NULL and active owned outputs transfer\n"
               "ownership to the caller. Clear and free owned outputs before reuse.\n"
               "An empty void result is successful. Failure preserves every output.\n";
      documentation(doc, "", &client_header_);
      client_header_ << signature.str() << ";\n\n";
      client_source_ << signature.str() << "\n{\n"
                     << "    struct " << record_symbol << "_args args = {0};\n";
      if (!oneway)
        client_source_ << "    struct " << record_symbol << "_result result = {0};\n";
      client_source_ << "    enum thrift_status status;\n";
      for (const auto& output : outputs)
        client_source_ << "    if (!" << output << ") return THRIFT_NULL_VALUE;\n";
      for (size_t i = 0; i < outputs.size(); ++i)
        for (size_t j = i + 1; j < outputs.size(); ++j)
          client_source_ << "    if ((const void *)" << outputs[i] << " == (const void *)"
                         << outputs[j] << ") return THRIFT_INVALID_OWNERSHIP;\n";
      for (const auto& output : owned_outputs)
        client_source_ << "    if (*" << output << ") return THRIFT_OUTPUT_NOT_EMPTY;\n";
      for (size_t i = 0; i < arg_fields.size(); ++i) {
        auto* field = arg_fields[i];
        auto* type = field->get_type()->get_true_type();
        const std::string fname = snake(field->get_name());
        const std::string& argument = argument_names[i];
        if (type->is_string())
          client_source_ << "    args.f_" << fname << ".data = (uint8_t *)(uintptr_t)" << argument << "_data;\n"
                         << "    args.f_" << fname << ".size = " << argument << "_size;\n";
        else
          client_source_ << "    args.f_" << fname << " = " << argument << ";\n";
        client_source_ << "    args.has_" << fname << " = true;\n";
      }
      client_source_ << "    status = " << flat << "_call(client, &args, "
                     << (oneway ? "NULL" : "&result") << ");\n";
      if (!oneway) {
        client_source_ << "    if (status == THRIFT_OK) {\n";
        if (has_return) {
          client_source_ << "        *out_success = result.has_success ? result.f_success : ("
                         << c_type(returntype) << "){0};\n";
          if (return_true->is_struct() || return_true->is_xception())
            client_source_ << "        result.f_success = NULL;\n";
        }
        for (auto* field : exceptions) {
          const std::string ename = snake(field->get_name());
          client_source_ << "        *out_" << ename << " = result.f_" << ename << ";\n"
                         << "        result.f_" << ename << " = NULL;\n";
        }
        client_source_ << "    }\n"
                       << "    " << record_symbol << "_result_clear(&result);\n";
      }
      client_source_ << "    return status;\n}\n\n";
    }
  }

  // Symmetric counterpart to flat_client_api() for server_stubs: emits an
  // application-implemented flat callback (declared in the always-regenerated
  // header, defined in the preserved *_server.c, like the record-based stub)
  // plus a static inline adapter matching the fixed (user_context, args, result)
  // handler slot. Output pointers alias result's own fields directly, so unlike
  // the client side no pre-NULL/aliasing checks are needed: the dispatcher hands
  // the adapter a freshly zeroed result before every call.
  void flat_server_callback(const std::string& symbol, const std::string& method_symbol,
                            const method_entry& entry) {
    auto* method = entry.method;
    const std::string member = snake(method->get_name());
    const std::vector<t_field*>& arg_fields = method->get_arglist()->get_members();
    const bool oneway = method->is_oneway();
    t_type* returntype = method->get_returntype();
    const bool has_return = !returntype->is_void();
    const std::vector<t_field*> exceptions = oneway ? std::vector<t_field*>{} : method->get_xceptions()->get_members();
    const std::string callback = symbol + "_server_" + member;
    const std::string adapter = callback + "_adapter";
    reserve(callback);
    reserve(adapter, false);
    auto out_param = [](const std::string& type_str) {
      return type_str + (type_str.back() == '*' ? "*" : " *");
    };
    std::vector<std::string> argument_names;
    std::vector<std::string> parameters = {"void *user_context"};
    for (size_t i = 0; i < arg_fields.size(); ++i) {
      auto* field = arg_fields[i];
      auto* type = field->get_type()->get_true_type();
      const std::string argument = "arg_" + std::to_string(i + 1) + "_" + snake(field->get_name());
      argument_names.push_back(argument);
      if (type->is_string()) {
        parameters.push_back("const void *" + argument + "_data");
        parameters.push_back("size_t " + argument + "_size");
      } else {
        parameters.push_back(c_type(field->get_type()) + " " + argument);
      }
    }
    if (!oneway) {
      if (has_return) parameters.push_back(out_param(c_type(returntype)) + "out_success");
      for (auto* field : exceptions)
        parameters.push_back(out_param(c_type(field->get_type())) + "out_" + snake(field->get_name()));
    }
    std::ostringstream signature;
    signature << "enum thrift_status " << callback << "(";
    for (size_t i = 0; i < parameters.size(); ++i)
      signature << (i ? ",\n    " : "") << parameters[i];
    signature << ")";

    std::string doc = "Implement " + entry.owner->get_name() + "." + method->get_name() + ".\n"
        "Flattened callback: one parameter per IDL argument, one output pointer per\n"
        "success/declared exception, matching the --gen c11:flat_calls client call.\n"
        "Arguments are borrowed for this call. On THRIFT_OK, populate exactly one\n"
        "output (or none, for a void result with no exception); any other status is\n"
        "reported to the caller as an application error. Copy borrowed data before\n"
        "storing it in an owned output. Oneway methods have no outputs and cannot\n"
        "send an error reply. Populating more than one declared-exception output (or\n"
        "a struct-typed success alongside one) makes the adapter reject the reply\n"
        "with THRIFT_PROTOCOL instead of silently picking the first; a scalar success\n"
        "cannot join that check and is only used when no exception fired.\n";
    documentation(doc, "", &server_header_);
    server_header_ << signature.str() << ";\n";

    // Matches the fixed handler slot signature; forwards to the flat callback
    // above by pointing its outputs directly at the freshly zeroed result.
    server_header_ << "static inline enum thrift_status " << adapter << "(void *user_context, const struct "
                   << method_symbol << "_args *args, struct " << method_symbol << "_result *result)\n{\n";
    if (!oneway) server_header_ << "    enum thrift_status status;\n";
    server_header_ << "    if (!args) return THRIFT_NULL_ARGS;\n";
    if (!oneway) server_header_ << "    if (!result) return THRIFT_NULL_RESULT;\n";
    std::ostringstream call;
    call << callback << "(user_context";
    for (size_t i = 0; i < arg_fields.size(); ++i) {
      auto* field = arg_fields[i];
      auto* type = field->get_type()->get_true_type();
      const std::string fname = snake(field->get_name());
      if (type->is_string())
        call << ", args->f_" << fname << ".data, args->f_" << fname << ".size";
      else
        call << ", args->f_" << fname;
    }
    if (!oneway) {
      if (has_return) call << ", &result->f_success";
      for (auto* field : exceptions)
        call << ", &result->f_" << snake(field->get_name());
    }
    call << ")";
    if (oneway) {
      server_header_ << "    return " << call.str() << ";\n}\n\n";
    } else {
      // A pointer-typed success (struct/xception) shares the exception outputs'
      // "non-NULL means active" signal; a scalar success has none (the value is
      // always present, active or not) and so cannot join this exclusivity check.
      const bool success_checkable = has_return && (returntype->get_true_type()->is_struct() ||
                                                       returntype->get_true_type()->is_xception());
      // A single checkable output can never conflict with itself: "(a != NULL)
      // > 1" is a compile-time-constant-false bool comparison GCC/Clang flag
      // under -Wbool-compare (an -Werror build would fail), so only emit the
      // check once there are at least two candidates to actually compare.
      const size_t checkable_count = exceptions.size() + (success_checkable ? 1 : 0);
      server_header_ << "    status = " << call.str() << ";\n"
                     << "    if (status != THRIFT_OK) return status;\n";
      if (checkable_count > 1) {
        server_header_ << "    if (";
        bool first = true;
        for (auto* field : exceptions) {
          server_header_ << (first ? "(" : " + (") << "result->f_" << snake(field->get_name()) << " != NULL)";
          first = false;
        }
        if (success_checkable)
          server_header_ << (first ? "(" : " + (") << "result->f_success != NULL)";
        server_header_ << " > 1)\n        return THRIFT_PROTOCOL; /* callback populated more than one output */\n";
      }
      {
        bool first = true;
        for (auto* field : exceptions) {
          const std::string ename = snake(field->get_name());
          server_header_ << "    " << (first ? "if" : "else if") << " (result->f_" << ename
                         << ") result->has_" << ename << " = true;\n";
          first = false;
        }
        if (has_return)
          server_header_ << "    " << (first ? "" : "else ") << "result->has_success = true;\n";
      }
      server_header_ << "    return THRIFT_OK;\n}\n\n";
    }

    server_source_ << "/* " << doc << " */\n" << signature.str() << "\n{\n    (void)user_context;\n";
    for (size_t i = 0; i < arg_fields.size(); ++i) {
      auto* field = arg_fields[i];
      auto* type = field->get_type()->get_true_type();
      if (type->is_string())
        server_source_ << "    (void)" << argument_names[i] << "_data;\n    (void)" << argument_names[i] << "_size;\n";
      else
        server_source_ << "    (void)" << argument_names[i] << ";\n";
    }
    if (!oneway) {
      if (has_return) server_source_ << "    (void)out_success;\n";
      for (auto* field : exceptions)
        server_source_ << "    (void)out_" << snake(field->get_name()) << ";\n";
    }
    server_source_ << "    return THRIFT_NOT_IMPLEMENTED;\n}\n\n";
  }

  void server_stub(t_service* service, const std::vector<method_entry>& entries) {
    const std::string symbol = name(service);
    reserve(symbol + "_server_init");
    reserve(symbol + "_server_process");
    server_header_ << "/** Initialize callbacks and store borrowed application data in handler->user_context (may be NULL).\n"
                   << " * The handler must outlive all server requests; synchronize access to shared application data.\n"
                   << " * Returns THRIFT_NULL_HANDLER for a NULL handler, otherwise THRIFT_OK. */\n"
                   << "enum thrift_status " << symbol << "_server_init(struct " << symbol
                   << "_handler *handler, void *user_context);\n"
                   << "/** Server-runtime adapter; handler_context points to the initialized handler, not user_context.\n"
                   << " * Validation returns THRIFT_NULL_PROTOCOL, THRIFT_PROTOCOL_UNINITIALIZED or THRIFT_NULL_HANDLER. */\n"
                   << "enum thrift_status " << symbol << "_server_process(struct thrift_protocol *protocol, void *handler_context);\n\n";
    for (const auto& entry : entries) {
      const std::string member = snake(entry.method->get_name());
      const std::string method = name(entry.owner) + "_" + member;
      if (flat_calls_ && flat_eligible(entry.method)) {
        flat_server_callback(symbol, method, entry);
        continue;
      }
      const std::string callback = symbol + "_server_handle_" + member;
      reserve(callback, false);
      server_source_ << "/* Implement " << entry.owner->get_name() << "." << entry.method->get_name()
                     << ". Args are borrowed for this call; result is owned by the dispatcher.\n"
                     << " * Use result setters for success or a declared exception, then return THRIFT_OK.\n"
                     << " * Copy borrowed data before storing it in an owned result.\n"
                     << " * Unimplemented methods fail; oneway methods cannot send an error reply. */\n"
                     << "static enum thrift_status " << callback << "(void *user_context, const struct " << method
                     << "_args *args, struct " << method << "_result *result)\n{\n"
                     << "    (void)user_context;\n    (void)result;\n"
                     << "    if (!args) return THRIFT_NULL_ARGS;\n"
                     << (entry.method->is_oneway() ? "" : "    if (!result) return THRIFT_NULL_RESULT;\n")
                     << "    return THRIFT_NOT_IMPLEMENTED;\n}\n\n";
    }
    server_source_ << "enum thrift_status " << symbol << "_server_init(struct " << symbol
                   << "_handler *handler, void *user_context)\n{\n"
                   << "    if (!handler) return THRIFT_NULL_HANDLER;\n"
                   << "    *handler = (struct " << symbol << "_handler){0};\n"
                   << "    handler->user_context = user_context;\n";
    for (const auto& entry : entries) {
      const std::string member = snake(entry.method->get_name());
      const std::string callback = flat_calls_ && flat_eligible(entry.method)
          ? symbol + "_server_" + member + "_adapter" : symbol + "_server_handle_" + member;
      server_source_ << "    handler->f_" << member << " = " << callback << ";\n";
    }
    server_source_ << "    return THRIFT_OK;\n}\n\n"
                   << "enum thrift_status " << symbol << "_server_process(struct thrift_protocol *protocol, void *handler_context)\n{\n"
                   << "    if (!protocol) return THRIFT_NULL_PROTOCOL;\n"
                   << "    if (!protocol->ops) return THRIFT_PROTOCOL_UNINITIALIZED;\n"
                   << "    if (!handler_context) return THRIFT_NULL_HANDLER;\n"
                   << "    return " << symbol << "_process(protocol, handler_context);\n}\n\n";
  }

  void generate_service(t_service* service) override {
    for (auto* method : service->get_functions()) {
      // The shared parser only warns, but C11 cannot deliver a result for oneway calls.
      if (method->is_oneway() && !method->get_returntype()->is_void())
        throw std::string("c11: oneway method must return void: ") + method->get_name();
    }
    const std::string service_symbol = name(service);
    reserve(service_symbol);
    reserve(service_symbol + "_handler");
    reserve(service_symbol + "_methods", false);
    for (auto* method : service->get_functions()) {
      const std::string symbol = service_symbol + "_" + snake(method->get_name());
      record(symbol + "_args", method->get_arglist()->get_members(), false);
      std::vector<t_field*> results = method->get_xceptions()->get_members();
      t_field success(method->get_returntype(), "success", 0);
      success.set_req(t_field::T_OPTIONAL);
      if (!method->get_returntype()->is_void()) results.insert(results.begin(), &success);
      record(symbol + "_result", results, true);
    }
    std::vector<method_entry> entries;
    methods(service, entries);
    client_api(service, entries);
    if (flat_calls_) flat_client_api(service, entries);
    if (server_stubs_) server_stub(service, entries);
    std::set<std::string> method_names;
    documentation(service->get_doc());
    header_ << "struct " << service_symbol << "_handler {\n    void *user_context; /**< Borrowed application data passed to method callbacks. */\n";
    for (const auto& entry : entries) {
      const std::string member = snake(entry.method->get_name());
      if (!method_names.insert(member).second)
        throw std::string("c11 method collision in ") + service_symbol + ": " + member;
      const std::string symbol = name(entry.owner) + "_" + member;
      documentation(entry.method->get_doc(), "    ");
      header_ << "    enum thrift_status (*f_" << member << ")(void *user_context, const struct " << symbol
              << "_args *args, struct " << symbol << "_result *result);\n";
    }
    header_ << "};\n";
    for (const auto& entry : entries) {
      const std::string member = snake(entry.method->get_name());
      const std::string call = service_symbol + "_" + member;
      const std::string symbol = name(entry.owner) + "_" + member;
      reserve(call + "_call");
      reserve(call + "_invoke", false);
      documentation(entry.method->get_doc());
      header_ << "enum thrift_status " << call << "_call(struct thrift_protocol *protocol, int32_t sequence,\n"
              << "    const struct " << symbol << "_args *args, struct " << symbol << "_result *result);\n";
      source_ << "static enum thrift_status " << call << "_invoke(void *handler_context, const void *args, void *result)\n{\n"
              << "    struct " << service_symbol << "_handler *handler = handler_context;\n"
              << "    if (!handler) return THRIFT_NULL_HANDLER;\n"
              << "    if (!args) return THRIFT_NULL_ARGS;\n"
              << (entry.method->is_oneway() ? "" : "    if (!result) return THRIFT_NULL_RESULT;\n")
              << "    if (!handler->f_" << member << ") return THRIFT_MISSING_CALLBACK;\n"
              << "    return handler->f_" << member << "(handler->user_context, args, result);\n}\n\n";
    }
    if (!entries.empty()) {
      source_ << "static const struct thrift_method " << service_symbol << "_methods[] = {\n";
      for (const auto& entry : entries) {
        const std::string member = snake(entry.method->get_name());
        const std::string symbol = name(entry.owner) + "_" + member;
        source_ << "    {" << literal(entry.method->get_name()) << ", &" << symbol << "_args_record, &" << symbol
                << "_result_record, " << (entry.method->is_oneway() ? "true" : "false") << ", "
                << (entry.method->get_returntype()->is_void() ? "false" : "true") << ", "
                << service_symbol << "_" << member << "_invoke},\n";
      }
      source_ << "};\n\n";
    }
    for (size_t i = 0; i < entries.size(); ++i) {
      const auto& entry = entries[i];
      const std::string member = snake(entry.method->get_name());
      const std::string symbol = name(entry.owner) + "_" + member;
      source_ << "enum thrift_status " << service_symbol << "_" << member
              << "_call(struct thrift_protocol *protocol, int32_t sequence,\n"
              << "    const struct " << symbol << "_args *args, struct " << symbol << "_result *result)\n{\n"
              << "    return thrift_client_call(protocol, &" << service_symbol << "_methods[" << i
              << "], sequence, args, result);\n}\n\n";
    }
    reserve(service_symbol + "_service");
    header_ << "struct thrift_service " << service_symbol << "_service(struct " << service_symbol << "_handler *handler);\n";
    source_ << "struct thrift_service " << service_symbol << "_service(struct " << service_symbol << "_handler *handler)\n{\n"
            << "    struct thrift_service service = {" << literal(service->get_name()) << ", "
            << (entries.empty() ? "NULL" : service_symbol + "_methods") << ", " << entries.size() << ", handler};\n"
            << "    return service;\n}\n\n";
    reserve(service_symbol + "_process");
    header_ << "enum thrift_status " << service_symbol << "_process(struct thrift_protocol *protocol,\n"
            << "    struct " << service_symbol << "_handler *handler);\n\n";
    source_ << "enum thrift_status " << service_symbol << "_process(struct thrift_protocol *protocol,\n"
            << "    struct " << service_symbol << "_handler *handler)\n{\n    return thrift_process(protocol, "
            << (entries.empty() ? "NULL" : service_symbol + "_methods") << ", " << entries.size() << ", handler);\n}\n\n";
    manifest_entry(service->get_name(), "service", {{"c_name", service_symbol}, {"handler", service_symbol + "_handler"},
        {"client", service_symbol + "_client"}, {"process", service_symbol + "_process"}});
    for (const auto& entry : entries) {
      const std::string member = snake(entry.method->get_name());
      const std::string symbol = name(entry.owner) + "_" + member;
      const std::string ineligibility = flat_ineligibility(entry.method);
      const bool eligible = flat_calls_ && ineligibility.empty();
      if (flat_calls_ && !ineligibility.empty())
        pwarning(1, "c11: flat_calls: no flat call for %s.%s (%s); use %s_client_%s_call() instead\n",
            entry.owner->get_name().c_str(), entry.method->get_name().c_str(), ineligibility.c_str(),
            service_symbol.c_str(), member.c_str());
      std::vector<std::pair<std::string, std::string>> fields = {
          {"stem", symbol}, {"owner", name(entry.owner)}, {"call", service_symbol + "_" + member + "_call"},
          {"client_call", service_symbol + "_client_" + member + "_call"},
          {"args_record", symbol + "_args"}, {"result_record", symbol + "_result"}};
      if (eligible)
        fields.emplace_back("flat_client_call", service_symbol + "_client_" + member);
      if (server_stubs_)
        fields.emplace_back("server_callback", eligible ? service_symbol + "_server_" + member
                                                          : service_symbol + "_server_handle_" + member);
      // Keyed by the service actually being generated, not by "owner": an
      // inherited method gets its own client_call/server_callback per service
      // that exposes it (Child's own client can call it through Child's own
      // wrapper, distinct from Base's), so "Base.get" would otherwise appear
      // once per subclass with different values under the same ambiguous key.
      manifest_entry(service->get_name() + "." + entry.method->get_name(), "method", fields);
    }
  }
};

THRIFT_REGISTER_GENERATOR(c11, "C11", "    Dependency-free C11 types and Binary/Compact client/server bindings.\n"
    "    server_stubs:    Emit editable server skeletons; preserve existing *_server.c files.\n"
    "    flat_calls:      Emit flattened <client>_<method>(client, args..., outs...) calls\n"
    "                     alongside the record-based ones, for eligible methods. With\n"
    "                     server_stubs, also emit a matching flat *_server_<method>()\n"
    "                     callback for the application to implement, instead of the\n"
    "                     record-based *_server_handle_<method>().\n"
    "    manifest:        Emit <program>_manifest.json mapping every IDL name to its\n"
    "                     generated C symbol(s), for tools that would otherwise have\n"
    "                     to reimplement the snake_case/prefix naming rules.\n")
