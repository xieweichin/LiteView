#pragma once

#include <windows.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

enum class RenameIssue {
    None,
    Duplicate,     // two files would get the same name
    InvalidName,   // empty, reserved or illegal characters
    PathTooLong,   // full path would exceed MAX_PATH
    Exists,        // another file (not being renamed) already has the name
};

struct RenamePlanItem {
    std::wstring from;  // full path
    std::wstring to;    // full path
    RenameIssue issue = RenameIssue::None;
    bool Changes() const;
};

struct RenameOptions {
    // "旅遊_###": # is the sequence number with one digit per #, * the
    // original name without extension, {date:yyyyMMdd} the date taken.
    std::wstring pattern;
    int startNumber = 1;
    bool lowercaseExtension = false;
};

// Moves one file; tests replace it to inject failures.
using MoveFunction = std::function<bool(const std::wstring& from, const std::wstring& to)>;
using ProgressFunction = std::function<void(std::size_t done, std::size_t total)>;
// Date used by {date:...} for a file: EXIF date taken, else modified time.
using DateFunction = std::function<SYSTEMTIME(const std::wstring& path)>;

class BatchRename {
public:
    // `paths` in list order (the order numbers are given out).
    static std::vector<RenamePlanItem> Preview(const std::vector<std::wstring>& paths, const RenameOptions& options,
                                               const DateFunction& dateOf);
    static bool HasConflicts(const std::vector<RenamePlanItem>& plan);
    // Expands the pattern for one file (no extension); exposed for tests.
    static std::wstring ExpandName(const std::wstring& pattern, int number, const std::wstring& originalStem,
                                   const SYSTEMTIME* date);

    // Renames in two phases (all to temporary names, then to the targets) so
    // swaps like A→B, B→A work. Any failure puts every file back. On success
    // the reverse mapping is written to `undoFile` (if not empty).
    static HRESULT Execute(const std::vector<RenamePlanItem>& plan, const std::wstring& undoFile,
                           const ProgressFunction& progress = nullptr, const MoveFunction& move = nullptr);

    static bool CanUndo(const std::wstring& undoFile);
    // Reverses the last Execute recorded in `undoFile` and deletes the record.
    static HRESULT UndoLast(const std::wstring& undoFile, const ProgressFunction& progress = nullptr,
                            const MoveFunction& move = nullptr);

    // Date for {date:...}: EXIF date taken if present, otherwise the file's
    // modified time (local). The calling thread must have COM initialized.
    static SYSTEMTIME DateTakenOrModified(const std::wstring& path);
};
