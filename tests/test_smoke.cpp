#include "doctest.h"

#include <string>

TEST_CASE("toolchain: wide strings and C++17 are available") {
    std::wstring name = L"LiteView";
    CHECK(name.size() == 8);
    if constexpr (sizeof(wchar_t) == 2) {
        CHECK(name[0] == L'L');
    }
}
