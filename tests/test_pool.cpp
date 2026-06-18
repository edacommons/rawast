#include <doctest/doctest.h>
#include <rawast/grammar.hpp>
#include <rawast/pool.hpp>

#include <sstream>

using namespace rawast;

// Pool basics --------------------------------------------------------------

TEST_CASE("ValuePool dedups equal strings to the same canonical pointer") {
    ValuePool pool;
    auto a = pool.intern(make_string("hello"));
    auto b = pool.intern(make_string("hello"));
    CHECK(a.get() == b.get());
}

TEST_CASE("ValuePool dedups equal ints") {
    ValuePool pool;
    auto a = pool.intern(make_int(42));
    auto b = pool.intern(make_int(42));
    auto c = pool.intern(make_int(-17));
    CHECK(a.get() == b.get());
    CHECK(a.get() != c.get());
}

TEST_CASE("ValuePool dedups equal uints") {
    ValuePool pool;
    auto a = pool.intern(make_uint(42));
    auto b = pool.intern(make_uint(42));
    CHECK(a.get() == b.get());
}

TEST_CASE("ValuePool dedups equal reals") {
    ValuePool pool;
    auto a = pool.intern(make_real(3.14));
    auto b = pool.intern(make_real(3.14));
    auto c = pool.intern(make_real(2.71));
    CHECK(a.get() == b.get());
    CHECK(a.get() != c.get());
}

TEST_CASE("ValuePool returns null/true/false singletons unchanged") {
    ValuePool pool;
    auto n = pool.intern(null_value());
    auto t = pool.intern(true_value());
    auto f = pool.intern(false_value());
    CHECK(n.get() == null_value().get());
    CHECK(t.get() == true_value().get());
    CHECK(f.get() == false_value().get());
}

TEST_CASE("ValuePool size counts unique primitive entries") {
    ValuePool pool;
    CHECK(pool.size() == 0);
    pool.intern(make_string("hello"));
    pool.intern(make_string("hello"));    // dedup, no growth
    pool.intern(make_string("world"));
    pool.intern(make_int(1));
    pool.intern(make_int(2));
    pool.intern(make_int(1));             // dedup
    CHECK(pool.size() == 4);              // 2 strings + 2 ints
}

TEST_CASE("ValuePool intern_X convenience helpers") {
    ValuePool pool;
    auto a = pool.intern_string("clk");
    auto b = pool.intern_string("clk");
    CHECK(a.get() == b.get());

    auto i = pool.intern_int(7);
    auto j = pool.intern_int(7);
    CHECK(i.get() == j.get());
}

// Back-references ---------------------------------------------------------

TEST_CASE("register_usage records a container for a value") {
    ValuePool pool;
    auto v = pool.intern_string("clk");
    auto container = make_array();
    pool.register_usage(v, container);

    auto refs = pool.find_containers_of(v);
    REQUIRE(refs.size() == 1);
    CHECK(refs[0].get() == container.get());
}

TEST_CASE("register_usage tracks multiple containers") {
    ValuePool pool;
    auto v = pool.intern_string("clk");
    auto c1 = make_array();
    auto c2 = make_dict();
    auto c3 = make_array();
    pool.register_usage(v, c1);
    pool.register_usage(v, c2);
    pool.register_usage(v, c3);

    auto refs = pool.find_containers_of(v);
    CHECK(refs.size() == 3);
}

TEST_CASE("find_containers_of returns empty for an unregistered value") {
    ValuePool pool;
    auto v = pool.intern_string("never-registered");
    auto refs = pool.find_containers_of(v);
    CHECK(refs.empty());
}

// Pool integration with Grammar::parse ------------------------------------

TEST_CASE("Grammar::parse interns repeated string values across a dict") {
    auto g = make_json_grammar();
    ValuePool pool;
    auto stream = Stream::from_string(R"({"a": "x", "b": "x", "c": "x"})");
    auto r = g.parse(stream, pool);
    REQUIRE(r);
    auto d = std::dynamic_pointer_cast<DictValue>(*r);
    REQUIRE(d);

    auto va = d->data().at("a");
    auto vb = d->data().at("b");
    auto vc = d->data().at("c");
    CHECK(va.get() == vb.get());
    CHECK(vb.get() == vc.get());
}

TEST_CASE("Grammar::parse interns repeated int values across an array") {
    auto g = make_json_grammar();
    ValuePool pool;
    auto stream = Stream::from_string("[7, 7, 7, 8, 8, 9]");
    auto r = g.parse(stream, pool);
    REQUIRE(r);
    auto arr = std::dynamic_pointer_cast<ArrayValue>(*r);
    REQUIRE(arr);
    REQUIRE(arr->data().size() == 6);

    // Three sevens share one canonical ValuePtr.
    CHECK(arr->data()[0].get() == arr->data()[1].get());
    CHECK(arr->data()[1].get() == arr->data()[2].get());
    // Eights share one, distinct from seven.
    CHECK(arr->data()[3].get() == arr->data()[4].get());
    CHECK(arr->data()[0].get() != arr->data()[3].get());
    // Pool has three unique primitives (7, 8, 9).
    CHECK(pool.size() == 3);
}

TEST_CASE("Grammar::parse populates back-references via the pool") {
    auto g = make_json_grammar();
    ValuePool pool;
    auto stream = Stream::from_string(R"({"a": "clk", "b": "clk"})");
    auto r = g.parse(stream, pool);
    REQUIRE(r);

    // Find every container that holds the "clk" string.
    auto clk = pool.intern_string("clk");
    auto refs = pool.find_containers_of(clk);
    // Both "a" and "b" entries refer to the same canonical "clk", and the
    // top-level dict registered each usage separately, so we expect two
    // back-refs even though the container is the same dict.
    CHECK(refs.size() == 2);
}

TEST_CASE("Grammar::parse — pool reports unique primitive count") {
    auto g = make_json_grammar();
    ValuePool pool;
    auto stream = Stream::from_string(R"({"name": "clk", "frequency": 100, "type": "clock", "period": 10})");
    auto r = g.parse(stream, pool);
    REQUIRE(r);

    // Unique primitives in this input:
    //   strings: "name", "clk", "frequency", "type", "clock", "period" = 6
    //   ints:    100, 10                                                = 2
    CHECK(pool.size() == 8);
}
