#include "core/BatchRename.h"

#include <algorithm>
#include <cwchar>
#include <unordered_map>
#include <unordered_set>

#include "core/FileOps.h"
#include "core/Metadata.h"

namespace {

std::wstring Lower(std::wstring text) {
    if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

void SplitPath(const std::wstring& path, std::wstring& folder, std::wstring& stem, std::wstring& extension) {
    const auto slash = path.find_last_of(L"\\/");
    folder = slash == std::wstring::npos ? std::wstring() : path.substr(0, slash + 1);
    const std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) {
        stem = name;
        extension.clear();
    } else {
        stem = name.substr(0, dot);
        extension = name.substr(dot);
    }
}

std::wstring Pad(int value, std::size_t digits) {
    std::wstring text = std::to_wstring(value < 0 ? -value : value);
    if (text.size() < digits) text.insert(0, digits - text.size(), L'0');
    return value < 0 ? L"-" + text : text;
}

// yyyy yy MM dd HH mm ss; anything else is copied.
std::wstring FormatDate(const std::wstring& format, const SYSTEMTIME& t) {
    std::wstring out;
    for (std::size_t i = 0; i < format.size();) {
        auto starts = [&](const wchar_t* token) { return format.compare(i, wcslen(token), token) == 0; };
        if (starts(L"yyyy")) { out += Pad(t.wYear, 4); i += 4; }
        else if (starts(L"yy")) { out += Pad(t.wYear % 100, 2); i += 2; }
        else if (starts(L"MM")) { out += Pad(t.wMonth, 2); i += 2; }
        else if (starts(L"dd")) { out += Pad(t.wDay, 2); i += 2; }
        else if (starts(L"HH")) { out += Pad(t.wHour, 2); i += 2; }
        else if (starts(L"mm")) { out += Pad(t.wMinute, 2); i += 2; }
        else if (starts(L"ss")) { out += Pad(t.wSecond, 2); i += 2; }
        else { out += format[i]; ++i; }
    }
    return out;
}

bool DefaultMove(const std::wstring& from, const std::wstring& to) {
    return MoveFileExW(from.c_str(), to.c_str(), 0) != FALSE;
}

std::wstring TempName(const std::wstring& folder, std::size_t index) {
    return folder + L"~lvrename-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) +
           L"-" + std::to_wstring(index) + L".tmp";
}

// Two-phase rename of the changing items, all or nothing.
HRESULT Run(const std::vector<RenamePlanItem>& plan, const ProgressFunction& progress, const MoveFunction& moveIn) {
    const MoveFunction move = moveIn ? moveIn : MoveFunction(&DefaultMove);
    std::vector<const RenamePlanItem*> items;
    for (const auto& item : plan) {
        if (item.Changes()) items.push_back(&item);
    }
    const std::size_t total = items.size() * 2;
    std::vector<std::wstring> temps(items.size());
    std::size_t phase1 = 0, phase2 = 0;
    HRESULT failure = S_OK;

    for (; phase1 < items.size(); ++phase1) {
        std::wstring folder, stem, ext;
        SplitPath(items[phase1]->from, folder, stem, ext);
        temps[phase1] = TempName(folder, phase1);
        if (!move(items[phase1]->from, temps[phase1])) {
            failure = HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_GEN_FAILURE);
            break;
        }
        if (progress && phase1 % 16 == 0) progress(phase1, total);
    }
    if (SUCCEEDED(failure)) {
        for (; phase2 < items.size(); ++phase2) {
            if (!move(temps[phase2], items[phase2]->to)) {
                failure = HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_GEN_FAILURE);
                break;
            }
            if (progress && phase2 % 16 == 0) progress(items.size() + phase2, total);
        }
    }
    if (SUCCEEDED(failure)) {
        if (progress) progress(total, total);
        return S_OK;
    }
    // Roll back in reverse order. Use the real file system: an injected
    // failure must not also block the repair.
    for (std::size_t i = phase2; i-- > 0;) DefaultMove(items[i]->to, temps[i]);
    for (std::size_t i = phase1; i-- > 0;) DefaultMove(temps[i], items[i]->from);
    return failure;
}

HRESULT WriteUndo(const std::wstring& file, const std::vector<RenamePlanItem>& plan) {
    // UTF-16LE with BOM; one "new<TAB>old" pair per line.
    std::wstring text(1, L'\xFEFF');
    for (const auto& item : plan) {
        if (!item.Changes()) continue;
        text += item.to + L'\t' + item.from + L"\r\n";
    }
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    DWORD written = 0;
    const DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    const BOOL ok = WriteFile(handle, text.data(), bytes, &written, nullptr);
    CloseHandle(handle);
    return ok && written == bytes ? S_OK : HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
}

HRESULT ReadUndo(const std::wstring& file, std::vector<RenamePlanItem>& plan) {
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
    LARGE_INTEGER size{};
    GetFileSizeEx(handle, &size);
    if (size.QuadPart > 64ll * 1024 * 1024) {
        CloseHandle(handle);
        return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    }
    std::wstring text(static_cast<std::size_t>(size.QuadPart) / sizeof(wchar_t), L'\0');
    DWORD read = 0;
    const BOOL ok = text.empty() || ReadFile(handle, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &read, nullptr);
    CloseHandle(handle);
    if (!ok) return HRESULT_FROM_WIN32(ERROR_READ_FAULT);
    if (!text.empty() && text[0] == L'\xFEFF') text.erase(0, 1);
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find(L"\r\n", at);
        if (end == std::wstring::npos) end = text.size();
        const std::wstring line = text.substr(at, end - at);
        at = end + 2;
        const auto tab = line.find(L'\t');
        if (tab == std::wstring::npos) continue;
        RenamePlanItem item;
        item.from = line.substr(0, tab);  // current (renamed) name
        item.to = line.substr(tab + 1);   // name to restore
        plan.push_back(std::move(item));
    }
    return plan.empty() ? HRESULT_FROM_WIN32(ERROR_INVALID_DATA) : S_OK;
}

}  // namespace

bool RenamePlanItem::Changes() const { return from != to; }

std::wstring BatchRename::ExpandName(const std::wstring& pattern, int number, const std::wstring& originalStem,
                                     const SYSTEMTIME* date) {
    std::wstring out;
    for (std::size_t i = 0; i < pattern.size();) {
        const wchar_t c = pattern[i];
        if (c == L'#') {
            std::size_t run = 0;
            while (i + run < pattern.size() && pattern[i + run] == L'#') ++run;
            out += Pad(number, run);
            i += run;
        } else if (c == L'*') {
            out += originalStem;
            ++i;
        } else if (c == L'{' && pattern.compare(i, 6, L"{date:") == 0) {
            const auto close = pattern.find(L'}', i);
            if (close == std::wstring::npos) {
                out += pattern.substr(i);
                break;
            }
            if (date) out += FormatDate(pattern.substr(i + 6, close - i - 6), *date);
            i = close + 1;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

std::vector<RenamePlanItem> BatchRename::Preview(const std::vector<std::wstring>& paths, const RenameOptions& options,
                                                 const DateFunction& dateOf) {
    std::vector<RenamePlanItem> plan;
    plan.reserve(paths.size());
    const bool needsDate = options.pattern.find(L"{date:") != std::wstring::npos;
    std::vector<std::wstring> names;  // new file names, for validation
    std::vector<bool> blankStem;
    int number = options.startNumber;
    for (const auto& path : paths) {
        std::wstring folder, stem, ext;
        SplitPath(path, folder, stem, ext);
        SYSTEMTIME date{};
        if (needsDate && dateOf) date = dateOf(path);
        std::wstring name = ExpandName(options.pattern, number++, stem, needsDate ? &date : nullptr);
        if (options.lowercaseExtension) ext = Lower(ext);
        RenamePlanItem item;
        item.from = path;
        item.to = folder + name + ext;
        blankStem.push_back(name.find_first_not_of(L' ') == std::wstring::npos);
        names.push_back(name + ext);
        plan.push_back(std::move(item));
    }

    // Duplicates within the plan.
    std::unordered_map<std::wstring, std::size_t> seen;
    std::unordered_set<std::wstring> sources;
    for (const auto& item : plan) sources.insert(Lower(item.from));
    for (std::size_t i = 0; i < plan.size(); ++i) {
        RenamePlanItem& item = plan[i];
        if (blankStem[i] || !FileOps::IsValidFileName(names[i])) {
            item.issue = RenameIssue::InvalidName;
            continue;
        }
        if (item.to.size() >= MAX_PATH) {
            item.issue = RenameIssue::PathTooLong;
            continue;
        }
        const std::wstring key = Lower(item.to);
        auto it = seen.find(key);
        if (it != seen.end()) {
            item.issue = RenameIssue::Duplicate;
            if (plan[it->second].issue == RenameIssue::None) plan[it->second].issue = RenameIssue::Duplicate;
            continue;
        }
        seen.emplace(key, i);
        // A file that exists and is not one of the files being renamed.
        if (item.Changes() && !sources.count(key) && GetFileAttributesW(item.to.c_str()) != INVALID_FILE_ATTRIBUTES) {
            item.issue = RenameIssue::Exists;
        }
    }
    return plan;
}

bool BatchRename::HasConflicts(const std::vector<RenamePlanItem>& plan) {
    return std::any_of(plan.begin(), plan.end(), [](const RenamePlanItem& item) { return item.issue != RenameIssue::None; });
}

HRESULT BatchRename::Execute(const std::vector<RenamePlanItem>& plan, const std::wstring& undoFile,
                             const ProgressFunction& progress, const MoveFunction& move) {
    if (HasConflicts(plan)) return E_INVALIDARG;
    try {
        const HRESULT hr = Run(plan, progress, move);
        if (FAILED(hr)) return hr;
        if (!undoFile.empty()) WriteUndo(undoFile, plan);  // the renames stand even if the record fails
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
}

bool BatchRename::CanUndo(const std::wstring& undoFile) {
    return !undoFile.empty() && GetFileAttributesW(undoFile.c_str()) != INVALID_FILE_ATTRIBUTES;
}

HRESULT BatchRename::UndoLast(const std::wstring& undoFile, const ProgressFunction& progress, const MoveFunction& move) {
    try {
        std::vector<RenamePlanItem> plan;
        HRESULT hr = ReadUndo(undoFile, plan);
        if (FAILED(hr)) return hr;
        // Every renamed file must still be there, and nothing else may have
        // taken an original name.
        std::unordered_set<std::wstring> current;
        for (const auto& item : plan) current.insert(Lower(item.from));
        for (const auto& item : plan) {
            if (GetFileAttributesW(item.from.c_str()) == INVALID_FILE_ATTRIBUTES) {
                return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            }
            if (!current.count(Lower(item.to)) && GetFileAttributesW(item.to.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
            }
        }
        hr = Run(plan, progress, move);
        if (SUCCEEDED(hr)) DeleteFileW(undoFile.c_str());
        return hr;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
}

SYSTEMTIME BatchRename::DateTakenOrModified(const std::wstring& path) {
    SYSTEMTIME result{};
    PhotoMetadata meta;
    if (SUCCEEDED(Metadata().Read(path, meta)) && !meta.dateTaken.empty()) {
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
        if (swscanf(meta.dateTaken.c_str(), L"%d:%d:%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) >= 3 && y > 1600 &&
            mo >= 1 && mo <= 12 && d >= 1 && d <= 31) {
            result.wYear = static_cast<WORD>(y);
            result.wMonth = static_cast<WORD>(mo);
            result.wDay = static_cast<WORD>(d);
            result.wHour = static_cast<WORD>(h);
            result.wMinute = static_cast<WORD>(mi);
            result.wSecond = static_cast<WORD>(s);
            return result;
        }
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    FILETIME local{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) &&
        FileTimeToLocalFileTime(&data.ftLastWriteTime, &local)) {
        FileTimeToSystemTime(&local, &result);
    }
    return result;
}
