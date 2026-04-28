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

#include "json.hpp"

#include <string>
#include <string_view>

using namespace remote_hands;

TEST_CASE("json::append_string round-trips simple ASCII") {
    std::string s;
    json::append_string(s, "hello");
    CHECK(s == "\"hello\"");
}

TEST_CASE("json::append_string handles empty input") {
    std::string s;
    json::append_string(s, std::string_view{""});
    CHECK(s == "\"\"");
}

TEST_CASE("json::append_string escapes quotes and backslashes") {
    std::string s;
    json::append_string(s, "with \"quotes\" and \\");
    CHECK(s == "\"with \\\"quotes\\\" and \\\\\"");
}

TEST_CASE("json::append_string escapes whitespace controls") {
    std::string s;
    json::append_string(s, "a\nb\rc\td");
    CHECK(s == "\"a\\nb\\rc\\td\"");
}

TEST_CASE("json::append_string escapes other control chars as \\uXXXX") {
    std::string s;
    const char ctrl = 0x07;  // BEL
    json::append_string(s, std::string_view{&ctrl, 1});
    CHECK(s == "\"\\u0007\"");
}

TEST_CASE("json::append_kv_string emits key:value") {
    std::string s;
    json::append_kv_string(s, "name", "agent");
    CHECK(s == "\"name\":\"agent\"");
}

TEST_CASE("json::append_kv_int handles signed and zero") {
    std::string s;
    json::append_kv_int(s, "n", 42);
    CHECK(s == "\"n\":42");

    s.clear();
    json::append_kv_int(s, "z", 0);
    CHECK(s == "\"z\":0");

    s.clear();
    json::append_kv_int(s, "neg", -7);
    CHECK(s == "\"neg\":-7");
}

TEST_CASE("json::append_kv_uint handles large values") {
    std::string s;
    json::append_kv_uint(s, "u", 4294967295ULL);
    CHECK(s == "\"u\":4294967295");
}

TEST_CASE("json::append_kv_bool emits literal true/false") {
    std::string s;
    json::append_kv_bool(s, "t", true);
    CHECK(s == "\"t\":true");

    s.clear();
    json::append_kv_bool(s, "f", false);
    CHECK(s == "\"f\":false");
}

TEST_CASE("json::append_kv_null emits literal null") {
    std::string s;
    json::append_kv_null(s, "x");
    CHECK(s == "\"x\":null");
}

TEST_CASE("json::append_string_array handles empty and populated") {
    std::string s;
    json::append_string_array(s, "empty", {});
    CHECK(s == "\"empty\":[]");

    s.clear();
    json::append_string_array(s, "items", {"a", "b", "c"});
    CHECK(s == "\"items\":[\"a\",\"b\",\"c\"]");
}

TEST_CASE("json::append_string_array escapes element strings") {
    std::string s;
    json::append_string_array(s, "k", {"with \"quote\""});
    CHECK(s == "\"k\":[\"with \\\"quote\\\"\"]");
}
