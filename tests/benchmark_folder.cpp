#include "core/FolderModel.h"

#include <chrono>
#include <cstdio>
#include <future>

// Run manually against a disposable directory of images. The timer includes
// WIC decoder discovery, directory enumeration, and natural-name sorting.
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fwprintf(stderr, L"Usage: LiteViewFolderBenchmark <directory>\n");
        return 2;
    }
    using namespace std::chrono;
    std::promise<std::uint32_t> completed;
    auto result = completed.get_future();
    FolderModel model;
    const auto start = steady_clock::now();
    const auto generation = model.Open(argv[1], [&](std::uint32_t value) { completed.set_value(value); });
    if (generation == 0 || result.wait_for(seconds(30)) != std::future_status::ready) {
        std::fwprintf(stderr, L"Folder enumeration did not complete.\n");
        return 1;
    }
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - start).count();
    const HRESULT hr = model.Commit(result.get());
    if (FAILED(hr)) {
        std::fwprintf(stderr, L"Folder enumeration failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        return 1;
    }
    std::printf("files=%zu elapsed_ms=%lld\n", model.Count(), static_cast<long long>(elapsed));
    return 0;
}
