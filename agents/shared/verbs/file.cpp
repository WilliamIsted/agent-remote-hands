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

// `file.*` and `directory.*` namespace verb handlers (one translation unit;
// the wire-namespace split lives in agents/shared/capabilities.cpp).
//
// Implements the verbs whose contracts are the spec JSON under
// protocol/spec/verbs/common/{file,directory}.*.json (the single source of
// truth). Registered handlers (see capabilities.cpp):
//
//   file.read         (R)  read    {path,encoding,offset,length}
//   file.write        (U)  write   {path,content,encoding,atomic}
//   file.write_at     (U)  write_at{path,offset,content,encoding,truncate}
//   file.stat         (R)  stat    {path}
//   file.delete       (D)  delete_ {path}
//   file.exists       (R)  exists  {path}
//   file.wait         (R)  wait    {glob,timeout_ms}
//   file.rename       (U)  rename  {src,dst,overwrite,cross_fs}
//   directory.list    (R)  list          {path,recursive,pattern,limit,
//                                          offset,sort,reverse,full}
//   directory.stat    (R)  directory_stat   {path}
//   directory.exists  (R)  directory_exists {path}
//   directory.create  (C)  mkdir            {path,parents,mode}
//   directory.rename  (U)  directory_rename {src,dst,overwrite,cross_fs}
//   directory.delete  (D)  directory_remove {path,recursive}
//
// PHASE 2.1 — NAMED-ARG MIGRATION. These handlers no longer index `req.args`
// positionally with ad-hoc `--flag` scanning or whitespace-joined multi-token
// path reconstruction. Each declares its input_schema property list (IN
// SCHEMA ORDER) and reads each value by NAME through the shared SchemaArgs
// resolver (schema_args.hpp), with the schema's property ORDER used as the
// positional fallback when the caller invoked the verb positionally (the v2.2
// reference client packs positional calls as `{"_args":[...]}`). The
// `window.*` namespace was the pattern-setter; this file follows it (and
// system.cpp / process.cpp / clipboard.cpp / registry.cpp) exactly.
// Validation emits the same ErrorCode::InvalidArgs + {"message":...}
// ergonomics as before. v2.2 header quoting (PROTOCOL.md 1.2.5) and the MCP
// named-args object both deliver `path` as one string, taken verbatim.
//
// ISSUE #93 — `flags` array. file.stat / file.exists / directory.list /
// directory.stat emit a `flags` array: the Win32 FILE_ATTRIBUTE_* set sourced
// zero-cost from GetFileAttributesEx / WIN32_FIND_DATA output, mapped to the
// spec's exact flag enum tokens (readonly, hidden, system, archive, ...).
//
// BINARY PAYLOAD — PHASE 2b. file.read / file.write / file.write_at carry
// content via a schema `content` string + `encoding` discriminator (the v2.2
// binary side-channel). All NON-payload args (path / offset / length /
// encoding / atomic / truncate) are refactored to named here; the actual
// content decode/encode is the Phase-2b side-channel and is DEFERRED — the
// arg shape is validated, then the spec-declared deferral code is emitted
// (each of these three verbs' x-errors is exactly
// ["not_found","permission_denied","invalid_args"], so the deferral uses
// invalid_args with an explicit {"reason":...} — not_supported is NOT in
// their x-errors). See report. The positional-path implementation that read
// raw payload bytes off the wire is gone (it predated the schema `content`
// model and never matched the v2.2 contract).
//
// ERROR-CODE DISCIPLINE. Every emission is within the DISPATCHED verb's spec
// x-errors. `not_a_directory` (file.delete, directory.list, directory.stat,
// directory.rename, directory.delete) is emitted via ErrorCode::NotADirectory
// and `cross_device` (file.rename, directory.rename) via ErrorCode::CrossDevice
// — both added to agents/shared/errors.hpp during this migration.
//
// Paths are UTF-8 on the wire; converted to wide-char internally.
// FILETIME values are converted to Unix epoch seconds.

#include "../connection.hpp"
#include "../errors.hpp"
#include "../json.hpp"
#include "../log.hpp"
#include "../text_util.hpp"
#include "args.hpp"
#include "schema_args.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>   // PathMatchSpecW — directory.list `pattern` name filter

#pragma comment(lib, "shlwapi.lib")

namespace remote_hands::file_verbs {

// The Phase-2.1 named-argument resolver and its invalid_args helper live in
// the shared header (schema_args.hpp) so every namespace reads through one
// definition. Pull them into this TU's unqualified name lookup; behaviour is
// identical to window.cpp / system.cpp / process.cpp / clipboard.cpp /
// registry.cpp.
using wire::SchemaArgs;
using wire::invalid_args;

namespace {

constexpr long long kFiletimeUnixDelta = 116444736000000000LL;  // 100ns intervals

long long filetime_to_unix(const FILETIME& ft) {
    ULARGE_INTEGER u{};
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (static_cast<long long>(u.QuadPart) - kFiletimeUnixDelta) / 10000000LL;
}

// Spec type enum for file.stat / file.exists / file.wait / directory.list:
// "file" | "directory" | "link" | "other". (file.exists additionally uses
// "absent" when exists is false; that string is emitted at the call site.)
// Reparse points -> "link"; directories -> "directory"; everything else with
// a valid attribute set -> "file" (regular file). INVALID_FILE_ATTRIBUTES is
// the caller's "absent" signal and is handled before this is reached.
std::string_view attribute_type(DWORD attrs) {
    if (attrs == INVALID_FILE_ATTRIBUTES)        return "other";
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT)    return "link";
    if (attrs & FILE_ATTRIBUTE_DIRECTORY)        return "directory";
    return "file";
}

// Issue #93 — append a `flags` array of the spec's FILE_ATTRIBUTE_* tokens
// currently set in `attrs`. Tokens are exactly the spec enum strings; the
// cloud-file flags (recall_on_*) are Windows 10+ and named via numeric
// constants so the legacy/XP toolchain (no Win10 SDK headers required) still
// compiles. Emitted in the spec enum's declared order. The key is appended
// (so the caller positions the comma); the value is a JSON array.
void append_flags_array(std::string& out, DWORD attrs) {
    // Win10-only attribute bits, defined numerically for toolchains whose
    // SDK predates them (FILE_ATTRIBUTE_RECALL_ON_* — winnt.h, Win 8/10).
    constexpr DWORD kAttrRecallOnOpen        = 0x00040000;  // RECALL_ON_OPEN
    constexpr DWORD kAttrRecallOnDataAccess  = 0x00400000;  // RECALL_ON_DATA_ACCESS

    json::append_string(out, "flags");
    out += ":[";
    bool first = true;
    auto emit = [&](bool set, std::string_view token) {
        if (!set) return;
        if (!first) out += ',';
        first = false;
        json::append_string(out, token);
    };
    emit((attrs & FILE_ATTRIBUTE_READONLY)      != 0, "readonly");
    emit((attrs & FILE_ATTRIBUTE_HIDDEN)        != 0, "hidden");
    emit((attrs & FILE_ATTRIBUTE_SYSTEM)        != 0, "system");
    emit((attrs & FILE_ATTRIBUTE_ARCHIVE)       != 0, "archive");
    emit((attrs & FILE_ATTRIBUTE_TEMPORARY)     != 0, "temporary");
    emit((attrs & FILE_ATTRIBUTE_COMPRESSED)    != 0, "compressed");
    emit((attrs & FILE_ATTRIBUTE_ENCRYPTED)     != 0, "encrypted");
    emit((attrs & FILE_ATTRIBUTE_SPARSE_FILE)   != 0, "sparse_file");
    emit((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0, "reparse_point");
    emit((attrs & FILE_ATTRIBUTE_OFFLINE)       != 0, "offline");
    emit((attrs & FILE_ATTRIBUTE_NOT_CONTENT_INDEXED) != 0, "not_indexed");
    emit((attrs & kAttrRecallOnDataAccess)      != 0, "recall_on_data_access");
    emit((attrs & kAttrRecallOnOpen)            != 0, "recall_on_open");
    out += ']';
}

// Resolve the required `path` property. Emits the spec-declared invalid_args
// on a missing/empty path (invalid_args is in EVERY file/directory verb's
// x-errors). The pre-2.1 multi-token path-reconstruction is gone.
bool resolve_path(Connection& conn, SchemaArgs& args, std::string_view verb,
                  std::string& path_out) {
    std::optional<std::string> path = args.str("path");
    if (!path || path->empty()) {
        invalid_args(conn, std::string(verb) + " requires 'path'");
        return false;
    }
    path_out = std::move(*path);
    return true;
}

// Map a Win32 GetLastError() into the spec error vocabulary for a file/dir
// open/query failure, constrained to the codes the calling verb declares.
// Every file/dir x-errors set that uses this contains BOTH not_found and
// permission_denied (read/write/stat/delete/rename/list). The pre-2.1
// helper funnelled non-not-found failures into `not_supported`, which is
// OUTSIDE every one of these verbs' x-errors — corrected to the spec-declared
// permission_denied (carrying the raw win32 status for diagnosis); reported.
void write_open_err(Connection& conn, std::string_view path) {
    const DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ||
        err == ERROR_INVALID_NAME   || err == ERROR_BAD_NETPATH    ||
        err == ERROR_BAD_PATHNAME) {
        std::string detail = "{";
        json::append_kv_string(detail, "path", path);
        detail += '}';
        conn.writer().write_err(ErrorCode::NotFound, detail);
        return;
    }
    char detail[96];
    std::snprintf(detail, sizeof(detail), "{\"win32_error\":%lu}", err);
    conn.writer().write_err(ErrorCode::PermissionDenied, detail);
}

}  // namespace

// ---------------------------------------------------------------------------
// file.read — input_schema (schema order): ["path","encoding","offset","length"]
// x-errors: ["not_found","permission_denied","invalid_args"].
//
// PHASE 2b — the `content` output is the binary side-channel (decoded text in
// the requested encoding, or base64 for `encoding: binary`). Path / encoding
// / offset / length are validated here as named args; the byte read +
// encode is deferred. not_supported is NOT in this verb's x-errors, so the
// deferral is surfaced via the spec-declared invalid_args with an explicit
// {"reason":...}.

void read(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path", "encoding", "offset", "length"});

    std::string path;
    if (!resolve_path(conn, args, "file.read", path)) return;

    if (args.present("encoding")) {
        auto enc = args.str("encoding");
        if (!enc) {
            invalid_args(conn, "file.read 'encoding' must be a string");
            return;
        }
    }
    if (args.present("offset")) {
        auto off = args.integer("offset");
        if (!off || *off < 0) {
            invalid_args(conn,
                         "file.read 'offset' must be a non-negative integer");
            return;
        }
    }
    if (args.present("length")) {
        auto len = args.integer("length");
        if (!len || *len < 0) {
            invalid_args(conn,
                         "file.read 'length' must be a non-negative integer");
            return;
        }
    }

    // PHASE 2b — content read/encode is the binary side-channel; deferred.
    invalid_args(conn,
                 "file.read content side-channel is deferred to Phase 2b "
                 "(reason: file_content_phase2b)");
}

// ---------------------------------------------------------------------------
// file.write — input_schema (schema order): ["path","content","encoding",
// "atomic"]. x-errors: ["not_found","permission_denied","invalid_args"].
//
// PHASE 2b — `content` is the binary side-channel. Path / encoding / atomic
// are validated as named args; the decode + write is deferred via the
// spec-declared invalid_args (not_supported is NOT in this verb's x-errors).

void write(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path", "content", "encoding", "atomic"});

    std::string path;
    if (!resolve_path(conn, args, "file.write", path)) return;

    // `content` is REQUIRED by file.write.json; validate presence + string
    // shape (the actual decode is the deferred side-channel).
    if (!args.present("content")) {
        invalid_args(conn, "file.write requires 'content'");
        return;
    }
    if (!args.str("content")) {
        invalid_args(conn, "file.write 'content' must be a string");
        return;
    }
    if (args.present("encoding") && !args.str("encoding")) {
        invalid_args(conn, "file.write 'encoding' must be a string");
        return;
    }
    if (args.present("atomic") && !args.boolean("atomic")) {
        invalid_args(conn, "file.write 'atomic' must be a boolean");
        return;
    }

    // PHASE 2b — content decode/write is the binary side-channel; deferred.
    invalid_args(conn,
                 "file.write content side-channel is deferred to Phase 2b "
                 "(reason: file_content_phase2b)");
}

// ---------------------------------------------------------------------------
// file.write_at — input_schema (schema order): ["path","offset","content",
// "encoding","truncate"]. x-errors:
// ["not_found","permission_denied","invalid_args"].
// x-conditional: truncate:true requires offset:0 (else invalid_args).
//
// PHASE 2b — `content` is the binary side-channel. Path / offset / encoding /
// truncate (incl. the offset==0 conditional) are validated as named args;
// the decode + random-access write is deferred via invalid_args.

void write_at(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path", "offset", "content", "encoding", "truncate"});

    std::string path;
    if (!resolve_path(conn, args, "file.write_at", path)) return;

    // offset — required, non-negative integer.
    if (!args.present("offset")) {
        invalid_args(conn, "file.write_at requires 'offset'");
        return;
    }
    auto offset = args.integer("offset");
    if (!offset || *offset < 0) {
        invalid_args(conn,
                     "file.write_at 'offset' must be a non-negative integer");
        return;
    }

    // content — required string (the deferred side-channel).
    if (!args.present("content")) {
        invalid_args(conn, "file.write_at requires 'content'");
        return;
    }
    if (!args.str("content")) {
        invalid_args(conn, "file.write_at 'content' must be a string");
        return;
    }

    if (args.present("encoding") && !args.str("encoding")) {
        invalid_args(conn, "file.write_at 'encoding' must be a string");
        return;
    }

    bool truncate = false;
    if (args.present("truncate")) {
        auto t = args.boolean("truncate");
        if (!t) {
            invalid_args(conn,
                         "file.write_at 'truncate' must be a boolean");
            return;
        }
        truncate = *t;
    }
    // x-conditional: truncate:true requires offset:0 -> invalid_args.
    if (truncate && *offset != 0) {
        invalid_args(conn,
                     "file.write_at 'truncate: true' requires 'offset: 0'");
        return;
    }

    // PHASE 2b — content decode + random-access write is the binary
    // side-channel; deferred.
    invalid_args(conn,
                 "file.write_at content side-channel is deferred to Phase 2b "
                 "(reason: file_content_phase2b)");
}

// ---------------------------------------------------------------------------
// directory.list — input_schema (schema order): ["path","recursive",
// "pattern","limit","offset","sort","reverse","full"].
// x-errors: ["not_found","not_a_directory","permission_denied",
// "invalid_args"]. The non-directory case emits ErrorCode::NotADirectory.

void list(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path", "recursive", "pattern", "limit", "offset",
                          "sort", "reverse", "full"});

    std::string path;
    if (!resolve_path(conn, args, "directory.list", path)) return;

    bool recursive = false;
    if (args.present("recursive")) {
        auto r = args.boolean("recursive");
        if (!r) {
            invalid_args(conn,
                         "directory.list 'recursive' must be a boolean");
            return;
        }
        recursive = *r;
    }

    std::string name_filter;  // empty => no filter
    if (args.present("pattern")) {
        auto p = args.str("pattern");
        if (!p) {
            invalid_args(conn, "directory.list 'pattern' must be a string");
            return;
        }
        name_filter = std::move(*p);
    }

    int limit = 200;
    if (args.present("limit")) {
        auto l = args.integer32("limit");
        if (!l || *l < 1) {
            invalid_args(conn,
                         "directory.list 'limit' must be an integer >= 1");
            return;
        }
        limit = *l;
    }

    int offset = 0;
    if (args.present("offset")) {
        auto o = args.integer32("offset");
        if (!o || *o < 0) {
            invalid_args(conn,
                         "directory.list 'offset' must be a non-negative "
                         "integer");
            return;
        }
        offset = *o;
    }

    std::string sort_key = "name";
    if (args.present("sort")) {
        auto s = args.str("sort");
        if (!s || (*s != "name" && *s != "mtime" &&
                   *s != "size" && *s != "ctime")) {
            invalid_args(conn,
                         "directory.list 'sort' must be name|mtime|size|ctime");
            return;
        }
        sort_key = std::move(*s);
    }

    bool reverse = false;
    if (args.present("reverse")) {
        auto r = args.boolean("reverse");
        if (!r) {
            invalid_args(conn,
                         "directory.list 'reverse' must be a boolean");
            return;
        }
        reverse = *r;
    }

    if (args.present("full")) {
        auto f = args.boolean("full");
        if (!f) {
            invalid_args(conn, "directory.list 'full' must be a boolean");
            return;
        }
        if (*f) limit = INT_MAX;
    }

    const std::wstring wroot = text::utf8_to_wide(path);

    // Existence + is-a-directory check up front so not_found /
    // not-a-directory surface as the spec codes (the bare FindFirstFile
    // approach cannot distinguish "missing dir" from "empty dir").
    const DWORD root_attrs = GetFileAttributesW(wroot.c_str());
    if (root_attrs == INVALID_FILE_ATTRIBUTES) {
        write_open_err(conn, path);
        return;
    }
    if ((root_attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        conn.writer().write_err(
            ErrorCode::NotADirectory,
            "{\"message\":\"directory.list 'path' is a file, not a directory\"}");
        return;
    }

    struct Entry {
        std::string name;        // relative path (forward-slash) when recursive
        std::string type;
        std::uint64_t size;
        long long  mtime_unix;
        long long  ctime_unix;
        long long  atime_unix;
        DWORD      attrs;
        bool       is_dir;
    };
    std::vector<Entry> entries;

    const std::wstring wfilter =
        name_filter.empty() ? std::wstring() : text::utf8_to_wide(name_filter);

    // Depth-first walk. `rel_prefix` is the forward-slash relative path of the
    // directory currently being scanned (empty at the root). FindFirst/Next is
    // a LOOP (no C-ABI callback), so no exception-barrier concern here.
    auto join = [](const std::wstring& a, const std::wstring& b) {
        std::wstring r = a;
        if (!r.empty() && r.back() != L'\\' && r.back() != L'/') r += L'\\';
        r += b;
        return r;
    };

    std::vector<std::pair<std::wstring, std::string>> stack;  // (dir, rel)
    stack.emplace_back(wroot, std::string());
    while (!stack.empty()) {
        const std::wstring dir = stack.back().first;
        const std::string  rel = stack.back().second;
        stack.pop_back();

        std::wstring pattern = join(dir, L"*");
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (std::wcscmp(fd.cFileName, L".") == 0 ||
                std::wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }

            const bool is_dir =
                (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool is_reparse =
                (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

            const bool name_ok =
                wfilter.empty() ||
                PathMatchSpecW(fd.cFileName, wfilter.c_str());

            std::string entry_name =
                text::wide_to_utf8(fd.cFileName, std::wcslen(fd.cFileName));
            std::string rel_name = rel.empty()
                ? entry_name
                : (rel + "/" + entry_name);

            if (name_ok) {
                ULARGE_INTEGER sz{};
                sz.LowPart  = fd.nFileSizeLow;
                sz.HighPart = fd.nFileSizeHigh;
                entries.push_back({
                    rel_name,
                    std::string(attribute_type(fd.dwFileAttributes)),
                    sz.QuadPart,
                    filetime_to_unix(fd.ftLastWriteTime),
                    filetime_to_unix(fd.ftCreationTime),
                    filetime_to_unix(fd.ftLastAccessTime),
                    fd.dwFileAttributes,
                    is_dir
                });
            }

            // Recurse into real subdirectories only (reparse points are NOT
            // followed — they appear as `link` entries, per the spec).
            if (recursive && is_dir && !is_reparse) {
                stack.emplace_back(join(dir, fd.cFileName), rel_name);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Sort. name: directories before files, then case-insensitive.
    // mtime/size/ctime: highest-first (newest / largest).
    auto cmp = [&](const Entry& a, const Entry& b) -> bool {
        if (sort_key == "name") {
            if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
            for (std::size_t k = 0; k < a.name.size() && k < b.name.size(); ++k) {
                const auto ca =
                    std::tolower(static_cast<unsigned char>(a.name[k]));
                const auto cb =
                    std::tolower(static_cast<unsigned char>(b.name[k]));
                if (ca != cb) return ca < cb;
            }
            return a.name.size() < b.name.size();
        }
        if (sort_key == "mtime") return a.mtime_unix > b.mtime_unix;
        if (sort_key == "size")  return a.size       > b.size;
        if (sort_key == "ctime") return a.ctime_unix > b.ctime_unix;
        return false;
    };
    std::sort(entries.begin(), entries.end(), cmp);
    if (reverse) std::reverse(entries.begin(), entries.end());

    // Apply offset + limit and emit. x-output-schema per entry:
    // {name,type,size,mtime_unix_s,ctime_unix_s,atime_unix_s,flags} — all
    // required, additionalProperties:false.
    const auto start = static_cast<std::size_t>(offset);
    std::string body = "{\"entries\":[";
    bool first   = true;
    int  emitted = 0;
    for (std::size_t i = start; i < entries.size() && emitted < limit; ++i) {
        const auto& e = entries[i];
        if (!first) body += ',';
        first = false;
        body += '{';
        json::append_kv_string(body, "name", e.name);          body += ',';
        json::append_kv_string(body, "type", e.type);          body += ',';
        json::append_kv_uint(body, "size", e.size);            body += ',';
        json::append_kv_int(body, "mtime_unix_s", e.mtime_unix); body += ',';
        json::append_kv_int(body, "ctime_unix_s", e.ctime_unix); body += ',';
        json::append_kv_int(body, "atime_unix_s", e.atime_unix); body += ',';
        append_flags_array(body, e.attrs);
        body += '}';
        ++emitted;
    }
    body += "]}";
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// file.stat — input_schema: ["path"].
// x-errors: ["not_found","permission_denied","invalid_args"].
// x-output-schema: {type,size,mtime_unix_s,ctime_unix_s,atime_unix_s,flags}
// all required, additionalProperties:false.

void stat(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path"});

    std::string path;
    if (!resolve_path(conn, args, "file.stat", path)) return;

    const std::wstring wpath = text::utf8_to_wide(path);
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &info)) {
        write_open_err(conn, path);
        return;
    }

    ULARGE_INTEGER sz{};
    sz.LowPart  = info.nFileSizeLow;
    sz.HighPart = info.nFileSizeHigh;

    std::string body = "{";
    json::append_kv_string(body, "type",
                           attribute_type(info.dwFileAttributes));
    body += ',';
    json::append_kv_uint(body, "size", sz.QuadPart);
    body += ',';
    json::append_kv_int(body, "mtime_unix_s",
                        filetime_to_unix(info.ftLastWriteTime));
    body += ',';
    json::append_kv_int(body, "ctime_unix_s",
                        filetime_to_unix(info.ftCreationTime));
    body += ',';
    json::append_kv_int(body, "atime_unix_s",
                        filetime_to_unix(info.ftLastAccessTime));
    body += ',';
    append_flags_array(body, info.dwFileAttributes);
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// file.delete — input_schema: ["path"].
// x-errors: ["not_found","permission_denied","not_a_directory",
// "invalid_args"]. A directory target emits ErrorCode::NotADirectory.
// x-output-schema: null — wire response is the literal OK 0.

void delete_(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path"});

    std::string path;
    if (!resolve_path(conn, args, "file.delete", path)) return;

    const std::wstring wpath = text::utf8_to_wide(path);
    const DWORD attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        write_open_err(conn, path);
        return;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        // file.delete.json: "Returns ERR not_a_directory if path resolves to
        // a directory" (not_a_directory is in file.delete x-errors).
        conn.writer().write_err(
            ErrorCode::NotADirectory,
            "{\"message\":\"file.delete 'path' is a directory; "
            "use directory.delete\"}");
        return;
    }

    if (!DeleteFileW(wpath.c_str())) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            conn.writer().write_err(ErrorCode::NotFound);
            return;
        }
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", err);
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }
    conn.writer().write_ok();  // x-output-schema: null (OK 0)
}

// ---------------------------------------------------------------------------
// file.exists — input_schema: ["path"].
// x-errors: ["permission_denied","invalid_args"]. (NOT not_found — a missing
// path is the success answer {exists:false,type:"absent",flags:[]}.)
// x-output-schema: {exists,type,flags} all required.

void exists(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path"});

    std::string path;
    if (!resolve_path(conn, args, "file.exists", path)) return;

    const std::wstring wpath = text::utf8_to_wide(path);
    const DWORD attrs = GetFileAttributesW(wpath.c_str());

    std::string body = "{";
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        // A genuine ACL denial is the spec's permission_denied; a
        // not-found / bad-path is the success answer "absent".
        if (err == ERROR_ACCESS_DENIED) {
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", err);
            conn.writer().write_err(ErrorCode::PermissionDenied, detail);
            return;
        }
        json::append_kv_bool(body, "exists", false);
        body += ',';
        json::append_kv_string(body, "type", "absent");
        body += ',';
        json::append_string(body, "flags");
        body += ":[]";
    } else {
        json::append_kv_bool(body, "exists", true);
        body += ',';
        json::append_kv_string(body, "type", attribute_type(attrs));
        body += ',';
        append_flags_array(body, attrs);
    }
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// file.wait — input_schema (schema order): ["glob","timeout_ms"].
// x-errors: ["timeout","permission_denied","invalid_args"].
// x-output-schema: {path,type} both required.
//
// Polls FindFirstFileW at 100 ms cadence until a match appears or the
// timeout expires.

void wait(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"glob", "timeout_ms"});

    std::optional<std::string> glob = args.str("glob");
    if (!glob || glob->empty()) {
        invalid_args(conn, "file.wait requires 'glob'");
        return;
    }

    long long timeout = 30000;  // default per file.wait.json
    if (args.present("timeout_ms")) {
        auto t = args.integer("timeout_ms");
        if (!t || *t < 0) {
            invalid_args(conn,
                         "file.wait 'timeout_ms' must be a non-negative "
                         "integer");
            return;
        }
        timeout = *t;
    }

    const std::wstring pattern = text::utf8_to_wide(*glob);

    // Directory portion to prefix to FindFirstFile results.
    std::wstring dir_prefix;
    if (const auto sep = pattern.find_last_of(L"\\/");
        sep != std::wstring::npos) {
        dir_prefix = pattern.substr(0, sep + 1);
    }

    using clock = std::chrono::steady_clock;
    const auto deadline =
        clock::now() + std::chrono::milliseconds(timeout);

    while (true) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            std::wstring full = dir_prefix;
            full.append(fd.cFileName);
            const DWORD matched_attrs = fd.dwFileAttributes;
            FindClose(h);

            std::string body = "{";
            json::append_kv_string(body, "path", text::wide_to_utf8(full));
            body += ',';
            json::append_kv_string(body, "type",
                                   attribute_type(matched_attrs));
            body += '}';
            conn.writer().write_ok(body);
            return;
        }

        if (clock::now() >= deadline) {
            conn.writer().write_err(ErrorCode::Timeout);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

// ---------------------------------------------------------------------------
// directory.create — input_schema (schema order): ["path","parents","mode"].
// x-errors: ["already_exists","permission_denied","not_found",
// "invalid_args"]. `mode` is accepted then ignored on Windows (NTFS uses
// ACLs, not POSIX bits — fields_ignored:["mode"] in x-families).
// x-output-schema: {created} required.

void mkdir(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path", "parents", "mode"});

    std::string path;
    if (!resolve_path(conn, args, "directory.create", path)) return;

    bool parents = false;
    if (args.present("parents")) {
        auto p = args.boolean("parents");
        if (!p) {
            invalid_args(conn,
                         "directory.create 'parents' must be a boolean");
            return;
        }
        parents = *p;
    }

    if (args.present("mode")) {
        // Validated for type/range then ignored on Windows.
        auto m = args.integer32("mode");
        if (!m || *m < 0 || *m > 511) {
            invalid_args(conn,
                         "directory.create 'mode' must be an integer in "
                         "[0, 511]");
            return;
        }
    }

    const std::wstring wpath = text::utf8_to_wide(path);

    if (parents) {
        // mkdir -p: create each missing component from the root. Existing
        // intermediates are skipped, not errored. A final-component
        // already-existing directory is still already_exists.
        const DWORD final_attrs = GetFileAttributesW(wpath.c_str());
        if (final_attrs != INVALID_FILE_ATTRIBUTES &&
            (final_attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            conn.writer().write_err(
                ErrorCode::AlreadyExists,
                "{\"message\":\"directory already exists\"}");
            return;
        }
        std::wstring acc;
        std::size_t i = 0;
        while (i <= wpath.size()) {
            if (i == wpath.size() ||
                wpath[i] == L'\\' || wpath[i] == L'/') {
                if (!acc.empty()) {
                    // Skip a bare drive spec ("C:") — not a creatable dir.
                    const bool drive_only =
                        acc.size() == 2 && acc[1] == L':';
                    if (!drive_only) {
                        if (!CreateDirectoryW(acc.c_str(), nullptr)) {
                            const DWORD err = GetLastError();
                            if (err != ERROR_ALREADY_EXISTS) {
                                if (err == ERROR_PATH_NOT_FOUND ||
                                    err == ERROR_FILE_NOT_FOUND) {
                                    conn.writer().write_err(
                                        ErrorCode::NotFound);
                                    return;
                                }
                                char d[64];
                                std::snprintf(d, sizeof(d),
                                              "{\"win32_error\":%lu}", err);
                                conn.writer().write_err(
                                    ErrorCode::PermissionDenied, d);
                                return;
                            }
                        }
                    }
                }
                if (i < wpath.size()) acc += wpath[i];
            } else {
                acc += wpath[i];
            }
            ++i;
        }
        conn.writer().write_ok("{\"created\":true}");
        return;
    }

    if (!CreateDirectoryW(wpath.c_str(), nullptr)) {
        const DWORD err = GetLastError();
        if (err == ERROR_ALREADY_EXISTS) {
            conn.writer().write_err(
                ErrorCode::AlreadyExists,
                "{\"message\":\"directory already exists\"}");
            return;
        }
        if (err == ERROR_PATH_NOT_FOUND || err == ERROR_FILE_NOT_FOUND) {
            // A missing parent with parents:false is the spec's not_found.
            conn.writer().write_err(
                ErrorCode::NotFound,
                "{\"message\":\"parent directory does not exist; pass "
                "parents:true\"}");
            return;
        }
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", err);
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }
    conn.writer().write_ok("{\"created\":true}");
}

// ---------------------------------------------------------------------------
// Shared rename body for file.rename and directory.rename. Both have
// input_schema (schema order) ["src","dst","overwrite","cross_fs"] and the
// same x-output-schema {renamed,fallback_used}. They differ in x-errors and
// in the source-type precondition:
//   file.rename      x-errors: not_found, already_exists, cross_device,
//                              permission_denied, invalid_args
//   directory.rename x-errors: not_found, already_exists, permission_denied,
//                              cross_device, not_a_directory, invalid_args
// `cross_device` (cross-FS without cross_fs) emits ErrorCode::CrossDevice;
// `not_a_directory` (directory.rename, non-directory src) emits
// ErrorCode::NotADirectory. Both are in the respective verbs' x-errors.
namespace {

void rename_impl(Connection& conn, const wire::Request& req,
                 std::string_view verb, bool require_directory) {
    SchemaArgs args(req, {"src", "dst", "overwrite", "cross_fs"});

    std::optional<std::string> src = args.str("src");
    if (!src || src->empty()) {
        invalid_args(conn, std::string(verb) + " requires 'src'");
        return;
    }
    std::optional<std::string> dst = args.str("dst");
    if (!dst || dst->empty()) {
        invalid_args(conn, std::string(verb) + " requires 'dst'");
        return;
    }

    bool overwrite = false;
    if (args.present("overwrite")) {
        auto o = args.boolean("overwrite");
        if (!o) {
            invalid_args(conn,
                         std::string(verb) +
                         " 'overwrite' must be a boolean");
            return;
        }
        overwrite = *o;
    }

    bool cross_fs = false;
    if (args.present("cross_fs")) {
        auto c = args.boolean("cross_fs");
        if (!c) {
            invalid_args(conn,
                         std::string(verb) +
                         " 'cross_fs' must be a boolean");
            return;
        }
        cross_fs = *c;
    }

    const std::wstring wsrc = text::utf8_to_wide(*src);
    const std::wstring wdst = text::utf8_to_wide(*dst);

    const DWORD src_attrs = GetFileAttributesW(wsrc.c_str());
    if (src_attrs == INVALID_FILE_ATTRIBUTES) {
        conn.writer().write_err(ErrorCode::NotFound,
                                "{\"message\":\"src does not exist\"}");
        return;
    }
    const bool src_is_dir =
        (src_attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (require_directory && !src_is_dir) {
        conn.writer().write_err(
            ErrorCode::NotADirectory,
            "{\"message\":\"directory.rename 'src' is a file, "
            "not a directory\"}");
        return;
    }
    if (!require_directory && src_is_dir) {
        // file.rename: a directory src is the spec's invalid_args (the
        // file/dir split is explicit — "use directory.rename for
        // directories"). not_a_directory is not applicable here.
        invalid_args(conn,
                     "file.rename 'src' is a directory; use directory.rename");
        return;
    }

    // Without overwrite, an existing dst is already_exists (in both verbs'
    // x-errors). MoveFileExW without MOVEFILE_REPLACE_EXISTING also fails on
    // an existing dst, but the explicit probe yields the precise code.
    if (!overwrite &&
        GetFileAttributesW(wdst.c_str()) != INVALID_FILE_ATTRIBUTES) {
        conn.writer().write_err(
            ErrorCode::AlreadyExists,
            "{\"message\":\"dst already exists; pass overwrite:true\"}");
        return;
    }

    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (overwrite) flags |= MOVEFILE_REPLACE_EXISTING;
    if (cross_fs)  flags |= MOVEFILE_COPY_ALLOWED;

    // x-output-schema: fallback_used == "copy_delete" only when the
    // cross-filesystem copy+delete path was actually engaged. MoveFileExW
    // with MOVEFILE_COPY_ALLOWED only copies+deletes when src/dst are on
    // different volumes; a same-volume move (even with cross_fs) is an
    // atomic rename. Resolve the volume mount points BEFORE the move
    // (GetVolumePathNameW walks up to an existing parent, so a not-yet-
    // existing dst leaf is fine) and only claim "copy_delete" when we are
    // confident the volumes differ.
    auto volume_of = [](const std::wstring& p) -> std::wstring {
        wchar_t vol[MAX_PATH];
        if (GetVolumePathNameW(p.c_str(), vol, MAX_PATH)) return vol;
        return std::wstring();
    };
    const std::wstring vsrc = volume_of(wsrc);
    const std::wstring vdst = volume_of(wdst);
    const bool cross_volume =
        !vsrc.empty() && !vdst.empty() &&
        _wcsicmp(vsrc.c_str(), vdst.c_str()) != 0;

    if (!MoveFileExW(wsrc.c_str(), wdst.c_str(), flags)) {
        const DWORD err = GetLastError();
        if (err == ERROR_NOT_SAME_DEVICE) {
            conn.writer().write_err(
                ErrorCode::CrossDevice,
                "{\"message\":\"" + std::string(verb) +
                " src and dst are on different filesystems; "
                "pass cross_fs:true\"}");
            return;
        }
        if (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS) {
            conn.writer().write_err(
                ErrorCode::AlreadyExists,
                "{\"message\":\"dst already exists; pass overwrite:true\"}");
            return;
        }
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            conn.writer().write_err(ErrorCode::NotFound);
            return;
        }
        char detail[64];
        std::snprintf(detail, sizeof(detail),
                      "{\"win32_error\":%lu}", err);
        conn.writer().write_err(ErrorCode::PermissionDenied, detail);
        return;
    }

    // x-output-schema: {renamed, fallback_used} both required.
    std::string body = "{";
    json::append_kv_bool(body, "renamed", true);
    body += ',';
    json::append_kv_string(body, "fallback_used",
                           (cross_fs && cross_volume) ? "copy_delete"
                                                      : "none");
    body += '}';
    conn.writer().write_ok(body);
}

}  // namespace

// ---------------------------------------------------------------------------
// file.rename — files only.

void rename(Connection& conn, const wire::Request& req) {
    rename_impl(conn, req, "file.rename", /*require_directory=*/false);
}

// ---------------------------------------------------------------------------
// directory.stat — input_schema: ["path"].
// x-errors: ["not_found","permission_denied","not_a_directory",
// "invalid_args"]. The non-directory case emits ErrorCode::NotADirectory.
// x-output-schema: {type,entry_count,mtime_unix_s,flags} all required;
// type is the const "directory".

void directory_stat(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path"});

    std::string path;
    if (!resolve_path(conn, args, "directory.stat", path)) return;

    const std::wstring wpath = text::utf8_to_wide(path);

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &fad)) {
        write_open_err(conn, path);
        return;
    }
    if ((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        conn.writer().write_err(
            ErrorCode::NotADirectory,
            "{\"message\":\"directory.stat 'path' is a file, "
            "not a directory\"}");
        return;
    }

    std::wstring pattern = wpath;
    if (!pattern.empty() && pattern.back() != L'\\' &&
        pattern.back() != L'/') {
        pattern += L'\\';
    }
    pattern += L'*';

    std::uint64_t entry_count = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (std::wcscmp(fd.cFileName, L".") == 0 ||
                std::wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            ++entry_count;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    std::string body = "{";
    json::append_kv_string(body, "type", "directory");
    body += ',';
    json::append_kv_uint(body, "entry_count", entry_count);
    body += ',';
    json::append_kv_int(body, "mtime_unix_s",
                        filetime_to_unix(fad.ftLastWriteTime));
    body += ',';
    append_flags_array(body, fad.dwFileAttributes);
    body += '}';
    conn.writer().write_ok(body);
}

// ---------------------------------------------------------------------------
// directory.exists — input_schema: ["path"].
// x-errors: ["permission_denied","invalid_args"]. (NOT not_found — a missing
// path is the success answer {exists:false}.)
// x-output-schema: {exists} required.

void directory_exists(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path"});

    std::string path;
    if (!resolve_path(conn, args, "directory.exists", path)) return;

    const std::wstring wpath = text::utf8_to_wide(path);
    const DWORD attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD err = GetLastError();
        if (err == ERROR_ACCESS_DENIED) {
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", err);
            conn.writer().write_err(ErrorCode::PermissionDenied, detail);
            return;
        }
        conn.writer().write_ok("{\"exists\":false}");
        return;
    }
    const bool is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    conn.writer().write_ok(is_dir ? "{\"exists\":true}"
                                  : "{\"exists\":false}");
}

// ---------------------------------------------------------------------------
// directory.rename — directories only.

void directory_rename(Connection& conn, const wire::Request& req) {
    rename_impl(conn, req, "directory.rename", /*require_directory=*/true);
}

// ---------------------------------------------------------------------------
// directory.delete — input_schema (schema order): ["path","recursive"].
// x-errors: ["not_found","not_empty","permission_denied","not_a_directory",
// "invalid_args"]. A file target emits ErrorCode::NotADirectory.
// x-output-schema: {entries_removed} required.

namespace {

// Recursive removal of a directory's contents and the directory itself.
// `removed` counts each file/subdirectory removed (excludes the top-level
// directory). FindFirst/Next is a LOOP — no C-ABI callback boundary, so the
// recursion needs no exception barrier. Every FindFirstFileW is paired with
// FindClose on all paths.
bool remove_recursive(const std::wstring& dir, std::uint64_t& removed) {
    std::wstring pattern = dir;
    if (!pattern.empty() && pattern.back() != L'\\' &&
        pattern.back() != L'/') {
        pattern += L'\\';
    }
    pattern += L'*';

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (std::wcscmp(fd.cFileName, L".") == 0 ||
                std::wcscmp(fd.cFileName, L"..") == 0) {
                continue;
            }
            std::wstring child = dir;
            if (!child.empty() && child.back() != L'\\' &&
                child.back() != L'/') {
                child += L'\\';
            }
            child += fd.cFileName;
            const bool is_dir =
                (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool is_reparse =
                (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (is_dir && !is_reparse) {
                if (!remove_recursive(child, removed)) {
                    FindClose(h);
                    return false;
                }
            } else {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
                    SetFileAttributesW(
                        child.c_str(),
                        fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                }
                // A reparse-point directory: remove the link, not its
                // target (RemoveDirectoryW on the junction itself).
                const bool ok = (is_dir && is_reparse)
                    ? (RemoveDirectoryW(child.c_str()) != 0)
                    : (DeleteFileW(child.c_str()) != 0);
                if (!ok) {
                    FindClose(h);
                    return false;
                }
                ++removed;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    if (!RemoveDirectoryW(dir.c_str())) {
        return false;
    }
    ++removed;
    return true;
}

}  // namespace

void directory_remove(Connection& conn, const wire::Request& req) {
    SchemaArgs args(req, {"path", "recursive"});

    std::string path;
    if (!resolve_path(conn, args, "directory.delete", path)) return;

    bool recursive = false;
    if (args.present("recursive")) {
        auto r = args.boolean("recursive");
        if (!r) {
            invalid_args(conn,
                         "directory.delete 'recursive' must be a boolean");
            return;
        }
        recursive = *r;
    }

    const std::wstring wpath = text::utf8_to_wide(path);
    const DWORD attrs = GetFileAttributesW(wpath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        write_open_err(conn, path);
        return;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        conn.writer().write_err(
            ErrorCode::NotADirectory,
            "{\"message\":\"directory.delete 'path' is a file, "
            "not a directory\"}");
        return;
    }

    std::uint64_t entries_removed = 0;
    if (recursive) {
        if (!remove_recursive(wpath, entries_removed)) {
            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND ||
                err == ERROR_PATH_NOT_FOUND) {
                conn.writer().write_err(ErrorCode::NotFound);
                return;
            }
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", err);
            conn.writer().write_err(ErrorCode::PermissionDenied, detail);
            return;
        }
        // remove_recursive counts the top-level dir; the wire's
        // entries_removed reports only the entries INSIDE the target.
        entries_removed = entries_removed > 0 ? entries_removed - 1 : 0;
    } else {
        if (!RemoveDirectoryW(wpath.c_str())) {
            const DWORD err = GetLastError();
            if (err == ERROR_DIR_NOT_EMPTY) {
                conn.writer().write_err(
                    ErrorCode::NotEmpty,
                    "{\"message\":\"directory not empty; pass "
                    "recursive:true\"}");
                return;
            }
            if (err == ERROR_FILE_NOT_FOUND ||
                err == ERROR_PATH_NOT_FOUND) {
                conn.writer().write_err(ErrorCode::NotFound);
                return;
            }
            char detail[64];
            std::snprintf(detail, sizeof(detail),
                          "{\"win32_error\":%lu}", err);
            conn.writer().write_err(ErrorCode::PermissionDenied, detail);
            return;
        }
    }

    char body[64];
    std::snprintf(body, sizeof(body),
        "{\"entries_removed\":%llu}",
        static_cast<unsigned long long>(entries_removed));
    conn.writer().write_ok(body);
}

}  // namespace remote_hands::file_verbs
