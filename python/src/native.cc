// rawast Python binding — exposes the Grammar class and its
// parse / save / lint methods. Type conversion goes directly
// between rawast::Value family and native Python objects
// (None/bool/int/float/str/list/dict) with no JSON intermediary.

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>

#include <rawast/grammar.hpp>
#include <rawast/linter.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/parsers_gdsii.hpp>
#include <rawast/parsers_lefdef.hpp>
#include <rawast/parsers_registry.hpp>

#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace nb = nanobind;

namespace {

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// --- Value <-> Python conversion ----------------------------------------

nb::object value_to_python(const rawast::ValuePtr& v) {
    using namespace rawast;
    if (!v) return nb::none();
    switch (v->type()) {
    case ValueType::Null:
        return nb::none();
    case ValueType::Bool:
        return nb::cast(std::dynamic_pointer_cast<BoolValue>(v)->data());
    case ValueType::Int:
        return nb::cast(std::dynamic_pointer_cast<IntValue>(v)->data());
    case ValueType::UInt:
        return nb::cast(std::dynamic_pointer_cast<UIntValue>(v)->data());
    case ValueType::Real:
        return nb::cast(std::dynamic_pointer_cast<RealValue>(v)->data());
    case ValueType::String:
        return nb::cast(std::dynamic_pointer_cast<StringValue>(v)->data());
    case ValueType::Array: {
        auto arr = std::dynamic_pointer_cast<ArrayValue>(v);
        nb::list out;
        for (const auto& e : arr->data()) out.append(value_to_python(e));
        return out;
    }
    case ValueType::Dict: {
        auto dict = std::dynamic_pointer_cast<DictValue>(v);
        nb::dict out;
        for (const auto& [k, val] : dict->data()) {
            out[nb::str(k.c_str())] = value_to_python(val);
        }
        return out;
    }
    }
    return nb::none();
}

rawast::ValuePtr python_to_value(nb::handle obj) {
    using namespace rawast;
    if (obj.is_none()) return null_value();
    if (nb::isinstance<nb::bool_>(obj)) {
        return nb::cast<bool>(obj) ? true_value() : false_value();
    }
    if (nb::isinstance<nb::int_>(obj)) {
        return make_int(nb::cast<std::int64_t>(obj));
    }
    if (nb::isinstance<nb::float_>(obj)) {
        return make_real(nb::cast<double>(obj));
    }
    if (nb::isinstance<nb::str>(obj)) {
        return make_string(nb::cast<std::string>(obj));
    }
    if (nb::isinstance<nb::list>(obj) || nb::isinstance<nb::tuple>(obj)) {
        auto arr = std::make_shared<ArrayValue>();
        for (auto e : obj) arr->data().push_back(python_to_value(e));
        return arr;
    }
    if (nb::isinstance<nb::dict>(obj)) {
        auto dict = std::make_shared<DictValue>();
        for (auto item : nb::cast<nb::dict>(obj)) {
            dict->data()[nb::cast<std::string>(item.first)] =
                python_to_value(item.second);
        }
        return dict;
    }
    throw std::runtime_error("rawast: unsupported Python type for save");
}

std::string format_parse_error(const rawast::ParseError& e) {
    std::ostringstream out;
    out << e.message << " (byte " << e.position.bytes
        << ", line " << e.position.line
        << ", column " << e.position.column << ")";
    return out.str();
}

} // namespace

NB_MODULE(_native, m) {
    // Register built-in parser groups so grammars opting in via
    // `"use": ["std", "gdsii"]` resolve correctly. Library-side
    // anonymous-namespace auto-registration is unreliable across
    // linker configurations; explicit init here is the durable path.
    rawast::register_std_parser_group();
    rawast::register_gdsii_parser_group();
    rawast::register_lefdef_parser_group();

    nb::class_<rawast::Grammar>(m, "Grammar",
        "A loaded grammar — drives parse, save, and lint via its methods.")
        .def_static("load",
            [](const std::string& path) {
                using namespace rawast;
                auto g = std::make_unique<Grammar>();

                // Pre-register the standard text-format terminal parsers
                // (int, float, identifier, double-quote string, whitespace,
                // line/block comments). Grammars that don't reference a
                // given parser are unaffected; those that do (almost any
                // text grammar) now work without the Python user knowing
                // about parser registration. Binary-format grammars (e.g.
                // gdsii.rawast) bring their own via `use:` directives.
                // No implicit parser registration here either —
                // grammars opt into parser groups via `use:` (e.g.
                // `"use": ["std"]` for text grammars, `use: gdsii`
                // for the binary GDSII grammar). Loader handles it.

                tl::expected<void, std::string> r;
                if (ends_with(path, ".rawast")) {
                    r = load_rawast_grammar_from_file(*g, path);
                } else {
                    r = load_json_grammar_from_file(*g, path);
                }
                if (!r) throw std::runtime_error(r.error());
                return g;
            },
            nb::arg("path"),
            "Load a grammar from a `.rawast` or `.json` file. Standard text "
            "terminal parsers (int, float, identifier, string, whitespace, "
            "comments) are pre-registered automatically; binary-format "
            "grammars supplement these via `use:` directives.")

        .def("parse_file",
            [](rawast::Grammar& g, const std::string& path) {
                std::ifstream fs(path, std::ios::binary);
                if (!fs) {
                    throw std::runtime_error("cannot open input: " + path);
                }
                rawast::StreamReader sr(fs);
                auto r = g.parse(sr);
                if (!r) throw std::runtime_error(format_parse_error(r.error()));
                return value_to_python(*r);
            },
            nb::arg("path"),
            "Parse a file. Returns a Python value (None/bool/int/float/str/list/dict).")

        .def("parse_string",
            [](rawast::Grammar& g, const std::string& content) {
                std::istringstream is(content);
                rawast::StreamReader sr(is);
                auto r = g.parse(sr);
                if (!r) throw std::runtime_error(format_parse_error(r.error()));
                return value_to_python(*r);
            },
            nb::arg("content"),
            "Parse a string.")

        .def("parse_bytes",
            [](rawast::Grammar& g, nb::bytes b) {
                std::string s(b.c_str(), b.size());
                std::istringstream is(std::move(s));
                rawast::StreamReader sr(is);
                auto r = g.parse(sr);
                if (!r) throw std::runtime_error(format_parse_error(r.error()));
                return value_to_python(*r);
            },
            nb::arg("data"),
            "Parse a bytes object. Use for binary grammars (GDSII, etc.).")

        .def("save",
            [](rawast::Grammar& g, nb::handle value, bool pretty) -> nb::object {
                auto v = python_to_value(value);
                std::ostringstream out;
                auto r = g.save(out, v, pretty);
                if (!r) throw std::runtime_error(r.error().message);
                const std::string s = out.str();
                // Return bytes — binary-safe for GDSII and similar.
                return nb::bytes(s.data(), s.size());
            },
            nb::arg("value"), nb::arg("pretty") = true,
            "Save a value through the grammar. Returns bytes (binary-safe).")

        .def("lint",
            [](const rawast::Grammar& g) {
                auto issues = rawast::lint_grammar(g);
                nb::list out;
                for (const auto& i : issues) {
                    out.append(nb::str(i.description.c_str()));
                }
                return out;
            },
            "Lint the grammar; returns a list of issue description strings.")

        .def_static("from_dict",
            [](nb::handle d) {
                using namespace rawast;
                auto g = std::make_unique<Grammar>();
                // Same standard parsers Grammar.load pre-registers — a
                // dict produced by parsing any rawast grammar file (JSON
                // or .rawast) references this same set.
                // No implicit parser registration here either —
                // grammars opt into parser groups via `use:` (e.g.
                // `"use": ["std"]` for text grammars, `use: gdsii`
                // for the binary GDSII grammar). Loader handles it.

                auto val = python_to_value(d);
                if (!val) throw std::runtime_error(
                    "rawast.Grammar.from_dict: input dict converted to null");
                auto r = load_json_grammar_into(*g, *val);
                if (!r) throw std::runtime_error(r.error());
                return g;
            },
            nb::arg("d"),
            "Build a Grammar from a value-tree dict (the form a grammar "
            "file produces when parsed through json_format() or "
            "rawast_format()). Inverse of `meta.parse_file(grammar_file)`.")

        .def_static("json_format_builtin",
            []() {
                return std::make_unique<rawast::Grammar>(
                    rawast::make_json_grammar());
            },
            "The JSON grammar — built in C++ via make_json_grammar(), no "
            "file IO. Parses any JSON document, including JSON-form "
            "grammar files (use it to browse a JSON-form grammar as a "
            "Python dict).");

    m.attr("__version__") = "0.1.0";
}
