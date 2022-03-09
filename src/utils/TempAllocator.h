/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// note: include BaseUtil.h instead of including directly

Allocator* GetTempAllocator();
void DestroyTempAllocator();
void ResetTempAllocator();

// exists just to mark the intent of the variable
// and provide convenience operators
struct TempStr {
    std::string_view sv{};

    TempStr() = default;
    explicit TempStr(char* s) {
        sv = {s, str::Len(s)};
    }
    explicit TempStr(char* s, size_t cch) {
        CrashIf((int)cch < 0);
        sv = {s, cch};
    }
    explicit TempStr(const char* s) {
        sv = {s, str::Len(s)};
    }
    explicit TempStr(const char* s, size_t cch) {
        CrashIf((int)cch < 0);
        sv = {s, cch};
    }
    explicit TempStr(std::string_view s) {
        sv = s;
    }
    TempStr(const TempStr& ts) {
        sv = ts.sv;
    }
    [[nodiscard]] bool empty() const {
        return sv.empty();
    }
    [[nodiscard]] size_t size() const {
        return sv.size();
    }
    [[nodiscard]] char* Get() const {
        return (char*)sv.data();
    }
    [[nodiscard]] std::string_view AsView() const {
        return sv;
    }
    [[nodiscard]] operator const char*() const { // NOLINT
        return sv.data();
    }
    [[nodiscard]] operator char*() const { // NOLINT
        return (char*)sv.data();
    }
    [[nodiscard]] operator std::string_view() const { // NOLINT
        return sv;
    }
};

struct TempWstr {
    std::wstring_view sv{};

    TempWstr() = default;
    explicit TempWstr(WCHAR* s) {
        sv = {s, str::Len(s)};
    }
    explicit TempWstr(WCHAR* s, size_t cch) {
        sv = {s, cch};
    }
    explicit TempWstr(const WCHAR* s) {
        sv = {s, str::Len(s)};
    }
    explicit TempWstr(const WCHAR* s, size_t cch) {
        sv = {s, cch};
    }
    explicit TempWstr(std::wstring_view s) {
        sv = s;
    }
    TempWstr(const TempWstr& ts) {
        sv = ts.sv;
    }
    [[nodiscard]] bool empty() const {
        return sv.empty();
    }
    [[nodiscard]] size_t size() const {
        return sv.size();
    }
    [[nodiscard]] WCHAR* Get() const {
        return (WCHAR*)sv.data();
    }
    [[nodiscard]] std::wstring_view AsView() const {
        return sv;
    }
    [[nodiscard]] operator const WCHAR*() const { // NOLINT
        return sv.data();
    }
    [[nodiscard]] operator WCHAR*() const { // NOLINT
        return (WCHAR*)sv.data();
    }
    explicit operator std::wstring_view() const {
        return sv;
    }
};

namespace str {
TempStr DupTemp(const char* s, size_t cb = (size_t)-1);
TempWstr DupTemp(const WCHAR* s, size_t cch = (size_t)-1);

TempStr DupTemp(std::string_view);
TempWstr DupTemp(std::wstring_view);

TempStr JoinTemp(const char* s1, const char* s2, const char* s3 = nullptr);
TempWstr JoinTemp(const WCHAR* s1, const WCHAR* s2, const WCHAR* s3 = nullptr);
} // namespace str

TempStr ToUtf8Temp(const WCHAR* s, size_t cch = (size_t)-1);
TempStr ToUtf8Temp(std::wstring_view);
TempWstr ToWstrTemp(const char* s, size_t cb = (size_t)-1);
TempWstr ToWstrTemp(std::string_view sv);
