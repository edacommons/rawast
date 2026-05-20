#include <rawast/stream.hpp>

#include <cassert>

namespace rawast {

void StreamReader::advance_position(char c) noexcept {
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
}

std::optional<char> StreamReader::get() {
    char c;
    if (pos_ < buf_.size()) {
        c = buf_[pos_++];
    } else {
        int ch = is_.get();
        if (ch == std::istream::traits_type::eof()) {
            return std::nullopt;
        }
        c = static_cast<char>(ch);
        buf_.push_back(c);
        pos_ = buf_.size();
    }
    advance_position(c);
    if (marks_.empty()) {
        consumed_ += pos_;
        buf_.erase(0, pos_);
        pos_ = 0;
    }
    return c;
}

std::optional<char> StreamReader::peek() {
    if (pos_ < buf_.size()) {
        return buf_[pos_];
    }
    int ch = is_.peek();
    if (ch == std::istream::traits_type::eof()) {
        return std::nullopt;
    }
    return static_cast<char>(ch);
}

bool StreamReader::eof() {
    return !peek().has_value();
}

void StreamReader::mark() {
    marks_.push_back({pos_, line_, column_});
}

void StreamReader::accept() {
    assert(!marks_.empty());
    marks_.pop_back();
    if (marks_.empty() && pos_ > 0) {
        consumed_ += pos_;
        buf_.erase(0, pos_);
        pos_ = 0;
    }
}

void StreamReader::reject() {
    assert(!marks_.empty());
    auto m = marks_.back();
    marks_.pop_back();
    pos_     = m.pos;
    line_    = m.line;
    column_  = m.column;
}

Position StreamReader::position() const noexcept {
    return {consumed_ + pos_, line_, column_};
}

} // namespace rawast
