#include <rawast/accessor.hpp>
#include <rawast/builder.hpp>
#include <rawast/node.hpp>

namespace rawast {

void convert(const Accessor& a, Accessor::Node n, Builder& b, bool is_name) {
    if (!n) { b.null_(is_name); return; }
    switch (a.kind(n)) {
    case ValueType::Null:
    case ValueType::Undefined: b.null_(is_name); return;
    case ValueType::Bool:      b.bool_(a.bool_(n), is_name); return;
    case ValueType::Int:       b.int_(a.int_(n), is_name); return;
    case ValueType::UInt:      b.uint_(a.uint_(n), is_name); return;
    case ValueType::Real:      b.real_(a.real_(n), is_name); return;
    case ValueType::String:    b.string_(a.string_(n), is_name); return;
    case ValueType::Array: {
        b.begin(Container::Array);
        const std::size_t count = a.size(n);
        for (std::size_t i = 0; i < count; ++i)
            convert(a, a.at(n, i), b, false);
        b.end();
        return;
    }
    case ValueType::Dict: {
        b.begin(Container::Dict);
        a.each(n, [&](std::string_view k, Accessor::Node v) {
            b.string_(k, true);
            convert(a, v, b, false);
        });
        b.end();
        return;
    }
    }
}

void convert(const Accessor& a, Builder& b) {
    convert(a, a.root(), b, false);
}

} // namespace rawast
