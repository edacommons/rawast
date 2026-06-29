// Unit tests for the generic Builder seam + SharedPtrBuilder.
//
// SharedPtrBuilder must reproduce Frame::finish's materialisation exactly
// (array/dict assembly, the `name[]` list-append marker, backtracking). These
// drive the builder directly; wiring it into the parse driver is a separate,
// corpus-gated step.

#include <doctest/doctest.h>

#include "../src/builder.hpp"

#include <rawast/pool.hpp>
#include <rawast/value.hpp>

#include <memory>

using namespace rawast;

namespace {

std::int64_t pick_int(const ValuePtr& v) {
    auto i = std::dynamic_pointer_cast<IntValue>(v);
    REQUIRE(i);
    return i->data();
}
std::string pick_str(const ValuePtr& v) {
    auto s = std::dynamic_pointer_cast<StringValue>(v);
    REQUIRE(s);
    return s->data();
}
std::shared_ptr<ArrayValue> pick_arr(const ValuePtr& v) {
    auto a = std::dynamic_pointer_cast<ArrayValue>(v);
    REQUIRE(a);
    return a;
}
std::shared_ptr<DictValue> pick_dict(const ValuePtr& v) {
    auto d = std::dynamic_pointer_cast<DictValue>(v);
    REQUIRE(d);
    return d;
}

} // namespace

TEST_CASE("SharedPtrBuilder: array of scalars") {
    ValuePool pool;
    SharedPtrBuilder b(pool);
    b.begin(Container::Array);
    b.value(make_int(1), false);
    b.value(make_int(2), false);
    b.value(make_int(3), false);
    b.end();

    auto arr = pick_arr(b.result());
    REQUIRE(arr->data().size() == 3);
    CHECK(pick_int(arr->data()[0]) == 1);
    CHECK(pick_int(arr->data()[2]) == 3);
}

TEST_CASE("SharedPtrBuilder: dict with nested array + scalar (name markers)") {
    ValuePool pool;
    SharedPtrBuilder b(pool);
    b.begin(Container::Dict);
    b.value(make_string("a"), true);          // key
    b.begin(Container::Array);                 // value of "a"
    b.value(make_int(1), false);
    b.value(make_int(2), false);
    b.end();
    b.value(make_string("b"), true);          // key
    b.value(make_string("x"), false);         // value of "b"
    b.end();

    auto d = pick_dict(b.result());
    REQUIRE(d->data().count("a"));
    REQUIRE(d->data().count("b"));
    auto a = pick_arr(d->data().at("a"));
    CHECK(a->data().size() == 2);
    CHECK(pick_int(a->data()[0]) == 1);
    CHECK(pick_str(d->data().at("b")) == "x");
}

TEST_CASE("SharedPtrBuilder: `name[]` list-append marker collects into a list") {
    ValuePool pool;
    SharedPtrBuilder b(pool);
    b.begin(Container::Dict);
    b.value(make_string("items[]"), true);
    b.value(make_int(10), false);
    b.value(make_string("items[]"), true);
    b.value(make_int(20), false);
    b.value(make_string("items[]"), true);
    b.value(make_int(30), false);
    b.end();

    auto d = pick_dict(b.result());
    REQUIRE(d->data().count("items"));      // the `[]` suffix is stripped
    REQUIRE_FALSE(d->data().count("items[]"));
    auto items = pick_arr(d->data().at("items"));
    REQUIRE(items->data().size() == 3);
    CHECK(pick_int(items->data()[0]) == 10);
    CHECK(pick_int(items->data()[2]) == 30);
}

TEST_CASE("SharedPtrBuilder: checkpoint/rollback discards a failed alternative") {
    ValuePool pool;
    SharedPtrBuilder b(pool);
    b.begin(Container::Array);
    b.value(make_int(1), false);
    auto cp = b.checkpoint();
    // a tentative alternative that opens a nested container then fails:
    b.begin(Container::Dict);
    b.value(make_string("k"), true);
    b.value(make_int(99), false);
    b.rollback(cp);                         // reject — even mid-container
    b.value(make_int(2), false);            // the alternative that succeeds
    b.end();

    auto arr = pick_arr(b.result());
    REQUIRE(arr->data().size() == 2);
    CHECK(pick_int(arr->data()[0]) == 1);
    CHECK(pick_int(arr->data()[1]) == 2);
}

TEST_CASE("SharedPtrBuilder: Container::None passes children through") {
    ValuePool pool;
    SharedPtrBuilder b(pool);
    b.begin(Container::Array);
    b.value(make_int(1), false);
    b.begin(Container::None);               // transparent group
    b.value(make_int(2), false);
    b.value(make_int(3), false);
    b.end();                                // 2,3 flow up into the array
    b.value(make_int(4), false);
    b.end();

    auto arr = pick_arr(b.result());
    REQUIRE(arr->data().size() == 4);
    CHECK(pick_int(arr->data()[1]) == 2);
    CHECK(pick_int(arr->data()[3]) == 4);
}
