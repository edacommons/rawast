// rawast Python binding — exposes the Grammar class and its
// parse / save / lint methods. Type conversion goes directly
// between rawast::Value family and native Python objects
// (None/bool/int/float/str/list/dict) with no JSON intermediary.

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>

#include <rawast/builder.hpp>
#include <rawast/compacting_builder.hpp>
#include <rawast/grammar.hpp>
#include <rawast/linter.hpp>
#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/parsers_gdsii.hpp>
#include <rawast/parsers_sv.hpp>
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

// Tag type backing the Python `Undefined` singleton.
struct UndefinedTag {};

// Non-owning handle to the Python `Undefined` singleton (created in
// NB_MODULE; kept alive by the module dict's `Undefined` attribute). Every
// rawast UndefinedValue maps to this exact object, so `x is Undefined`
// holds — the same identity contract Python's `None` has. A non-owning
// handle (not a static nb::object) avoids a decref running after the
// interpreter has finalized.
nb::handle& undefined_py() {
    static nb::handle h;
    return h;
}

// --- Value <-> Python conversion ----------------------------------------

nb::object value_to_python(const rawast::ValuePtr& v) {
    using namespace rawast;
    if (!v) return nb::none();
    switch (v->type()) {
    case ValueType::Null:
        return nb::none();
    case ValueType::Undefined:
        return nb::borrow(undefined_py());
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

// --- PyObjectBuilder — native Python as a representation ------------------
//
// The first foreign representation over the universal Builder seam:
// materialises Python dict/list/scalars DIRECTLY during the parse — no
// ValuePtr tree, no post-parse conversion pass. All events arrive on the
// calling thread with the GIL held (the binding never releases it).
// adopt() is overridden to route completed reference-model subtrees
// (subparse products, scope captures, grammar constants) through
// value_to_python — bit-identical fidelity with the classic path,
// including the Undefined return-only sentinel, which the typed event
// surface deliberately does not carry.
class PyObjectBuilder final : public rawast::Builder {
    struct EV    { nb::object v; bool is_name; };
    struct Level { rawast::Container kind; std::vector<EV> emitted; };
    std::vector<Level> levels_;

    void push(nb::object o, bool is_name) {
        levels_.back().emitted.push_back({std::move(o), is_name});
    }

public:
    PyObjectBuilder() { levels_.push_back({rawast::Container::None, {}}); }

    void null_(bool n) override                 { push(nb::none(), n); }
    void bool_(bool v, bool n) override         { push(nb::cast(v), n); }
    void int_(std::int64_t v, bool n) override  { push(nb::cast(v), n); }
    void uint_(std::uint64_t v, bool n) override{ push(nb::cast(v), n); }
    void real_(double v, bool n) override       { push(nb::cast(v), n); }
    void string_(std::string_view v, bool n) override {
        push(nb::str(v.data(), v.size()), n);
    }
    void adopt(const rawast::ValuePtr& v, bool is_name) override {
        push(value_to_python(v), is_name);
    }

    void begin(rawast::Container k) override { levels_.push_back({k, {}}); }

    void end() override {
        if (levels_.size() < 2) return;
        Level lvl = std::move(levels_.back());
        levels_.pop_back();
        auto& parent = levels_.back().emitted;
        switch (lvl.kind) {
        case rawast::Container::None:
            for (auto& e : lvl.emitted) parent.push_back(std::move(e));
            return;
        case rawast::Container::Array: {
            nb::list out;
            for (auto& e : lvl.emitted) out.append(e.v);
            parent.push_back({std::move(out), false});
            return;
        }
        case rawast::Container::Dict: {
            nb::dict out;
            std::string name;
            bool have = false;
            for (auto& e : lvl.emitted) {
                if (e.is_name) {
                    if (nb::isinstance<nb::str>(e.v)) {
                        name = nb::cast<std::string>(e.v);
                        have = true;
                    }
                } else if (have) {
                    if (name.size() >= 2
                        && name.compare(name.size() - 2, 2, "[]") == 0) {
                        nb::str base(name.c_str(),
                                     name.size() - 2);
                        if (!out.contains(base)) out[base] = nb::list();
                        nb::cast<nb::list>(out[base]).append(e.v);
                    } else {
                        out[nb::str(name.c_str(), name.size())] = e.v;
                    }
                    have = false;
                }
            }
            parent.push_back({std::move(out), false});
            return;
        }
        }
    }

    Checkpoint checkpoint() const override {
        return {levels_.size(), levels_.back().emitted.size()};
    }
    void rollback(Checkpoint cp) override {
        std::size_t depth = cp.depth < 1 ? 1 : cp.depth;
        while (levels_.size() > depth) levels_.pop_back();
        auto& emitted = levels_.back().emitted;
        if (cp.size <= emitted.size()) emitted.resize(cp.size);
    }
    Recording record_from(Checkpoint cp) const override {
        if (cp.depth < 1 || cp.depth > levels_.size()) return {};
        const auto& lvl = levels_[cp.depth - 1].emitted;
        if (cp.size > lvl.size()) return {};
        return std::make_shared<const std::vector<EV>>(
            lvl.begin() + cp.size, lvl.end());
    }
    void replay(const Recording& rec) override {
        if (!rec) return;
        const auto& evs = *static_cast<const std::vector<EV>*>(rec.get());
        for (const auto& e : evs) levels_.back().emitted.push_back(e);
    }

    nb::object result() const {
        if (levels_.front().emitted.empty()) return nb::none();
        return levels_.front().emitted.front().v;
    }
};


rawast::ValuePtr python_to_value(nb::handle obj) {
    using namespace rawast;
    // Undefined is a RETURN-ONLY sentinel: the engine may hand it back, but
    // it must not be fed back in as an input value. Reject it here (the one
    // path that turns Python values into the C++ tree) rather than silently
    // accepting it.
    if (obj.is(undefined_py())) {
        throw nb::type_error(
            "Undefined is a return-only sentinel; it cannot be assigned or "
            "used as an input value");
    }
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

// The direct-parse implementation behind the *_direct bindings (and, after
// promotion, every parse entry point): native Python objects materialise
// during the parse. Grammars with `#opchain` get the compaction as an
// event-stream decorator — fully representation-independent, no reference-
// model detour.
nb::object py_parse_direct(rawast::Grammar& g, rawast::Stream& stream,
                           const std::string* start) {
    PyObjectBuilder b;
    tl::expected<void, rawast::ParseError> r;
    if (g.has_any_opchain()) {
        rawast::CompactingBuilder cb(b, g);
        r = start ? g.parse_into(stream, cb, *start)
                  : g.parse_into(stream, cb);
        if (r) cb.finish();
    } else {
        r = start ? g.parse_into(stream, b, *start)
                  : g.parse_into(stream, b);
    }
    if (!r) throw std::runtime_error(format_parse_error(r.error()));
    return b.result();
}


// Convert the RESULT a Python `expr_eval` callback hands back into the
// engine's ValuePtr tri-state. Unlike python_to_value (the input/save
// path, which rejects Undefined), this is a return TO the engine, so
// Undefined is allowed: True/False -> bool, Undefined/None -> undefined,
// anything else -> Python truthiness.
rawast::ValuePtr expr_eval_result_to_value(nb::handle r) {
    if (r.is(undefined_py()) || r.is_none()) return rawast::undefined_value();
    if (nb::isinstance<nb::bool_>(r)) {
        return nb::cast<bool>(r) ? rawast::true_value() : rawast::false_value();
    }
    return nb::cast<bool>(nb::bool_(r)) ? rawast::true_value()
                                        : rawast::false_value();
}

// Install a Python `expr_eval` callback (or clear it, given None) onto a
// Preprocessor, wrapping it in the C++ adapter: the condition AST is handed
// over as a native Python value and the result is mapped back to the
// engine's ValuePtr tri-state. The callback is captured strongly here; the
// host breaks any pp<->callback cycle with a weakref on its side.
void install_py_expr_eval(rawast::Preprocessor& pp, nb::object cb) {
    if (cb.is_none()) {
        pp.set_expr_eval(nullptr);
        return;
    }
    auto held = nb::borrow<nb::object>(cb);
    pp.set_expr_eval([held](const rawast::ValuePtr& cond) -> rawast::ValuePtr {
        nb::gil_scoped_acquire gil;
        return expr_eval_result_to_value(held(value_to_python(cond)));
    });
}

} // namespace

NB_MODULE(_rawast, m) {
    // Register built-in parser groups so grammars opting in via
    // `"use": ["std", "gdsii"]` resolve correctly. Library-side
    // anonymous-namespace auto-registration is unreliable across
    // linker configurations; explicit init here is the durable path.
    rawast::register_std_parser_group();
    rawast::register_gdsii_parser_group();
    rawast::register_lefdef_parser_group();
    rawast::register_sv_parser_group();
    rawast::register_tcl_parser_group();

    // `Undefined` — a sentinel value distinct from `None`/null that some
    // projects use to mean "undefined" rather than "explicitly null". One
    // shared instance, like `None`, so `value is rawast.Undefined` holds.
    // It maps to/from the C++ UndefinedValue; no grammar produces it.
    nb::class_<UndefinedTag>(m, "UndefinedType")
        .def("__repr__", [](const UndefinedTag&) { return "Undefined"; })
        .def("__bool__", [](const UndefinedTag&) { return false; })
        .def("__copy__", [](nb::object self) { return self; })
        .def("__deepcopy__", [](nb::object self, nb::object) { return self; });
    // The module dict's `Undefined` attribute owns the one instance; the
    // handle is a non-owning view kept valid by that ownership.
    m.attr("Undefined") = nb::cast(UndefinedTag{});
    undefined_py() = m.attr("Undefined");

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
                return py_parse_direct(g, stream, nullptr);
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
                return py_parse_direct(g, stream, nullptr);
            },
            nb::arg("path"), nb::arg("preprocessor"),
            "Parse a file after running it through the given Preprocessor. "
            "Macro state, includes, and warnings accumulate on the Preprocessor "
            "across calls so it can be reused for a multi-file corpus.")

        .def("parse_file",
            [](rawast::Grammar& g, const std::string& path,
               const std::string& start) {
                auto stream = rawast::Stream::from_file(path);
                return py_parse_direct(g, stream, &start);
            },
            nb::arg("path"), nb::arg("start"),
            "Parse a file starting from the named rule instead of the grammar's "
            "default start. Used for re-parsing arbitrary strings through any "
            "rule in the grammar.")

        .def("parse_string",
            [](rawast::Grammar& g, const std::string& content) {
                auto stream = rawast::Stream::from_string(content);
                return py_parse_direct(g, stream, nullptr);
            },
            nb::arg("content"),
            "Parse a string from the grammar's default start.")

        .def("parse_string",
            [](rawast::Grammar& g, const std::string& content,
               rawast::Preprocessor& pp) {
                auto preprocessed = pp.process(content);
                auto stream = rawast::Stream::from_string(std::move(preprocessed));
                return py_parse_direct(g, stream, nullptr);
            },
            nb::arg("content"), nb::arg("preprocessor"),
            "Parse a string after running it through the given Preprocessor.")

        .def("parse_string",
            [](rawast::Grammar& g, const std::string& content,
               const std::string& start) {
                auto stream = rawast::Stream::from_string(content);
                return py_parse_direct(g, stream, &start);
            },
            nb::arg("content"), nb::arg("start"),
            "Parse a string starting from the named rule.")

        .def("parse_bytes",
            [](rawast::Grammar& g, nb::bytes b) {
                auto stream = rawast::Stream::from_string(std::string(b.c_str(), b.size()));
                return py_parse_direct(g, stream, nullptr);
            },
            nb::arg("data"),
            "Parse a bytes object from the grammar's default start. Use for "
            "binary grammars (GDSII, etc.).")

        .def("parse_bytes",
            [](rawast::Grammar& g, nb::bytes b, const std::string& start) {
                auto stream = rawast::Stream::from_string(std::string(b.c_str(), b.size()));
                return py_parse_direct(g, stream, &start);
            },
            nb::arg("data"), nb::arg("start"),
            "Parse a bytes object starting from the named rule.")

        // Parse a Stream directly. The canonical entry point — the
        // string/file/bytes overloads above are convenience wrappers
        // that build a Stream internally. Used by callers who already
        // hold a Stream (e.g. from Preprocessor.preprocess).
        .def("parse_stream",
            [](rawast::Grammar& g, rawast::Stream& stream) {
                return py_parse_direct(g, stream, nullptr);
            },
            nb::arg("stream"),
            "Parse a Stream from the grammar's default start. The Stream "
            "is consumed; reuse is undefined.")

        .def("parse_stream",
            [](rawast::Grammar& g, rawast::Stream& stream,
               const std::string& start) {
                return py_parse_direct(g, stream, &start);
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
    // systemverilog) and keyword-only behavior options. The class
    // holds policy + accumulating state; reuse one instance across many
    // process_file calls so macros, included_files, and warnings carry
    // through naturally.
    //
    // Lifetime: keep_alive<1, 2> ties the Grammar to the Preprocessor
    // instance — the Grammar must outlive the Preprocessor (the C++
    // class stores a const Grammar&).
    nb::class_<rawast::Preprocessor>(m, "Preprocessor",
        "Apply preprocessor semantics (macro expansion, conditional "
        "compilation, includes) to source text before parsing.",
        nb::dynamic_attr(),          // holds the expr_eval callable in __dict__
        nb::is_weak_referenceable()) // so a host can weakref it (cycle break)
        .def("__init__",
            [](rawast::Preprocessor* self, rawast::Grammar& g,
               const std::string& predefined,
               const std::vector<std::string>& include_paths,
               bool splice,
               const std::string& on_undefined,
               const std::string& on_undecidable,
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
                auto od = rawast::parse_pp_on_undecidable(on_undecidable);
                if (!od) {
                    throw std::runtime_error(
                        "Preprocessor: unknown on_undecidable '" +
                        on_undecidable + "' (valid: false, true, error)");
                }
                opts.on_undecidable = *od;
                opts.max_expansion_depth = max_expansion_depth;
                opts.trace = trace;
                // expr_eval is wired AFTER construction via the `expr_eval`
                // property so the callback can refer back to this instance
                // (with a weakref) without a construct-time chicken-and-egg.
                new (self) rawast::Preprocessor(g, std::move(opts));
            },
            nb::arg("grammar"),
            nb::arg("predefined") = std::string{},
            nb::arg("include_paths") = std::vector<std::string>{},
            nb::arg("splice") = false,
            nb::arg("on_undefined") = std::string{"leave"},
            nb::arg("on_undecidable") = std::string{"false"},
            nb::arg("max_expansion_depth") = 200,
            nb::arg("trace") = false,
            nb::keep_alive<1, 2>(),
            "Construct a Preprocessor. The grammar should be a loaded "
            "preprocessor grammar (e.g. systemverilog.rawast). All "
            "behavior options are keyword-only; defaults match the "
            "documented spec.")

        // The `\`if`/`\`elsif` condition evaluator. Settable AFTER
        // construction so the callback can refer back to this Preprocessor
        // (use a weakref to avoid a pp<->callback reference cycle). The
        // callable receives the condition AST as a Python value and returns
        // bool | rawast.Undefined | None (None == undecidable). Stored in
        // the instance __dict__ so the cyclic GC can see it.
        .def_prop_rw("expr_eval",
            [](nb::handle self) -> nb::object {
                return nb::hasattr(self, "_expr_eval_cb")
                           ? self.attr("_expr_eval_cb") : nb::none();
            },
            [](nb::handle self, nb::object cb) {
                self.attr("_expr_eval_cb") = cb;
                install_py_expr_eval(nb::cast<rawast::Preprocessor&>(self), cb);
            })

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

    m.attr("__version__") = "0.1.11";
}
