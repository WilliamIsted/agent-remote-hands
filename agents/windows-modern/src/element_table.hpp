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

// Per-connection UI Automation element registry.
//
// Element ids are connection-scoped sequential integers, formatted on the
// wire as `elt:<n>`. The table owns one COM ref per registered element and
// releases all refs (and the IUIAutomation singleton) on destruction.
//
// Connection holds one ElementTable. Verbs in `verbs/element.cpp` interact
// with it through Connection::element_table().

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

struct IUIAutomation;
struct IUIAutomationElement;

namespace remote_hands {

class ElementTable {
public:
    ElementTable();
    ~ElementTable();
    ElementTable(const ElementTable&)            = delete;
    ElementTable& operator=(const ElementTable&) = delete;

    // Registers an element and returns its wire id (e.g. "elt:42"). The
    // table takes a fresh ref via AddRef; callers that hold a ref of their
    // own should still Release it when done.
    std::string register_element(IUIAutomationElement* elem);

    // Borrowed lookup. Returns nullptr if id is unknown or malformed.
    IUIAutomationElement* lookup(std::string_view id) const;

    // Removes and Releases. Idempotent on unknown ids.
    void unregister(std::string_view id);

    // Lazily-created IUIAutomation for this connection. Returns nullptr on
    // COM failure. Borrowed.
    IUIAutomation* uia();

private:
    mutable std::mutex                                    mu_;
    std::unordered_map<unsigned, IUIAutomationElement*>   elements_;
    unsigned                                              next_id_ = 1;
    IUIAutomation*                                        uia_     = nullptr;
};

}  // namespace remote_hands
