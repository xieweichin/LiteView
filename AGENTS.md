# LiteView 開發規範

開始任何工作前，請先閱讀 `docs/spec.md`（功能規格）與 `docs/architecture.md`（系統架構）。

1. 只用 C++17 與 Windows SDK，不引入任何第三方函式庫（doctest 只在 tests/ 內）。
2. `core/` 不可使用 HWND、視窗訊息或 `ui/` 的任何標頭檔。
3. 所有字串用 `std::wstring`；介面文字一律放在 `LiteView.rc` 字串表，程式碼內不寫死中文。
4. COM 物件一律用 `ComPtr`；GDI 控制代碼用 RAII 包裝，不可洩漏。
5. 錯誤回傳 `HRESULT`，不用例外；使用者看得到的錯誤要有中文訊息。
6. UI 執行緒上不可做解碼、列檔或任何可能超過 16 毫秒的工作。
7. 每個工作單只改該單列出的檔案；要改介面約定時先在 PR 說明理由。
8. `core/` 新增功能要附 doctest 測試，CI 全綠才算完成。
9. Release 版執行檔超過 2 MB 視為失敗，CI 會檢查。
10. 程式碼註解用繁體中文或英文皆可，識別字一律英文。

## 建置

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

執行檔在 `build\Release\LiteView.exe`。
