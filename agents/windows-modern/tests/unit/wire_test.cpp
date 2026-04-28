//   Copyright 2026 William Isted and contributors
//
//   Licensed under the Apache License, Version 2.0 (the "License");
//   you may not use this file except in compliance with the License.
//   You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//   Unless required by applicable law or agreed to in writing, software
//   distributed under the License is distributed on an "AS IS" BASIS,
//   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//   See the License for the specific language governing permissions and
//   limitations under the License.

#include <doctest/doctest.h>

#include "protocol.hpp"

using namespace remote_hands::wire;

TEST_CASE("tokenize_header on a bare verb") {
    Request r = tokenize_header("system.info");
    CHECK(r.verb == "system.info");
    CHECK(r.args.empty());
}

TEST_CASE("tokenize_header pulls out positional args in order") {
    Request r = tokenize_header("input.click 100 200");
    CHECK(r.verb == "input.click");
    REQUIRE(r.args.size() == 2);
    CHECK(r.args[0] == "100");
    CHECK(r.args[1] == "200");
}

TEST_CASE("tokenize_header handles a length-style payload arg") {
    Request r = tokenize_header("input.type 13");
    CHECK(r.verb == "input.type");
    REQUIRE(r.args.size() == 1);
    CHECK(r.args[0] == "13");
}

TEST_CASE("tokenize_header collapses runs of spaces") {
    Request r = tokenize_header("a   b  c");
    CHECK(r.verb == "a");
    REQUIRE(r.args.size() == 2);
    CHECK(r.args[0] == "b");
    CHECK(r.args[1] == "c");
}

TEST_CASE("tokenize_header tolerates leading and trailing spaces") {
    Request r = tokenize_header("  window.list  ");
    CHECK(r.verb == "window.list");
    CHECK(r.args.empty());
}

TEST_CASE("tokenize_header on empty input gives an empty Request") {
    Request r = tokenize_header("");
    CHECK(r.verb.empty());
    CHECK(r.args.empty());
}

TEST_CASE("tokenize_header on whitespace-only input gives an empty Request") {
    Request r = tokenize_header("   ");
    CHECK(r.verb.empty());
    CHECK(r.args.empty());
}

TEST_CASE("tokenize_header preserves opaque token bytes (no quote handling)") {
    // Tokenisation does not interpret quotes; payload-bearing verbs use the
    // length-prefixed payload mechanism instead.
    Request r = tokenize_header("file.read /path/with-utf-8/\xc3\xa9");
    CHECK(r.verb == "file.read");
    REQUIRE(r.args.size() == 1);
    CHECK(r.args[0] == "/path/with-utf-8/\xc3\xa9");
}

TEST_CASE("tokenize_header preserves ordering with many args") {
    Request r = tokenize_header("registry.write HKCU\\Software\\Foo Bar REG_DWORD 1");
    CHECK(r.verb == "registry.write");
    REQUIRE(r.args.size() == 4);
    CHECK(r.args[0] == "HKCU\\Software\\Foo");
    CHECK(r.args[1] == "Bar");
    CHECK(r.args[2] == "REG_DWORD");
    CHECK(r.args[3] == "1");
}
