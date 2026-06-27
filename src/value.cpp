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

} // namespace rawast
