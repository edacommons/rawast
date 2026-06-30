#include <rawast/value.hpp>

namespace rawast {

ValuePtr null_value() {
    static const ValuePtr instance = std::make_shared<NullValue>();
    return instance;
}

ValuePtr undefined_value() {
    static const ValuePtr instance = std::make_shared<UndefinedValue>();
    return instance;
}

ValuePtr true_value() {
    static const ValuePtr instance = std::make_shared<BoolValue>(true);
    return instance;
}

ValuePtr false_value() {
    static const ValuePtr instance = std::make_shared<BoolValue>(false);
    return instance;
}

bool value_equal(const ValuePtr& a, const ValuePtr& b) {
    if (a.get() == b.get()) return true;     // same ptr (incl. both null)
    if (!a || !b) return false;
    if (a->type() != b->type()) return false;
    switch (a->type()) {
    case ValueType::Null:
    case ValueType::Undefined:
        return true;
    case ValueType::Bool:
        return static_cast<const BoolValue&>(*a).data()
             == static_cast<const BoolValue&>(*b).data();
    case ValueType::Int:
        return static_cast<const IntValue&>(*a).data()
             == static_cast<const IntValue&>(*b).data();
    case ValueType::UInt:
        return static_cast<const UIntValue&>(*a).data()
             == static_cast<const UIntValue&>(*b).data();
    case ValueType::Real:
        return static_cast<const RealValue&>(*a).data()
             == static_cast<const RealValue&>(*b).data();
    case ValueType::String:
        return static_cast<const StringValue&>(*a).data()
             == static_cast<const StringValue&>(*b).data();
    case ValueType::Array: {
        const auto& av = static_cast<const ArrayValue&>(*a).data();
        const auto& bv = static_cast<const ArrayValue&>(*b).data();
        if (av.size() != bv.size()) return false;
        for (std::size_t i = 0; i < av.size(); ++i)
            if (!value_equal(av[i], bv[i])) return false;
        return true;
    }
    case ValueType::Dict: {
        const auto& am = static_cast<const DictValue&>(*a).data();
        const auto& bm = static_cast<const DictValue&>(*b).data();
        if (am.size() != bm.size()) return false;
        auto ai = am.begin();
        auto bi = bm.begin();
        for (; ai != am.end(); ++ai, ++bi) {         // both std::map: key-ordered
            if (ai->first != bi->first) return false;
            if (!value_equal(ai->second, bi->second)) return false;
        }
        return true;
    }
    }
    return false;
}

} // namespace rawast
