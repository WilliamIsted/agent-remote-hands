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

#pragma once

// Error codes for ERR responses. See PROTOCOL.md §5 for the canonical set.
//
// The wire format is `ERR <code> [<length>\n<json-detail>]`. Codes here are
// translated to wire form via to_wire(). Detail JSON is constructed by the
// verb that fires the error (errors.cpp will add helpers in build phase 4).

#include <string_view>

namespace remote_hands {

enum class ErrorCode {
    // Verb-agnostic
    TierRequired,
    NotSupported,
    InvalidArgs,
    InvalidState,
    WireDesync,
    Timeout,
    Busy,
    Conflict,
    ProtocolMismatch,
    AuthRequired,
    AuthInvalid,

    // Domain-specific
    TargetGone,
    UipiBlocked,
    NotFound,
    PermissionDenied,
    UiaBlind,
    LockHeld,
    Readonly,
    NotSupportedByTarget,
    InsufficientPrivilege,
    FramingUnsupported,
    UserCancelled,
    NoHandler,
    Empty,
    AlreadyExists,
    NotEmpty,
    NotADirectory,
    CrossDevice,
    UnsupportedFormat,
    ImageTooLarge,
    SizeLimitExceeded,
    NoImplementationAvailable,
    TransferFailed,
};

constexpr std::string_view to_wire(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::TierRequired:           return "tier_required";
        case ErrorCode::NotSupported:           return "not_supported";
        case ErrorCode::InvalidArgs:            return "invalid_args";
        case ErrorCode::InvalidState:           return "invalid_state";
        case ErrorCode::WireDesync:             return "wire_desync";
        case ErrorCode::Timeout:                return "timeout";
        case ErrorCode::Busy:                   return "busy";
        case ErrorCode::Conflict:               return "conflict";
        case ErrorCode::ProtocolMismatch:       return "protocol_mismatch";
        case ErrorCode::AuthRequired:           return "auth_required";
        case ErrorCode::AuthInvalid:            return "auth_invalid";
        case ErrorCode::TargetGone:             return "target_gone";
        case ErrorCode::UipiBlocked:            return "uipi_blocked";
        case ErrorCode::NotFound:               return "not_found";
        case ErrorCode::PermissionDenied:       return "permission_denied";
        case ErrorCode::UiaBlind:               return "uia_blind";
        case ErrorCode::LockHeld:               return "lock_held";
        case ErrorCode::Readonly:               return "readonly";
        case ErrorCode::NotSupportedByTarget:   return "not_supported_by_target";
        case ErrorCode::InsufficientPrivilege:  return "insufficient_privilege";
        case ErrorCode::FramingUnsupported:     return "framing_unsupported";
        case ErrorCode::UserCancelled:          return "user_cancelled";
        case ErrorCode::NoHandler:              return "no_handler";
        case ErrorCode::Empty:                  return "empty";
        case ErrorCode::AlreadyExists:          return "already_exists";
        case ErrorCode::NotEmpty:               return "not_empty";
        case ErrorCode::NotADirectory:          return "not_a_directory";
        case ErrorCode::CrossDevice:            return "cross_device";
        case ErrorCode::UnsupportedFormat:      return "unsupported_format";
        case ErrorCode::ImageTooLarge:          return "image_too_large";
        case ErrorCode::SizeLimitExceeded:      return "size_limit_exceeded";
        case ErrorCode::NoImplementationAvailable:
                                                return "no_implementation_available";
        case ErrorCode::TransferFailed:         return "transfer_failed";
    }
    return "unknown";
}

}  // namespace remote_hands
