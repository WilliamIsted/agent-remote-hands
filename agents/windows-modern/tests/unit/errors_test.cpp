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

#include "errors.hpp"

using namespace remote_hands;

TEST_CASE("Verb-agnostic error codes serialise per spec") {
    CHECK(to_wire(ErrorCode::TierRequired)      == "tier_required");
    CHECK(to_wire(ErrorCode::NotSupported)      == "not_supported");
    CHECK(to_wire(ErrorCode::InvalidArgs)       == "invalid_args");
    CHECK(to_wire(ErrorCode::InvalidState)      == "invalid_state");
    CHECK(to_wire(ErrorCode::WireDesync)        == "wire_desync");
    CHECK(to_wire(ErrorCode::Timeout)           == "timeout");
    CHECK(to_wire(ErrorCode::Busy)              == "busy");
    CHECK(to_wire(ErrorCode::ProtocolMismatch)  == "protocol_mismatch");
    CHECK(to_wire(ErrorCode::AuthRequired)      == "auth_required");
    CHECK(to_wire(ErrorCode::AuthInvalid)       == "auth_invalid");
}

TEST_CASE("Domain-specific error codes serialise per spec") {
    CHECK(to_wire(ErrorCode::TargetGone)            == "target_gone");
    CHECK(to_wire(ErrorCode::UipiBlocked)           == "uipi_blocked");
    CHECK(to_wire(ErrorCode::NotFound)              == "not_found");
    CHECK(to_wire(ErrorCode::UiaBlind)              == "uia_blind");
    CHECK(to_wire(ErrorCode::LockHeld)              == "lock_held");
    CHECK(to_wire(ErrorCode::Readonly)              == "readonly");
    CHECK(to_wire(ErrorCode::NotSupportedByTarget)  == "not_supported_by_target");
    CHECK(to_wire(ErrorCode::InsufficientPrivilege) == "insufficient_privilege");
}

TEST_CASE("to_wire is constexpr-callable") {
    constexpr auto s = to_wire(ErrorCode::TierRequired);
    static_assert(s == "tier_required");
    CHECK(s == "tier_required");
}
