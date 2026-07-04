#pragma once

#include <rawast/accessor.hpp>
#include <rawast/builder.hpp>
#include <rawast/value.hpp>

#include <utility>

namespace rawast {

// A representation bundle ties a {Builder, Accessor} pair to its owning
// Document type, for the call-site sugar Grammar::parse_as<R> /
// Grammar::save_as<R>. Duck-typed contract (C++17 — enforced by
// static_asserts at the use sites):
//
//   R::Builder    — models the Builder interface
//   R::Accessor   — models the Accessor interface
//   R::Document   — the owning product type
//   static Document take(Builder&)          — extract the finished product
//   static Accessor read(const Document&)   — a read view over a product
//
// Policy stays in the app: which grammar/format uses which bundle is a
// one-token decision at each call site.

// The reference bundle: the classic shared_ptr<Value> AST.
struct SharedPtrRepr {
    using Builder  = SharedPtrBuilder;
    using Accessor = SharedPtrAccessor;
    using Document = ValuePtr;

    static Document take(Builder& b) { return b.result(); }
    static Accessor read(const Document& d) { return SharedPtrAccessor(d); }
};

} // namespace rawast
