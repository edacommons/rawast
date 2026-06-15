#include <rawast/parsers_gdsii.hpp>
#include <rawast/grammar.hpp>
#include <rawast/parsers_registry.hpp>
#include <rawast/stream.hpp>
#include <rawast/value.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

namespace rawast {

namespace {

// GDSII data-type codes (low byte of rec_id).
constexpr std::uint8_t DT_NO_DATA   = 0x00;
constexpr std::uint8_t DT_BIT_ARRAY = 0x01;
constexpr std::uint8_t DT_INT16     = 0x02;
constexpr std::uint8_t DT_INT32     = 0x03;
constexpr std::uint8_t DT_REAL32    = 0x04;
constexpr std::uint8_t DT_REAL64    = 0x05;
constexpr std::uint8_t DT_STR       = 0x06;

// rec_id encoder: (rec_type << 8) | data_type, matching the on-wire
// big-endian encoding of bytes 3-4 of the record header.
constexpr std::uint16_t REC(std::uint8_t type, std::uint8_t dt) {
    return (static_cast<std::uint16_t>(type) << 8) | dt;
}

// --- Stream byte helpers --------------------------------------------------

bool read_exact(StreamReader& sr, std::size_t n, std::string& out) {
    out.clear();
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        auto c = sr.get();
        if (!c) return false;
        out.push_back(*c);
    }
    return true;
}

// Big-endian unpackers — operate on raw byte pointers.

std::uint16_t be_u16(const char* p) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[0])) << 8) |
         static_cast<std::uint16_t>(static_cast<std::uint8_t>(p[1])));
}

std::int16_t be_i16(const char* p) {
    return static_cast<std::int16_t>(be_u16(p));
}

std::int32_t be_i32(const char* p) {
    std::uint32_t u =
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[0])) << 24) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[1])) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[2])) << 8)  |
         static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[3]));
    return static_cast<std::int32_t>(u);
}

// GDSII 64-bit real format (NOT IEEE-754).
//
// 8 bytes: byte 0's high bit is sign; byte 0's low 7 bits are the
// exponent E (biased by 64, base-16). Bytes 1-7 are the 56-bit
// unsigned mantissa M (big-endian). Value = M * 2^(4*E - 312).
double decode_real64(const char* p) {
    const std::uint8_t b0 = static_cast<std::uint8_t>(p[0]);
    const int E = static_cast<int>(b0 & 0x7F);
    std::uint64_t m = 0;
    for (int i = 1; i < 8; ++i) {
        m = (m << 8) | static_cast<std::uint8_t>(p[i]);
    }
    double v = std::ldexp(static_cast<double>(m), 4 * E - 312);
    if (b0 & 0x80) v = -v;
    return v;
}

// GDSII 32-bit real: same shape, 3-byte mantissa, 2^(4*E - 280).
double decode_real32(const char* p) {
    const std::uint8_t b0 = static_cast<std::uint8_t>(p[0]);
    const int E = static_cast<int>(b0 & 0x7F);
    std::uint32_t m =
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[1])) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[2])) << 8)  |
         static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[3]));
    double v = std::ldexp(static_cast<double>(m), 4 * E - 280);
    if (b0 & 0x80) v = -v;
    return v;
}

// Encode double into GDSII 64-bit real format (8 bytes). Inverse of
// decode_real64. Special case for zero (all-zero bytes).
std::string encode_real64(double v) {
    std::string out(8, '\0');
    if (v == 0.0) return out;
    std::uint8_t b0 = 0;
    if (v < 0) { b0 = 0x80; v = -v; }
    int exp_bin;
    double m_frac = std::frexp(v, &exp_bin);   // v = m_frac * 2^exp_bin
    // GDSII: M_int * 2^(4*E - 312). Set M_int = m_frac * 2^56, so:
    // m_frac * 2^exp_bin = (m_frac * 2^56) * 2^(exp_bin - 56)
    // Choose E so 4*E - 312 = exp_bin - 56, modulo shifts to align E.
    std::uint64_t m_int = static_cast<std::uint64_t>(std::ldexp(m_frac, 56));
    int adj_exp = exp_bin - 56;
    while (adj_exp & 3) { ++adj_exp; m_int >>= 1; }
    int E = (adj_exp + 312) / 4;
    if (E < 0)       { E = 0; m_int = 0; }
    if (E > 0x7F)    { E = 0x7F; m_int = 0xFFFFFFFFFFFFFFULL; }
    b0 |= static_cast<std::uint8_t>(E & 0x7F);
    out[0] = static_cast<char>(b0);
    for (int i = 7; i >= 1; --i) {
        out[i] = static_cast<char>(m_int & 0xFF);
        m_int >>= 8;
    }
    return out;
}

// --- Decode payload into a Value ----------------------------------------

ValuePtr decode_payload(std::uint8_t data_type, const std::string& data) {
    switch (data_type) {
    case DT_NO_DATA:
        return null_value();

    case DT_BIT_ARRAY: {
        if (data.size() < 2) return null_value();
        return make_int(static_cast<std::int64_t>(be_u16(data.data())));
    }

    case DT_INT16: {
        const std::size_t n = data.size() / 2;
        if (n == 1) {
            return make_int(static_cast<std::int64_t>(be_i16(data.data())));
        }
        auto arr = std::make_shared<ArrayValue>();
        arr->data().reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            arr->data().push_back(
                make_int(static_cast<std::int64_t>(be_i16(data.data() + i * 2))));
        }
        return arr;
    }

    case DT_INT32: {
        const std::size_t n = data.size() / 4;
        if (n == 1) {
            return make_int(static_cast<std::int64_t>(be_i32(data.data())));
        }
        auto arr = std::make_shared<ArrayValue>();
        arr->data().reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            arr->data().push_back(
                make_int(static_cast<std::int64_t>(be_i32(data.data() + i * 4))));
        }
        return arr;
    }

    case DT_REAL32: {
        const std::size_t n = data.size() / 4;
        if (n == 1) return make_real(decode_real32(data.data()));
        auto arr = std::make_shared<ArrayValue>();
        arr->data().reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            arr->data().push_back(make_real(decode_real32(data.data() + i * 4)));
        }
        return arr;
    }

    case DT_REAL64: {
        const std::size_t n = data.size() / 8;
        if (n == 1) return make_real(decode_real64(data.data()));
        auto arr = std::make_shared<ArrayValue>();
        arr->data().reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            arr->data().push_back(make_real(decode_real64(data.data() + i * 8)));
        }
        return arr;
    }

    case DT_STR: {
        std::string s = data;
        auto pos = s.find('\0');
        if (pos != std::string::npos) s.resize(pos);
        return make_string(std::move(s));
    }

    default:
        return null_value();
    }
}

// --- Encode Value back to payload bytes ----------------------------------

// Helpers: append big-endian fixed-width integer to `out`.
void be_append_u16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}
void be_append_i16(std::string& out, std::int16_t v) {
    be_append_u16(out, static_cast<std::uint16_t>(v));
}
void be_append_i32(std::string& out, std::int32_t v) {
    std::uint32_t u = static_cast<std::uint32_t>(v);
    out.push_back(static_cast<char>((u >> 24) & 0xFF));
    out.push_back(static_cast<char>((u >> 16) & 0xFF));
    out.push_back(static_cast<char>((u >>  8) & 0xFF));
    out.push_back(static_cast<char>( u        & 0xFF));
}

// Extract a list of int64 from a Value that's either an IntValue
// (single) or ArrayValue<Int> (many). Returns empty on shape mismatch.
std::vector<std::int64_t> as_int_list(const Value& v) {
    std::vector<std::int64_t> out;
    if (auto iv = dynamic_cast<const IntValue*>(&v)) {
        out.push_back(iv->data());
    } else if (auto av = dynamic_cast<const ArrayValue*>(&v)) {
        for (const auto& e : av->data()) {
            if (auto eiv = std::dynamic_pointer_cast<IntValue>(e)) {
                out.push_back(eiv->data());
            }
        }
    }
    return out;
}

std::vector<double> as_real_list(const Value& v) {
    std::vector<double> out;
    if (auto rv = dynamic_cast<const RealValue*>(&v)) {
        out.push_back(rv->data());
    } else if (auto av = dynamic_cast<const ArrayValue*>(&v)) {
        for (const auto& e : av->data()) {
            if (auto erv = std::dynamic_pointer_cast<RealValue>(e)) {
                out.push_back(erv->data());
            }
        }
    }
    return out;
}

// Encode the payload bytes for a given data_type and Value.
tl::expected<std::string, SaveError>
encode_payload(std::uint8_t data_type, const Value& v) {
    switch (data_type) {
    case DT_NO_DATA:
        return std::string{};

    case DT_BIT_ARRAY: {
        auto iv = dynamic_cast<const IntValue*>(&v);
        if (!iv) return tl::unexpected(SaveError{
            "GDSII BIT_ARRAY expects IntValue"});
        std::string out;
        be_append_u16(out, static_cast<std::uint16_t>(iv->data()));
        return out;
    }

    case DT_INT16: {
        auto xs = as_int_list(v);
        std::string out;
        for (auto x : xs) be_append_i16(out, static_cast<std::int16_t>(x));
        return out;
    }

    case DT_INT32: {
        auto xs = as_int_list(v);
        std::string out;
        for (auto x : xs) be_append_i32(out, static_cast<std::int32_t>(x));
        return out;
    }

    case DT_REAL64: {
        auto xs = as_real_list(v);
        std::string out;
        for (auto x : xs) out += encode_real64(x);
        return out;
    }

    case DT_STR: {
        auto sv = dynamic_cast<const StringValue*>(&v);
        if (!sv) return tl::unexpected(SaveError{
            "GDSII STR expects StringValue"});
        std::string s = sv->data();
        if (s.size() & 1) s.push_back('\0');   // pad to even
        return s;
    }

    // REAL32 emit is rare in practice; skipped in v1.
    case DT_REAL32:
    default:
        return tl::unexpected(SaveError{
            "GDSII encode: unsupported data_type"});
    }
}

} // namespace

// -------------------------------------------------------------------------
// GdsiiRecordParser
// -------------------------------------------------------------------------

GdsiiRecordParser::GdsiiRecordParser(std::string name, std::uint16_t rec_id)
    : Parser(std::move(name)), expected_rec_id_(rec_id) {}

WalkResult GdsiiRecordParser::walk(StreamReader& sr) {
    const Position start = sr.position();
    sr.mark();

    std::string hdr;
    if (!read_exact(sr, 4, hdr)) {
        sr.reject();
        return tl::unexpected(ParseError{start, "GDSII: EOF in record header"});
    }
    const std::uint16_t size   = be_u16(hdr.data());
    const std::uint16_t rec_id = be_u16(hdr.data() + 2);
    if (rec_id != expected_rec_id_) {
        sr.reject();
        return tl::unexpected(ParseError{start, "GDSII: record type mismatch"});
    }
    if (size < 4) {
        sr.reject();
        return tl::unexpected(ParseError{start, "GDSII: invalid record size"});
    }
    std::string data;
    if (size > 4 && !read_exact(sr, size - 4, data)) {
        sr.reject();
        return tl::unexpected(ParseError{start, "GDSII: EOF in record payload"});
    }
    sr.accept();
    decoded_ = decode_payload(
        static_cast<std::uint8_t>(expected_rec_id_ & 0xFF), data);
    return {};
}

ValuePtr GdsiiRecordParser::value() const { return decoded_; }

void GdsiiRecordParser::reset() {
    accum_.clear();
    decoded_.reset();
}

SaveResult GdsiiRecordParser::unparse(const Value& value) const {
    auto payload = encode_payload(
        static_cast<std::uint8_t>(expected_rec_id_ & 0xFF), value);
    if (!payload) return tl::unexpected(payload.error());
    const std::size_t total = 4 + payload->size();
    if (total > 0xFFFF) {
        return tl::unexpected(SaveError{"GDSII: record too large"});
    }
    std::string out;
    out.reserve(total);
    be_append_u16(out, static_cast<std::uint16_t>(total));
    be_append_u16(out, expected_rec_id_);
    out += *payload;
    return out;
}

// -------------------------------------------------------------------------
// gdsii parser group — all 47 record types
// -------------------------------------------------------------------------

namespace {

// GDSII files are written in 2048-byte blocks (legacy mag-tape
// convention); the final block is padded with NUL bytes after END_LIB.
// This terminal consumes any number of trailing NULs (including zero)
// so the LIBRARY rule's trailing padding is absorbed and the engine's
// end-of-input check passes. Used only at the end of LIBRARY — not
// added to the grammar's ignore list, since record payloads (DT_STR
// fields in particular) can legitimately contain NUL bytes mid-file.
class GdsiiPaddingParser : public Parser {
    std::int64_t count_ = 0;
public:
    GdsiiPaddingParser() : Parser("padding") {}
    WalkResult walk(StreamReader& sr) override {
        // No mark/accept envelope: this parser cannot fail, so we
        // never need to rewind. Skipping the mark means consumed
        // bytes are NOT buffered by StreamReader, keeping memory at
        // O(1) regardless of padding size (a malicious file with
        // gigabytes of trailing NULs would otherwise force buffering
        // of every byte until accept()).
        while (auto b = sr.peek()) {
            if (*b != '\0') break;
            sr.get();
            ++count_;
        }
        return {};
    }
    ValuePtr value() const override {
        return std::make_shared<IntValue>(count_);
    }
    void reset() override {
        accum_.clear();
        count_ = 0;
    }
    SaveResult unparse(const Value& v) const override {
        // Round-trip: emit the same number of NUL bytes we consumed
        // on parse. A value constructed from scratch (no
        // padding_bytes field set) saves with zero padding — valid
        // GDSII, just not 2,048-byte-aligned.
        if (auto* iv = dynamic_cast<const IntValue*>(&v)) {
            if (iv->data() <= 0) return std::string();
            return std::string(static_cast<std::size_t>(iv->data()), '\0');
        }
        return std::string();
    }
};

// Each spec yields a fresh GdsiiRecordParser instance bound to one
// record-type / data-type pair. The factory captures the type+dt by
// value so it remains valid past the spec list's construction.
ParserSpec gds_spec(std::string name, std::uint8_t type, std::uint8_t dt) {
    auto stored_name = name;
    return ParserSpec{
        std::move(name),
        [stored_name, type, dt]() {
            return std::make_unique<GdsiiRecordParser>(
                stored_name, REC(type, dt));
        }
    };
}

ParserGroup make_gdsii_group() {
    ParserGroup g;
    g.name = "gdsii";
    g.parsers = {
        gds_spec("header",       0x00, DT_INT16),
        gds_spec("bgnlib",       0x01, DT_INT16),
        gds_spec("libname",      0x02, DT_STR),
        gds_spec("units",        0x03, DT_REAL64),
        gds_spec("endlib",       0x04, DT_NO_DATA),
        gds_spec("bgnstr",       0x05, DT_INT16),
        gds_spec("strname",      0x06, DT_STR),
        gds_spec("endstr",       0x07, DT_NO_DATA),
        gds_spec("boundary",     0x08, DT_NO_DATA),
        gds_spec("path",         0x09, DT_NO_DATA),
        gds_spec("sref",         0x0A, DT_NO_DATA),
        gds_spec("aref",         0x0B, DT_NO_DATA),
        gds_spec("text",         0x0C, DT_NO_DATA),
        gds_spec("layer",        0x0D, DT_INT16),
        gds_spec("datatype",     0x0E, DT_INT16),
        gds_spec("width",        0x0F, DT_INT32),
        gds_spec("xy",           0x10, DT_INT32),
        gds_spec("endel",        0x11, DT_NO_DATA),
        gds_spec("sname",        0x12, DT_STR),
        gds_spec("colrow",       0x13, DT_INT16),
        gds_spec("textnode",     0x14, DT_NO_DATA),
        gds_spec("node",         0x15, DT_NO_DATA),
        gds_spec("texttype",     0x16, DT_INT16),
        gds_spec("presentation", 0x17, DT_BIT_ARRAY),
        gds_spec("string",       0x19, DT_STR),
        gds_spec("strans",       0x1A, DT_BIT_ARRAY),
        gds_spec("mag",          0x1B, DT_REAL64),
        gds_spec("angle",        0x1C, DT_REAL64),
        gds_spec("reflibs",      0x1F, DT_STR),
        gds_spec("fonts",        0x20, DT_STR),
        gds_spec("pathtype",     0x21, DT_INT16),
        gds_spec("generations",  0x22, DT_INT16),
        gds_spec("attrtable",    0x23, DT_STR),
        gds_spec("elflags",      0x26, DT_BIT_ARRAY),
        gds_spec("nodetype",     0x2A, DT_INT16),
        gds_spec("propattr",     0x2B, DT_INT16),
        gds_spec("propvalue",    0x2C, DT_STR),
        gds_spec("box",          0x2D, DT_NO_DATA),
        gds_spec("boxtype",      0x2E, DT_INT16),
        gds_spec("plex",         0x2F, DT_INT32),
        gds_spec("bgnextn",      0x30, DT_INT32),
        gds_spec("endextn",      0x31, DT_INT32),
        gds_spec("tapenum",      0x32, DT_INT16),
        gds_spec("tapecode",     0x33, DT_INT16),
        gds_spec("strclass",     0x34, DT_BIT_ARRAY),
        gds_spec("format",       0x36, DT_INT16),
        gds_spec("mask",         0x37, DT_STR),
        gds_spec("endmasks",     0x38, DT_NO_DATA),
        ParserSpec{"padding", []() {
            return std::make_unique<GdsiiPaddingParser>();
        }},
    };
    return g;
}

} // namespace

void register_gdsii_parser_group() {
    register_parser_group(make_gdsii_group());
}

// Back-compat shim: the legacy function-style entry point still works
// for any direct C++ caller. Internally it applies the structured
// group, so both code paths converge on the same parser instances.
void register_gdsii_parsers(Grammar& g) {
    register_gdsii_parser_group();   // idempotent
    auto r = apply_parser_group(g, "gdsii");
    (void)r;   // structured group always exists once registered
}

} // namespace rawast
