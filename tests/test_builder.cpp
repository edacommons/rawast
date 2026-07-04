// Unit tests for the universal Builder seam.
//
// The Builder interface is TYPED: plug-in representations implement only
// null_/bool_/int_/uint_/real_/string_ + begin/end + checkpoint/rollback +
// record/replay, and never see a ValuePtr. adopt() has a default that
// translates a reference-model subtree into typed events; SharedPtrBuilder
// overrides it for zero-copy adoption and owns interning.

#include <doctest/doctest.h>

#include <rawast/builder.hpp>

#include <rawast/grammar.hpp>
#include <rawast/pool.hpp>
#include <rawast/stream.hpp>
#include <rawast/value.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace rawast;

namespace {

std::int64_t pick_int(const ValuePtr& v) {
    auto i = as_int(v);
    REQUIRE(i);
    return i->data();
}
std::string pick_str(const ValuePtr& v) {
    auto s = as_string(v);
    REQUIRE(s);
    return s->data();
}
std::shared_ptr<ArrayValue> pick_arr(const ValuePtr& v) {
    auto a = as_array(v);
    REQUIRE(a);
    return a;
}
std::shared_ptr<DictValue> pick_dict(const ValuePtr& v) {
    auto d = as_dict(v);
    REQUIRE(d);
    return d;
}

} // namespace

TEST_CASE("SharedPtrBuilder: typed scalars build an array") {
    ValuePool pool;
    SharedPtrBuilder b(pool);
    b.begin(Container::Array);
    b.int_(1, false);
    b.string_("x", false);
    b.bool_(true, false);
    b.real_(2.5, false);
    b.end();

    auto arr = pick_arr(b.result());
    REQUIRE(arr->data().size() == 4);
    CHECK(pick_int(arr->data()[0]) == 1);
    CHECK(pick_str(arr->data()[1]) == "x");
    CHECK(arr->data()[2]->type() == ValueType::Bool);
    CHECK(arr->data()[3]->type() == ValueType::Real);
}

TEST_CASE("SharedPtrBuilder: interning is the builder's job") {
    ValuePool pool;
    SharedPtrBuilder b(pool);
    b.begin(Container::Array);
    b.string_("clk", false);
    b.string_("clk", false);
    b.int_(42, false);
    b.int_(42, false);
    b.end();

    auto arr = pick_arr(b.result());
    REQUIRE(arr->data().size() == 4);
    CHECK(arr->data()[0].get() == arr->data()[1].get());   // shared canonical
    CHECK(arr->data()[2].get() == arr->data()[3].get());
}

TEST_CASE("SharedPtrBuilder: dict via typed name markers + name[] append") {
    ValuePool pool;
    SharedPtrBuilder b(pool);
    b.begin(Container::Dict);
    b.string_("a", true);              // key
    b.int_(1, false);
    b.string_("items[]", true);        // list-append marker
    b.int_(10, false);
    b.string_("items[]", true);
    b.int_(20, false);
    b.end();

    auto d = pick_dict(b.result());
    CHECK(pick_int(d->data().at("a")) == 1);
    auto items = pick_arr(d->data().at("items"));
    REQUIRE(items->data().size() == 2);
    CHECK(pick_int(items->data()[1]) == 20);
}

TEST_CASE("SharedPtrBuilder: checkpoint/rollback discards a failed alternative") {
    ValuePool pool;
    SharedPtrBuilder b(pool);
    b.begin(Container::Array);
    b.int_(1, false);
    auto cp = b.checkpoint();
    b.begin(Container::Dict);          // tentative alternative...
    b.string_("k", true);
    b.int_(99, false);
    b.rollback(cp);                    // ...rejected mid-container
    b.int_(2, false);
    b.end();

    auto arr = pick_arr(b.result());
    REQUIRE(arr->data().size() == 2);
    CHECK(pick_int(arr->data()[1]) == 2);
}

TEST_CASE("SharedPtrBuilder: opaque record/replay reproduces a subtree") {
    ValuePool pool;
    SharedPtrBuilder b(pool);
    b.begin(Container::Array);
    auto cp = b.checkpoint();
    b.begin(Container::Dict);
    b.string_("k", true);
    b.int_(7, false);
    b.end();
    Builder::Recording rec = b.record_from(cp);   // opaque token
    b.replay(rec);
    b.end();

    auto arr = pick_arr(b.result());
    REQUIRE(arr->data().size() == 2);
    CHECK(arr->data()[0].get() == arr->data()[1].get());  // shared replay
}

TEST_CASE("SharedPtrBuilder: adopt is zero-copy for composites, interns scalars") {
    ValuePool pool;
    SharedPtrBuilder b(pool);
    auto sub = make_dict();
    as_dict(ValuePtr(sub))->data()["n"] = make_int(5);
    b.begin(Container::Array);
    b.adopt(sub, false);               // composite: shared as-is
    b.string_("dup", false);
    b.adopt(make_string("dup"), false); // scalar: interns to the canonical
    b.end();

    auto arr = pick_arr(b.result());
    REQUIRE(arr->data().size() == 3);
    CHECK(arr->data()[0].get() == sub.get());
    CHECK(arr->data()[1].get() == arr->data()[2].get());
}

// A minimal test-only plug-in representation: records the event stream as
// a flat parenthesized trace string. Implements ONLY the typed surface —
// proving a plug-in never needs ValuePtr, that the default adopt()
// translation delivers reference-model subtrees as ordinary events, and
// that the stream carries the pure value model (it caught the
// Container::None plumbing leak).
namespace {
struct EventTraceBuilder final : Builder {
    std::string out;
    std::vector<std::size_t> marks;
    void null_(bool) override { out += "() "; }
    void bool_(bool v, bool) override { out += v ? "#t " : "#f "; }
    void int_(std::int64_t v, bool) override { out += std::to_string(v) + " "; }
    void uint_(std::uint64_t v, bool) override { out += std::to_string(v) + " "; }
    void real_(double, bool) override { out += "r "; }
    void string_(std::string_view v, bool is_name) override {
        out += std::string(v) + (is_name ? ": " : " ");
    }
    void begin(Container) override { out += "( "; }
    void end() override { out += ") "; }
    Checkpoint checkpoint() const override { return {1, out.size()}; }
    void rollback(Checkpoint cp) override { out.resize(cp.size); }
    Recording record_from(Checkpoint cp) const override {
        return std::make_shared<const std::string>(out.substr(cp.size));
    }
    void replay(const Recording& r) override {
        if (r) out += *static_cast<const std::string*>(r.get());
    }
};
} // namespace

TEST_CASE("plug-in builder: typed surface only; default adopt translates") {
    EventTraceBuilder b;
    b.begin(Container::Array);
    b.int_(1, false);
    // Hand it a reference-model subtree: the DEFAULT adopt must deliver
    // it as typed events (the plug-in has no ValuePtr knowledge).
    auto sub = make_dict();
    as_dict(ValuePtr(sub))->data()["k"] = make_int(9);
    b.adopt(sub, false);
    b.end();
    CHECK(b.out == "( 1 ( k: 9 ) ) ");
}

TEST_CASE("parse_into: a plug-in representation receives a real parse") {
    auto g = make_json_grammar();
    auto stream = Stream::from_string(R"({"n": 42, "xs": [1, 2]})");
    EventTraceBuilder b;
    auto r = g.parse_into(stream, b);
    REQUIRE(r);
    // dict + array structure delivered purely through typed events
    CHECK(b.out == "( n: 42 xs: ( 1 2 ) ) ");
}

TEST_CASE("parse_into: parse failure reaches the caller, builder untouched by commit") {
    auto g = make_json_grammar();
    auto stream = Stream::from_string("{broken");
    EventTraceBuilder b;
    auto r = g.parse_into(stream, b);
    CHECK_FALSE(r);
}

// ---------------------------------------------------------------------------
// Accessor + convert + save_from — the read half of the representation pair
// ---------------------------------------------------------------------------

#include <rawast/accessor.hpp>
#include <sstream>

TEST_CASE("convert: SharedPtrAccessor -> SharedPtrBuilder reproduces the tree") {
    auto g = make_json_grammar();
    auto stream = Stream::from_string(R"({"n": 42, "xs": [1, {"k": "v"}], "b": true})");
    auto ast = g.parse(stream);
    REQUIRE(ast);

    SharedPtrAccessor acc(*ast);
    ValuePool pool;
    SharedPtrBuilder b(pool);
    convert(acc, b);
    CHECK(value_equal(b.result(), *ast));
}

TEST_CASE("convert: Accessor -> plug-in builder delivers the value model") {
    auto g = make_json_grammar();
    auto stream = Stream::from_string(R"({"n": 42, "xs": [1, 2]})");
    auto ast = g.parse(stream);
    REQUIRE(ast);

    SharedPtrAccessor acc(*ast);
    EventTraceBuilder b;
    convert(acc, b);
    CHECK(b.out == "( n: 42 xs: ( 1 2 ) ) ");
}

TEST_CASE("save_from: reference fast path == save") {
    auto g = make_json_grammar();
    auto stream = Stream::from_string(R"({"a": [1, 2], "s": "hi"})");
    auto ast = g.parse(stream);
    REQUIRE(ast);

    std::ostringstream direct, via;
    REQUIRE(g.save(direct, *ast));
    SharedPtrAccessor acc(*ast);
    REQUIRE(g.save_from(via, acc));
    CHECK(via.str() == direct.str());
}

namespace {
// Forwarding accessor that is NOT a SharedPtrAccessor — forces save_from's
// foreign-representation path (convert into the reference model, then save).
struct ForeignAccessor final : Accessor {
    const SharedPtrAccessor inner;
    explicit ForeignAccessor(ValuePtr r) : inner(std::move(r)) {}
    Node root() const override { return inner.root(); }
    ValueType kind(Node n) const override { return inner.kind(n); }
    bool bool_(Node n) const override { return inner.bool_(n); }
    std::int64_t int_(Node n) const override { return inner.int_(n); }
    std::uint64_t uint_(Node n) const override { return inner.uint_(n); }
    double real_(Node n) const override { return inner.real_(n); }
    std::string_view string_(Node n) const override { return inner.string_(n); }
    std::size_t size(Node n) const override { return inner.size(n); }
    Node at(Node n, std::size_t i) const override { return inner.at(n, i); }
    Node get(Node n, std::string_view k) const override { return inner.get(n, k); }
    void each(Node n, const std::function<void(std::string_view, Node)>& f)
        const override { inner.each(n, f); }
};
} // namespace

TEST_CASE("save_from: a foreign representation round-trips through convert") {
    auto g = make_json_grammar();
    auto stream = Stream::from_string(R"({"a": [1, 2], "s": "hi", "t": true})");
    auto ast = g.parse(stream);
    REQUIRE(ast);

    std::ostringstream direct, via;
    REQUIRE(g.save(direct, *ast));
    ForeignAccessor acc(*ast);
    REQUIRE(g.save_from(via, acc));
    CHECK(via.str() == direct.str());

    // and the full circle: saved text reparses to the identical AST
    auto s2 = Stream::from_string(via.str());
    auto back = g.parse(s2);
    REQUIRE(back);
    CHECK(value_equal(*back, *ast));
}

// ---------------------------------------------------------------------------
// compact_opchain_into — the universal #opchain pass
// ---------------------------------------------------------------------------

#include <rawast/loader.hpp>
#include <rawast/parsers.hpp>
#include <rawast/parsers_sv.hpp>

TEST_CASE("compact_opchain_into: raw parse_into stream + pass == compacted parse()") {
    register_std_parser_group();
    register_sv_parser_group();
    Grammar g;
    REQUIRE(load_rawast_grammar_from_file(g, "grammars/systemverilog.rawast"));
    const std::string src = "module m;\n  assign y = a + b + c | d;\nendmodule\n";

    // Reference path: parse() applies the #opchain compaction.
    auto s1 = Stream::from_string(src);
    auto compacted = g.parse(s1);
    REQUIRE(compacted);

    // Plug-in-style path: parse_into applies NO compaction (raw
    // always-wrap {lhs,tail} stream) — then the universal pass folds it.
    ValuePool pool;
    SharedPtrBuilder raw(pool);
    auto s2 = Stream::from_string(src);
    REQUIRE(g.parse_into(s2, raw));
    CHECK_FALSE(value_equal(raw.result(), *compacted));   // truly uncompacted

    SharedPtrAccessor acc(raw.result());
    ValuePool pool2;
    SharedPtrBuilder folded(pool2);
    g.compact_opchain_into(acc, folded);
    CHECK(value_equal(folded.result(), *compacted));
}

TEST_CASE("compact_opchain_into: no-opchain grammar degenerates to convert") {
    auto g = make_json_grammar();
    auto s = Stream::from_string(R"({"a": [1, 2]})");
    auto ast = g.parse(s);
    REQUIRE(ast);
    SharedPtrAccessor acc(*ast);
    ValuePool pool;
    SharedPtrBuilder out(pool);
    g.compact_opchain_into(acc, out);
    CHECK(value_equal(out.result(), *ast));
}

// ---------------------------------------------------------------------------
// Representation bundles — parse_as<R> / save_as<R>
// ---------------------------------------------------------------------------

#include <rawast/representation.hpp>

TEST_CASE("parse_as<SharedPtrRepr> == parse(), including #opchain compaction") {
    register_std_parser_group();
    register_sv_parser_group();
    Grammar g;
    REQUIRE(load_rawast_grammar_from_file(g, "grammars/systemverilog.rawast"));
    const std::string src = "module m;\n  assign y = a + b + c | d;\nendmodule\n";

    auto s1 = Stream::from_string(src);
    auto via_parse = g.parse(s1);
    REQUIRE(via_parse);

    auto s2 = Stream::from_string(src);
    auto via_bundle = g.parse_as<SharedPtrRepr>(s2);
    REQUIRE(via_bundle);
    CHECK(value_equal(*via_bundle, *via_parse));   // compacted identically
}

TEST_CASE("parse_as + save_as round-trip through the bundle") {
    auto g = make_json_grammar();
    auto s = Stream::from_string(R"({"a": [1, 2], "s": "hi"})");
    auto doc = g.parse_as<SharedPtrRepr>(s);
    REQUIRE(doc);

    std::ostringstream direct, via;
    REQUIRE(g.save(direct, *doc));
    REQUIRE(g.save_as<SharedPtrRepr>(via, *doc));
    CHECK(via.str() == direct.str());

    auto s2 = Stream::from_string(via.str());
    auto back = g.parse_as<SharedPtrRepr>(s2);
    REQUIRE(back);
    CHECK(value_equal(*back, *doc));
}

// ---------------------------------------------------------------------------
// CompactingBuilder — #opchain compaction as an event-stream decorator
// ---------------------------------------------------------------------------

#include <rawast/compacting_builder.hpp>

TEST_CASE("CompactingBuilder: decorated plug-in stream == parse()'s compacted AST") {
    register_std_parser_group();
    register_sv_parser_group();
    Grammar g;
    REQUIRE(load_rawast_grammar_from_file(g, "grammars/systemverilog.rawast"));
    const std::string src =
        "module m;\n"
        "  assign y = a + b + c | d && e;\n"          // multi-tier chain
        "  assign z = (&a) & b;\n"                     // unary-reduction guard
        "  assign w = q inside {1, 2};\n"              // non-rhs tail bailout
        "endmodule\n";

    auto s1 = Stream::from_string(src);
    auto compacted = g.parse(s1);
    REQUIRE(compacted);

    // Any-representation path: raw events -> decorator -> builder.
    SharedPtrBuilder inner;                    // stands in for any plug-in
    CompactingBuilder cb(inner, g);
    auto s2 = Stream::from_string(src);
    REQUIRE(g.parse_into(s2, cb));
    cb.finish();
    CHECK(value_equal(inner.result(), *compacted));
}

TEST_CASE("CompactingBuilder: backtracking is absorbed by the buffer") {
    register_std_parser_group();
    register_sv_parser_group();
    Grammar g;
    REQUIRE(load_rawast_grammar_from_file(g, "grammars/systemverilog.rawast"));
    // Heavy Choice retries (expression ladder) — inner must still receive
    // exactly one clean, folded document.
    const std::string src = "module m;\n  assign y = (a + b) * (c - d);\nendmodule\n";
    auto s1 = Stream::from_string(src);
    auto expect = g.parse(s1);
    REQUIRE(expect);
    SharedPtrBuilder inner;
    CompactingBuilder cb(inner, g);
    auto s2 = Stream::from_string(src);
    REQUIRE(g.parse_into(s2, cb));
    cb.finish();
    CHECK(value_equal(inner.result(), *expect));
}
