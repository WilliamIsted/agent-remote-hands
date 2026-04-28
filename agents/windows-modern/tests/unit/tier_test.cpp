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
    CHECK(to_wire(Tier::Observe) == "observe");
    CHECK(to_wire(Tier::Drive)   == "drive");
    CHECK(to_wire(Tier::Power)   == "power");
}

TEST_CASE("tier_from_wire round-trips known names") {
    CHECK(tier_from_wire("observe") == Tier::Observe);
    CHECK(tier_from_wire("drive")   == Tier::Drive);
    CHECK(tier_from_wire("power")   == Tier::Power);
}

TEST_CASE("tier_from_wire rejects unknown / empty names") {
    CHECK_FALSE(tier_from_wire("").has_value());
    CHECK_FALSE(tier_from_wire("Observe").has_value());      // case-sensitive
    CHECK_FALSE(tier_from_wire("super_user").has_value());
}

TEST_CASE("tier_satisfies enforces the order observe < drive < power") {
    // A connection at observe can run observe-only verbs.
    CHECK(tier_satisfies(Tier::Observe, Tier::Observe));
    // A connection at observe cannot run drive or power verbs.
    CHECK_FALSE(tier_satisfies(Tier::Drive,  Tier::Observe));
    CHECK_FALSE(tier_satisfies(Tier::Power,  Tier::Observe));
}

TEST_CASE("tier_satisfies allows higher tier to run lower-tier verbs") {
    CHECK(tier_satisfies(Tier::Observe, Tier::Drive));
    CHECK(tier_satisfies(Tier::Drive,   Tier::Drive));
    CHECK_FALSE(tier_satisfies(Tier::Power, Tier::Drive));

    CHECK(tier_satisfies(Tier::Observe, Tier::Power));
    CHECK(tier_satisfies(Tier::Drive,   Tier::Power));
    CHECK(tier_satisfies(Tier::Power,   Tier::Power));
}
