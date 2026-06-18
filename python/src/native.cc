// rawast Python binding — exposes the Grammar class and its
// parse / save / lint methods. Type conversion goes directly
// between rawast::Value family and native Python objects
// (None/bool/int/float/str/list/dict) with no JSON intermediary.

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>

#include <rawast/grammar.hpp>
#include <rawast/linter.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/parsers_gdsii.hpp>
#include <rawast/parsers_lefdef.hpp>
#include <rawast/parsers_tcl.hpp>
#include <rawast/parsers_registry.hpp>
#include <rawast/preprocessor.hpp>
#include <rawast/to_value.hpp>

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
    rawast::register_tcl_parser_group();

    // Stream — opaque Python type. The canonical Grammar input.
    // Produced by Stream.from_string / Stream.from_file, or by
    // Preprocessor.preprocess. Consumed by Grammar.parse_stream.
    // Move-only on the C++ side; nanobind treats this as an opaque
    // handle (Python sees one object, no copies).
    nb::class_<rawast::Stream>(m, "Stream",
        "Canonical parser input. Construct via from_string / from_file, "
        "or receive one from Preprocessor.preprocess. Consume via "
        "Grammar.parse_stream.")
        .def_static("from_string",
            [](std::string source) {
                return rawast::Stream::from_string(std::move(source));
            },
            nb::arg("source"),
            "Build a Stream backed by an in-memory string.")
        .def_static("from_file",
            [](const std::string& path) {
                return rawast::Stream::from_file(path);
            },
            nb::arg("path"),
            "Build a Stream that reads from a file path. Throws on open "
            "failure.");

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
                auto stream = rawast::Stream::from_file(path);
                auto r = g.parse(stream);
                if (!r) throw std::runtime_error(format_parse_error(r.error()));
                return value_to_python(*r);
            },
            nb::arg("path"),
            "Parse a file from the grammar's default start. Returns a Python "
            "value (None/bool/int/float/str/list/dict).")

        // Overload accepting a Preprocessor. Reads the file, runs it
        // through the preprocessor, then parses the resulting string
        // through `g`. The Preprocessor's state (macros, included_files,
        // warnings) accumulates across calls; reuse one instance for
        // a multi-file corpus.
        .def("parse_file",
            [](rawast::Grammar& g, const std::string& path,
               rawast::Preprocessor& pp) {
                auto preprocessed = pp.process_file(path);
                auto stream = rawast::Stream::from_string(std::move(preprocessed));
                auto r = g.parse(stream);
                if (!r) throw std::runtime_error(format_parse_error(r.error()));
                return value_to_python(*r);
            },
            nb::arg("path"), nb::arg("preprocessor"),
            "Parse a file after running it through the given Preprocessor. "
            "Macro state, includes, and warnings accumulate on the Preprocessor "
            "across calls so it can be reused for a multi-file corpus.")

        .def("parse_file",
            [](rawast::Grammar& g, const std::string& path,
               const std::string& start) {
                auto stream = rawast::Stream::from_file(path);
                auto r = g.parse_from(stream, start);
                if (!r) throw std::runtime_error(format_parse_error(r.error()));
                return value_to_python(*r);
            },
            nb::arg("path"), nb::arg("start"),
            "Parse a file starting from the named rule instead of the grammar's "
            "default start. Used for re-parsing arbitrary strings through any "
            "rule in the grammar.")

        .def("parse_string",
            [](rawast::Grammar& g, const std::string& content) {
                auto stream = rawast::Stream::from_string(content);
                auto r = g.parse(stream);
                if (!r) throw std::runtime_error(format_parse_error(r.error()));
                return value_to_python(*r);
            },
            nb::arg("content"),
            "Parse a string from the grammar's default start.")

        .def("parse_string",
            [](rawast::Grammar& g, const std::string& content,
               rawast::Preprocessor& pp) {
                auto preprocessed = pp.process(content);
                auto stream = rawast::Stream::from_string(std::move(preprocessed));
                auto r = g.parse(stream);
                if (!r) throw std::runtime_error(format_parse_error(r.error()));
                return value_to_python(*r);
            },
            nb::arg("content"), nb::arg("preprocessor"),
            "Parse a string after running it through the given Preprocessor.")

        .def("parse_string",
            [](rawast::Grammar& g, const std::string& content,
               const std::string& start) {
                auto stream = rawast::Stream::from_string(content);
                auto r = g.parse_from(stream, start);
                if (!r) throw std::runtime_error(format_parse_error(r.error()));
                return value_to_python(*r);
            },
            nb::arg("content"), nb::arg("start"),
            "Parse a string starting from the named rule.")

        .def("parse_bytes",
            [](rawast::Grammar& g, nb::bytes b) {
                auto stream = rawast::Stream::from_string(std::string(b.c_str(), b.size()));
                auto r = g.parse(stream);
                if (!r) throw std::runtime_error(format_parse_error(r.error()));
                return value_to_python(*r);
            },
            nb::arg("data"),
            "Parse a bytes object from the grammar's default start. Use for "
            "binary grammars (GDSII, etc.).")

        .def("parse_bytes",
            [](rawast::Grammar& g, nb::bytes b, const std::string& start) {
                auto stream = rawast::Stream::from_string(std::string(b.c_str(), b.size()));
                auto r = g.parse_from(stream, start);
                if (!r) throw std::runtime_error(format_parse_error(r.error()));
                return value_to_python(*r);
            },
            nb::arg("data"), nb::arg("start"),
            "Parse a bytes object starting from the named rule.")

        // Parse a Stream directly. The canonical entry point — the
        // string/file/bytes overloads above are convenience wrappers
        // that build a Stream internally. Used by callers who already
        // hold a Stream (e.g. from Preprocessor.preprocess).
        .def("parse_stream",
            [](rawast::Grammar& g, rawast::Stream& stream) {
                auto r = g.parse(stream);
                if (!r) throw std::runtime_error(format_parse_error(r.error()));
                return value_to_python(*r);
            },
            nb::arg("stream"),
            "Parse a Stream from the grammar's default start. The Stream "
            "is consumed; reuse is undefined.")

        .def("parse_stream",
            [](rawast::Grammar& g, rawast::Stream& stream,
               const std::string& start) {
                auto r = g.parse_from(stream, start);
                if (!r) throw std::runtime_error(format_parse_error(r.error()));
                return value_to_python(*r);
            },
            nb::arg("stream"), nb::arg("start"),
            "Parse a Stream starting from the named rule.")

        .def("save",
            [](rawast::Grammar& g, nb::handle value, bool pretty,
               std::optional<std::string> start) -> nb::object {
                auto v = python_to_value(value);
                rawast::NodeId start_id{};
                if (start) {
                    if (!g.has_rule(*start)) {
                        throw std::runtime_error(
                            "save: unknown start rule '" + *start + "'");
                    }
                    start_id = g.rule_id(*start);
                }
                std::ostringstream out;
                auto r = g.save(out, v, pretty, start_id);
                if (!r) throw std::runtime_error(r.error().message);
                const std::string s = out.str();
                // Return bytes — binary-safe for GDSII and similar.
                return nb::bytes(s.data(), s.size());
            },
            nb::arg("value"), nb::arg("pretty") = true,
            nb::arg("start") = nb::none(),
            "Save a value through the grammar. Returns bytes (binary-"
            "safe). `start` picks a top rule other than the grammar's "
            "default (mirrors parse_string/parse_file/parse_bytes).")

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

        // --- Profiling -----------------------------------------------
        .def("profile_enable",
            [](const rawast::Grammar& g, bool yes) {
                g.profile_enable(yes);
            },
            nb::arg("yes") = true,
            "Toggle parse-time profiling. When on, each subsequent "
            "parse_file/parse_string/parse_bytes call records per-rule "
            "entry/fail/time counters; read them back via "
            "last_profile_report(). Sticky until explicitly disabled.")

        .def("profile_enabled",
            [](const rawast::Grammar& g) { return g.profile_enabled(); },
            "Returns True if profiling is on.")

        .def("last_profile_report",
            [](const rawast::Grammar& g) -> nb::object {
                const auto& r = g.last_profile_report();
                nb::list entries;
                for (const auto& e : r.entries) {
                    nb::dict d;
                    d["node_id"]      = e.node_id.value();
                    d["rule_name"]    = e.rule_name;
                    d["entry_count"]  = e.entry_count;
                    d["fail_count"]   = e.fail_count;
                    d["total_ns"]     = e.total_ns;
                    d["max_depth"]    = e.max_depth;
                    entries.append(d);
                }
                nb::dict report;
                report["entries"]      = entries;
                report["total_ns"]     = r.total_ns;
                report["total_frames"] = r.total_frames;
                report["max_depth"]    = r.max_depth;
                return report;
            },
            "Return the profile report from the most recent parse call. "
            "Shape: {entries: [{node_id, rule_name, entry_count, fail_count, "
            "total_ns, max_depth}, …], total_ns, total_frames, max_depth}. "
            "`entries` is unsorted; sort by total_ns or entry_count to get "
            "the top hotspots.")

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
            "file produces when parsed through Grammar() — the built-in "
            "JSONC grammar — or Grammar(\"rawast\") — the .rawast "
            "meta-grammar). Inverse of `meta.parse_file(grammar_file)`.")

        .def("to_dict",
            [](const rawast::Grammar& g) -> nb::object {
                auto v = rawast::to_value(g);
                if (!v) return nb::none();
                return value_to_python(v);
            },
            "Walk this Grammar and return its dict representation — the "
            "inverse of `from_dict`. The returned dict can be passed back "
            "through `Grammar.from_dict(...)` to reconstruct an equivalent "
            "Grammar (the host must register the same parser groups via "
            "`use:` in the dict, or pre-register parsers on the receiving "
            "Grammar instance). Useful for inspection, debugging, and the "
            "planned `rawast cppgen` codegen path.")

        .def_static("json_format_builtin",
            []() {
                return std::make_unique<rawast::Grammar>(
                    rawast::make_json_grammar());
            },
            "The JSON grammar — built in C++ via make_json_grammar(), no "
            "file IO. Parses any JSON document, including JSON-form "
            "grammar files (use it to browse a JSON-form grammar as a "
            "Python dict).");

    // --- Preprocessor binding ------------------------------------------
    //
    // Construction takes a Grammar (the preprocessor grammar — typically
    // sv_preprocessor) and keyword-only behavior options. The class
    // holds policy + accumulating state; reuse one instance across many
    // process_file calls so macros, included_files, and warnings carry
    // through naturally.
    //
    // Lifetime: keep_alive<1, 2> ties the Grammar to the Preprocessor
    // instance — the Grammar must outlive the Preprocessor (the C++
    // class stores a const Grammar&).
    nb::class_<rawast::Preprocessor>(m, "Preprocessor",
        "Apply preprocessor semantics (macro expansion, conditional "
        "compilation, includes) to source text before parsing.")
        .def("__init__",
            [](rawast::Preprocessor* self, rawast::Grammar& g,
               const std::string& predefined,
               const std::vector<std::string>& include_paths,
               bool splice,
               const std::string& on_undefined,
               int max_expansion_depth,
               bool trace) {
                rawast::PpOptions opts;
                opts.predefined = predefined;
                opts.include_paths = include_paths;
                opts.splice = splice;
                auto ou = rawast::parse_pp_on_undefined(on_undefined);
                if (!ou) {
                    throw std::runtime_error(
                        "Preprocessor: unknown on_undefined '" +
                        on_undefined + "' (valid: leave, error, warn, empty)");
                }
                opts.on_undefined = *ou;
                opts.max_expansion_depth = max_expansion_depth;
                opts.trace = trace;
                new (self) rawast::Preprocessor(g, std::move(opts));
            },
            nb::arg("grammar"),
            nb::arg("predefined") = std::string{},
            nb::arg("include_paths") = std::vector<std::string>{},
            nb::arg("splice") = false,
            nb::arg("on_undefined") = std::string{"leave"},
            nb::arg("max_expansion_depth") = 200,
            nb::arg("trace") = false,
            nb::keep_alive<1, 2>(),
            "Construct a Preprocessor. The grammar should be a loaded "
            "preprocessor grammar (e.g. sv_preprocessor.rawast). All "
            "behavior options are keyword-only; defaults match the "
            "documented spec.")

        .def("process",
            [](rawast::Preprocessor& pp, const std::string& text) {
                return pp.process(text);
            },
            nb::arg("text"),
            "Run the preprocessor on the given source text. State "
            "(macros, included_files, warnings) accumulates on this "
            "instance across calls; reuse it for a multi-file corpus.")

        .def("process_file",
            [](rawast::Preprocessor& pp, const std::string& path) {
                return pp.process_file(path);
            },
            nb::arg("path"),
            "Read a file from disk and run it through the preprocessor. "
            "Tracks the path in `included_files` (first-seen order; "
            "duplicates suppressed) and sets the internal current-file "
            "context for warning attribution.")

        // Three-mode API ------------------------------------------------
        // Mode 1: parse only — return the preprocessor AST without
        // expanding macros, includes, or conditionals.
        .def("parse",
            [](rawast::Preprocessor& pp, const std::string& source) {
                auto r = pp.parse(source);
                if (!r) throw std::runtime_error(format_parse_error(r.error()));
                return value_to_python(*r);
            },
            nb::arg("source"),
            "Mode 1: parse the source through the preprocessor grammar "
            "and return the raw AST as a Python value, without expanding "
            "directives. Useful for tooling that wants to inspect macro / "
            "include / ifdef structure.")

        // Mode 2: expand a Python-shaped AST into a Stream — same
        // canonical handoff type as the C++ API. Feed it to
        // Grammar.parse_stream for Mode 3.
        .def("preprocess",
            [](rawast::Preprocessor& pp, nb::handle ast,
               const std::string& source) {
                auto v = python_to_value(ast);
                return pp.preprocess(v, source);
            },
            nb::arg("ast"), nb::arg("source"),
            "Mode 2: expand a preprocessor AST (as returned by parse()) "
            "and return the expanded bytes as a Stream. `source` is the "
            "original input text the AST was parsed from. State (macros, "
            "includes, warnings, spans) accumulates on this instance.")

        .def("is_defined",
            [](const rawast::Preprocessor& pp, const std::string& name) {
                return pp.is_defined(name);
            },
            nb::arg("name"),
            "True iff a macro of this name is currently in the macro table.")

        .def("get_macro",
            [](const rawast::Preprocessor& pp, const std::string& name)
                -> nb::object {
                const auto* m = pp.get_macro(name);
                if (!m) return nb::none();
                nb::dict d;
                d["name"] = m->name;
                d["body"] = m->body_text();
                nb::list params;
                for (const auto& p : m->params) params.append(p);
                d["params"] = params;
                d["is_function_like"] = m->is_function_like;
                return d;
            },
            nb::arg("name"),
            "Return the macro definition as a dict (or None if undefined). "
            "Keys: name, body, params, is_function_like.")

        .def("reset",
            [](rawast::Preprocessor& pp) { pp.reset(); },
            "Clear all accumulated state — macros, included_files, "
            "warnings — back to construction defaults.")

        .def_prop_ro("macros",
            [](const rawast::Preprocessor& pp) {
                nb::dict out;
                for (const auto& [name, m] : pp.macros()) {
                    nb::dict d;
                    d["name"] = m.name;
                    d["body"] = m.body_text();
                    nb::list params;
                    for (const auto& p : m.params) params.append(p);
                    d["params"] = params;
                    d["is_function_like"] = m.is_function_like;
                    out[nb::str(name.c_str())] = d;
                }
                return out;
            },
            "Current macro table as a dict of name → macro-info dict.")

        .def_prop_ro("included_files",
            [](const rawast::Preprocessor& pp) {
                return pp.included_files();
            },
            "Files processed so far, in first-seen order. Duplicates "
            "are suppressed (implicit include-once).")

        .def_prop_ro("warnings",
            [](const rawast::Preprocessor& pp) {
                nb::list out;
                for (const auto& w : pp.warnings()) {
                    nb::dict d;
                    d["message"] = w.message;
                    d["file"] = w.file;
                    d["line"] = w.line;
                    out.append(d);
                }
                return out;
            },
            "Accumulated warnings as a list of {message, file, line} dicts.")

        .def_prop_ro("spans",
            [](const rawast::Preprocessor& pp) {
                nb::list out;
                for (const auto& s : pp.spans()) {
                    nb::dict d;
                    d["id"] = s.id;
                    d["parent_id"] = (s.parent_id == rawast::Span::NoParent)
                        ? nb::cast<nb::object>(nb::none())
                        : nb::cast<nb::object>(nb::int_(s.parent_id));
                    d["parent_offset"] = s.parent_offset;
                    d["length"] = s.length;
                    d["out_offset"] = (s.out_offset == rawast::Span::NoOutput)
                        ? nb::cast<nb::object>(nb::none())
                        : nb::cast<nb::object>(nb::int_(s.out_offset));
                    d["name"] = s.name;
                    out.append(d);
                }
                return out;
            },
            "All source-provenance spans recorded by the last process() / "
            "process_file() call. Each entry is a dict {id, parent_id, "
            "parent_offset, length, out_offset, name}. parent_id and "
            "out_offset are None for root spans / source-structure spans "
            "respectively.")

        .def("stack_at",
            [](const rawast::Preprocessor& pp, std::size_t out_offset) {
                nb::list out;
                for (const auto& f : pp.stack_at(out_offset)) {
                    nb::dict d;
                    d["where"] = f.where;
                    d["offset"] = f.offset;
                    out.append(d);
                }
                return out;
            },
            nb::arg("out_offset"),
            "Build the source-provenance frame stack for a byte offset in "
            "the preprocessed output. Returns a list of {where, offset} "
            "dicts ordered leaf-first (immediate source first, ultimate "
            "root last). Empty list if the offset is outside any recorded "
            "span.");

    m.attr("__version__") = "0.1.8";
}
