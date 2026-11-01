# LiteView

輕巧、快速的 Windows 看圖程式，操作方式比照 ACDSee 3.2：瀏覽模式（資料夾樹＋縮圖清單＋預覽窗）與檢視模式（單張看圖）用 Enter 切換。

- 規格：[docs/spec.md](docs/spec.md)
- 架構：[docs/architecture.md](docs/architecture.md)
- 開發規範：[AGENTS.md](AGENTS.md)

## 建置

需要 Visual Studio 2022（含「使用 C++ 的桌面開發」）與 CMake 3.25 以上。

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

執行檔在 `build\Release\LiteView.exe`。每次推送到 GitHub 後，Actions 也會建置並把執行檔上傳成 artifact。
