#include <doctest/doctest.h>
#include <rawast/stream.hpp>

#include <sstream>

using namespace rawast;

TEST_CASE("StreamReader returns bytes in order then EOF") {
    std::istringstream is{"abc"};
    StreamReader sr{is};

    CHECK(sr.get() == 'a');
    CHECK(sr.get() == 'b');
    CHECK(sr.get() == 'c');
    CHECK_FALSE(sr.get().has_value());
}

TEST_CASE("StreamReader peek does not consume") {
    std::istringstream is{"abc"};
    StreamReader sr{is};

    CHECK(sr.peek() == 'a');
    CHECK(sr.peek() == 'a');
    CHECK(sr.get()  == 'a');
    CHECK(sr.peek() == 'b');
}

TEST_CASE("StreamReader eof reports correctly") {
    std::istringstream is{"x"};
    StreamReader sr{is};

    CHECK_FALSE(sr.eof());
    sr.get();
    CHECK(sr.eof());
}

TEST_CASE("StreamReader mark + accept advances past the marked span") {
    std::istringstream is{"abc"};
    StreamReader sr{is};

    sr.mark();
    CHECK(sr.get() == 'a');
    CHECK(sr.get() == 'b');
    sr.accept();
    CHECK(sr.get() == 'c');
}

TEST_CASE("StreamReader mark + reject rewinds the cursor") {
    std::istringstream is{"abc"};
    StreamReader sr{is};

    sr.mark();
    CHECK(sr.get() == 'a');
    CHECK(sr.get() == 'b');
    sr.reject();
    CHECK(sr.get() == 'a');
}

TEST_CASE("StreamReader nested marks rewind locally") {
    std::istringstream is{"abcdef"};
    StreamReader sr{is};

    sr.mark();
    sr.get(); // 'a'
    sr.mark();
    sr.get(); // 'b'
    sr.get(); // 'c'
    sr.reject();           // back to position after 'a'
    CHECK(sr.get() == 'b');
    sr.accept();
    CHECK(sr.get() == 'c');
}

TEST_CASE("StreamReader tracks line and column") {
    std::istringstream is{"ab\ncd\nef"};
    StreamReader sr{is};

    CHECK(sr.position().line   == 1);
    CHECK(sr.position().column == 1);

    sr.get();   // 'a'
    CHECK(sr.position().line   == 1);
    CHECK(sr.position().column == 2);

    sr.get();   // 'b'
    sr.get();   // '\n'
    CHECK(sr.position().line   == 2);
    CHECK(sr.position().column == 1);

    sr.get();   // 'c'
    CHECK(sr.position().line   == 2);
    CHECK(sr.position().column == 2);
}

TEST_CASE("StreamReader reject restores line and column") {
    std::istringstream is{"a\nb"};
    StreamReader sr{is};

    sr.mark();
    Position p0 = sr.position();
    sr.get(); // 'a'
    sr.get(); // '\n'
    sr.get(); // 'b'
    sr.reject();

    auto p1 = sr.position();
    CHECK(p1.bytes  == p0.bytes);
    CHECK(p1.line   == p0.line);
    CHECK(p1.column == p0.column);
}

TEST_CASE("StreamReader mark_count tracks active marks") {
    std::istringstream is{"x"};
    StreamReader sr{is};

    CHECK(sr.mark_count() == 0);
    sr.mark();
    CHECK(sr.mark_count() == 1);
    sr.mark();
    CHECK(sr.mark_count() == 2);
    sr.accept();
    CHECK(sr.mark_count() == 1);
    sr.reject();
    CHECK(sr.mark_count() == 0);
}

TEST_CASE("StreamReader absolute byte position advances monotonically") {
    std::istringstream is{"abcdef"};
    StreamReader sr{is};

    CHECK(sr.position().bytes == 0);
    sr.get();
    CHECK(sr.position().bytes == 1);
    sr.get();
    sr.get();
    CHECK(sr.position().bytes == 3);
}

TEST_CASE("StreamReader byte position survives accept/trim") {
    std::istringstream is{"abcdef"};
    StreamReader sr{is};

    sr.mark();
    sr.get(); // a
    sr.get(); // b
    sr.accept();           // buffer trims here
    CHECK(sr.position().bytes == 2);

    sr.mark();
    sr.get(); // c
    sr.accept();
    CHECK(sr.position().bytes == 3);
}
