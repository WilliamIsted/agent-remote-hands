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

#include "element_table.hpp"

#include <charconv>
#include <cstdio>
#include <system_error>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <UIAutomation.h>

namespace remote_hands {

namespace {

bool parse_elt_id(std::string_view id, unsigned& out) {
    if (id.size() < 5 || id.substr(0, 4) != "elt:") return false;
    const auto rest = id.substr(4);
    const auto* end = rest.data() + rest.size();
    const auto [p, ec] = std::from_chars(rest.data(), end, out, 10);
    return ec == std::errc{} && p == end;
}

}  // namespace

ElementTable::ElementTable() = default;

ElementTable::~ElementTable() {
    for (auto& [_, elem] : elements_) {
        if (elem) elem->Release();
    }
    if (uia_) uia_->Release();
}

std::string ElementTable::register_element(IUIAutomationElement* elem) {
    if (!elem) return {};

    std::lock_guard lock{mu_};
    elem->AddRef();
    const unsigned id = next_id_++;
    elements_[id] = elem;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "elt:%u", id);
    return buf;
}

IUIAutomationElement* ElementTable::lookup(std::string_view id) const {
    unsigned n = 0;
    if (!parse_elt_id(id, n)) return nullptr;

    std::lock_guard lock{mu_};
    auto it = elements_.find(n);
    return it == elements_.end() ? nullptr : it->second;
}

void ElementTable::unregister(std::string_view id) {
    unsigned n = 0;
    if (!parse_elt_id(id, n)) return;

    std::lock_guard lock{mu_};
    auto it = elements_.find(n);
    if (it != elements_.end()) {
        if (it->second) it->second->Release();
        elements_.erase(it);
    }
}

IUIAutomation* ElementTable::uia() {
    std::lock_guard lock{mu_};
    if (uia_) return uia_;

    const HRESULT hr = CoCreateInstance(
        __uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
        __uuidof(IUIAutomation), reinterpret_cast<void**>(&uia_));
    if (FAILED(hr)) {
        uia_ = nullptr;
        return nullptr;
    }
    return uia_;
}

}  // namespace remote_hands
