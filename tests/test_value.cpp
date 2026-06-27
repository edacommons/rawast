#include <doctest/doctest.h>
#include <rawast/value.hpp>

#include <memory>

using namespace rawast;

TEST_CASE("Value singletons are stable identities") {
    CHECK(null_value().get() == null_value().get());
    CHECK(true_value().get() == true_value().get());
    CHECK(false_value().get() == false_value().get());
    CHECK(true_value().get() != false_value().get());
    CHECK(true_value().get() != null_value().get());
}

TEST_CASE("Value type tags match concrete classes") {
    CHECK(null_value()->type()   == ValueType::Null);
    CHECK(true_value()->type()   == ValueType::Bool);
    CHECK(false_value()->type()  == ValueType::Bool);
    CHECK(make_int(0)->type()    == ValueType::Int);
    CHECK(make_uint(0)->type()   == ValueType::UInt);
    CHECK(make_real(0.0)->type() == ValueType::Real);
    CHECK(make_string("x")->type() == ValueType::String);
    CHECK(make_array()->type()   == ValueType::Array);
    CHECK(make_dict()->type()    == ValueType::Dict);
}

TEST_CASE("Bool values carry the right payload") {
    auto t = std::dynamic_pointer_cast<BoolValue>(true_value());
    auto f = std::dynamic_pointer_cast<BoolValue>(false_value());
    REQUIRE(t);
    REQUIRE(f);
    CHECK(t->data() == true);
    CHECK(f->data() == false);
}

TEST_CASE("Numeric values carry their payload") {
    auto i = std::dynamic_pointer_cast<IntValue>(make_int(-42));
    REQUIRE(i);
    CHECK(i->data() == -42);

    auto u = std::dynamic_pointer_cast<UIntValue>(make_uint(42));
    REQUIRE(u);
    CHECK(u->data() == 42u);

    auto r = std::dynamic_pointer_cast<RealValue>(make_real(3.14));
    REQUIRE(r);
    CHECK(r->data() == doctest::Approx(3.14));
}

TEST_CASE("String value carries its payload") {
    auto s = std::dynamic_pointer_cast<StringValue>(make_string("hello"));
    REQUIRE(s);
    CHECK(s->data() == "hello");
}

TEST_CASE("Array can hold mixed-type children") {
    auto arr = std::dynamic_pointer_cast<ArrayValue>(make_array());
    REQUIRE(arr);
    arr->data().push_back(make_int(1));
    arr->data().push_back(make_string("two"));
    arr->data().push_back(null_value());

    CHECK(arr->data().size() == 3);
    CHECK(arr->data()[0]->type() == ValueType::Int);
    CHECK(arr->data()[1]->type() == ValueType::String);
    CHECK(arr->data()[2]->type() == ValueType::Null);
}

TEST_CASE("Dict can hold keyed children") {
    auto d = std::dynamic_pointer_cast<DictValue>(make_dict());
    REQUIRE(d);
    d->data().emplace("answer", make_int(42));
    d->data().emplace("greeting", make_string("hi"));

    CHECK(d->data().size() == 2);
    CHECK(d->data().at("answer")->type()   == ValueType::Int);
    CHECK(d->data().at("greeting")->type() == ValueType::String);
}

TEST_CASE("Undefined is a distinct singleton value") {
    auto u = undefined_value();
    REQUIRE(u);
    CHECK(u->type() == ValueType::Undefined);
    // Singleton: same pointer every time...
    CHECK(undefined_value().get() == u.get());
    // ...and distinct from null (different type and identity).
    CHECK(undefined_value().get() != null_value().get());
    CHECK(u->type() != ValueType::Null);
}
