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

// doctest entry point. The implementation symbol must live in exactly one
// translation unit; everything else just `#include <doctest/doctest.h>`.

// MSVC 14.44's <string_view> declares helpers that use basic_ostream<>; under
// /permissive- the bodies are parsed eagerly, so <ostream> must already be
// complete before any header that pulls in <string_view>. doctest does.
#include <ostream>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
