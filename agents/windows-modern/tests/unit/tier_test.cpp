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

#include <ostream>           // see test_main.cpp — needed before doctest under MSVC /permissive-
#include <doctest/doctest.h>

#include "tier.hpp"

using namespace remote_hands;

TEST_CASE("Tier serialises to wire string") {
    CHECK(to_wire(Tier::Read)       == "read");
    CHECK(to_wire(Tier::Create)     == "create");
    CHECK(to_wire(Tier::Update)     == "update");
    CHECK(to_wire(Tier::Delete)     == "delete");
    CHECK(to_wire(Tier::ExtraRisky) == "extra_risky");
}

TEST_CASE("tier_from_wire round-trips known names") {
    CHECK(tier_from_wire("read")        == Tier::Read);
    CHECK(tier_from_wire("create")      == Tier::Create);
    CHECK(tier_from_wire("update")      == Tier::Update);
    CHECK(tier_from_wire("delete")      == Tier::Delete);
    CHECK(tier_from_wire("extra_risky") == Tier::ExtraRisky);
}

TEST_CASE("tier_from_wire rejects unknown / empty names") {
    CHECK_FALSE(tier_from_wire("").has_value());
    CHECK_FALSE(tier_from_wire("Read").has_value());           // case-sensitive
    CHECK_FALSE(tier_from_wire("super_user").has_value());
    // v2.0 vocabulary explicitly rejected (no alias period — see PROTOCOL.md §12.5).
    CHECK_FALSE(tier_from_wire("observe").has_value());
    CHECK_FALSE(tier_from_wire("drive").has_value());
    CHECK_FALSE(tier_from_wire("power").has_value());
}

TEST_CASE("tier_satisfies enforces ladder order: read < create < update < delete < extra_risky") {
    // A connection at read can run read-only verbs but nothing higher.
    CHECK(tier_satisfies(Tier::Read, Tier::Read));
    CHECK_FALSE(tier_satisfies(Tier::Create,     Tier::Read));
    CHECK_FALSE(tier_satisfies(Tier::Update,     Tier::Read));
    CHECK_FALSE(tier_satisfies(Tier::Delete,     Tier::Read));
    CHECK_FALSE(tier_satisfies(Tier::ExtraRisky, Tier::Read));
}

TEST_CASE("tier_satisfies allows higher tier to run lower-tier verbs") {
    // create subsumes read.
    CHECK(tier_satisfies(Tier::Read,   Tier::Create));
    CHECK(tier_satisfies(Tier::Create, Tier::Create));
    CHECK_FALSE(tier_satisfies(Tier::Update,     Tier::Create));
    CHECK_FALSE(tier_satisfies(Tier::Delete,     Tier::Create));
    CHECK_FALSE(tier_satisfies(Tier::ExtraRisky, Tier::Create));

    // update subsumes create + read.
    CHECK(tier_satisfies(Tier::Read,   Tier::Update));
    CHECK(tier_satisfies(Tier::Create, Tier::Update));
    CHECK(tier_satisfies(Tier::Update, Tier::Update));
    CHECK_FALSE(tier_satisfies(Tier::Delete,     Tier::Update));
    CHECK_FALSE(tier_satisfies(Tier::ExtraRisky, Tier::Update));

    // delete subsumes update + create + read.
    CHECK(tier_satisfies(Tier::Read,   Tier::Delete));
    CHECK(tier_satisfies(Tier::Create, Tier::Delete));
    CHECK(tier_satisfies(Tier::Update, Tier::Delete));
    CHECK(tier_satisfies(Tier::Delete, Tier::Delete));
    CHECK_FALSE(tier_satisfies(Tier::ExtraRisky, Tier::Delete));

    // extra_risky is the top of the ladder.
    CHECK(tier_satisfies(Tier::Read,       Tier::ExtraRisky));
    CHECK(tier_satisfies(Tier::Create,     Tier::ExtraRisky));
    CHECK(tier_satisfies(Tier::Update,     Tier::ExtraRisky));
    CHECK(tier_satisfies(Tier::Delete,     Tier::ExtraRisky));
    CHECK(tier_satisfies(Tier::ExtraRisky, Tier::ExtraRisky));
}
