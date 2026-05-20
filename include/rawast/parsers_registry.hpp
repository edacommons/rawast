#pragma once

#include <tl/expected.hpp>

#include <functional>
#include <string>
#include <vector>

namespace rawast {

class Grammar;

// A "parser group" is a named set of terminal parsers that a grammar
// declares it needs via a `use:` directive. The group is identified by
// a short name (e.g. "gdsii", "standard") and resolves to a function
// that registers the group's parsers on a target Grammar.
//
// Groups are typically registered at static-initialisation time via the
// RAWAST_REGISTER_PARSER_GROUP macro below; the loader looks them up at
// grammar-load time when it encounters a `use:` declaration.

using ParserGroupRegisterFn = std::function<void(Grammar&)>;

// Register a parser-group factory under `name`. Later loads of any
// grammar containing `use: name` will call `fn(grammar)` to register
// that group's parsers on the target.
void register_parser_group(std::string name, ParserGroupRegisterFn fn);

// Apply a previously-registered group to `g`. Returns an error if the
// group name was never registered.
tl::expected<void, std::string>
apply_parser_group(Grammar& g, const std::string& name);

// Diagnostic: is the named group available?
bool parser_group_exists(const std::string& name);

// Return the list of currently-registered group names, sorted.
std::vector<std::string> registered_parser_groups();

// Static-init-time registration pattern. Each parser-providing TU
// places an anonymous-namespace struct whose constructor calls
// register_parser_group:
//
//   namespace {
//       struct MyParsersAutoRegister {
//           MyParsersAutoRegister() {
//               register_parser_group("my_group", register_my_parsers);
//           }
//       };
//       MyParsersAutoRegister my_parsers_auto_register_;
//   }

} // namespace rawast
