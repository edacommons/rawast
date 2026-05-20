#pragma once

#include <rawast/parser.hpp>

#include <cstdint>
#include <string>

namespace rawast {

class Grammar;

// Parser for one GDSII record type, identified by its 16-bit rec_id —
// the big-endian wire encoding of bytes 3-4 of the record header:
//
//   rec_id = (record_type << 8) | data_type
//
// Matches if-and-only-if the next record in the stream has the
// expected rec_id; otherwise rejects (rewinds the stream via
// StreamReader::mark / reject) so a Choice can dispatch on record
// type. Successful parse consumes the full record (header + payload)
// and returns the decoded payload as a typed Value:
//
//   NO_DATA   -> null_value()                  (the record is structural;
//                                              its presence is the signal)
//   BIT_ARRAY -> IntValue (16-bit unsigned)
//   INT16     -> IntValue, or ArrayValue<Int>  (one int → scalar; many → array)
//   INT32     -> IntValue, or ArrayValue<Int>
//   REAL32    -> RealValue, or ArrayValue<Real>
//   REAL64    -> RealValue, or ArrayValue<Real>
//   STR       -> StringValue (trailing nulls trimmed)
//
// XY records (record-type 0x10, data_type INT32) emit a flat
// ArrayValue<Int> of alternating x,y coordinates matching the on-wire
// layout. A separate XY-aware decoder emitting (x,y) pairs is a
// follow-on if desired.
class GdsiiRecordParser final : public Parser {
    std::uint16_t expected_rec_id_;
public:
    GdsiiRecordParser(std::string name, std::uint16_t rec_id);
    ParseResult parse(StreamReader& sr) override;
    SaveResult  unparse(const Value& value) const override;
};

// Register all GDSII record-type parsers on `g` under their conventional
// names: "gds_header", "gds_bgnlib", ..., "gds_endmasks". Use these
// names in a .rawast grammar as parser-name references. The complete
// list (~47 entries) mirrors the GDSII spec.
void register_gdsii_parsers(Grammar& g);

} // namespace rawast
