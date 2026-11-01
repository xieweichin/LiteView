#include "core/PdfRenderer.h"

#include <inspectable.h>
#include <roapi.h>
#include <shcore.h>
#include <shlwapi.h>
#include <winstring.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <new>

using Microsoft::WRL::ComPtr;

namespace {

// Minimal declarations of the Windows.Data.Pdf ABI (from windows.data.pdf.idl
// in the Windows SDK). Only the vtable order and the IIDs matter; parameters
// LiteView does not use are typed loosely.
struct PdfSize { float width; float height; };
struct PdfRect { float x; float y; float width; float height; };
struct PdfColor { BYTE a; BYTE r; BYTE g; BYTE b; };

struct IAsyncInfoLite : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_Id(UINT32* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Status(int* status) = 0;  // 0 started, 1 completed, 2 canceled, 3 error
    virtual HRESULT STDMETHODCALLTYPE get_ErrorCode(HRESULT* error) = 0;
    virtual HRESULT STDMETHODCALLTYPE Cancel() = 0;
    virtual HRESULT STDMETHODCALLTYPE Close() = 0;
};

// Windows.Foundation.IAsyncAction
struct IAsyncActionLite : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults() = 0;
};

struct IPdfPageLite;

struct IPdfDocumentLite : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE GetPage(UINT32 index, IPdfPageLite** page) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PageCount(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsPasswordProtected(boolean* value) = 0;
};

// Windows.Foundation.IAsyncOperation<PdfDocument>
struct IPdfDocumentOperation : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(IPdfDocumentLite** document) = 0;
};

struct IPdfDocumentStaticsLite : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileAsync(IInspectable* file, IPdfDocumentOperation** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromFileWithPasswordAsync(IInspectable* file, HSTRING password,
                                                                    IPdfDocumentOperation** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamAsync(IInspectable* stream, IPdfDocumentOperation** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadFromStreamWithPasswordAsync(IInspectable* stream, HSTRING password,
                                                                      IPdfDocumentOperation** op) = 0;
};

struct IPdfPageRenderOptionsLite : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_SourceRect(PdfRect* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_SourceRect(PdfRect value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationWidth(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationWidth(UINT32 value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DestinationHeight(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_DestinationHeight(UINT32 value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BackgroundColor(PdfColor* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BackgroundColor(PdfColor value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsIgnoringHighContrast(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsIgnoringHighContrast(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BitmapEncoderId(GUID* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_BitmapEncoderId(GUID value) = 0;
};

struct IPdfPageLite : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RenderToStreamAsync(IInspectable* stream, IAsyncActionLite** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE RenderWithOptionsToStreamAsync(IInspectable* stream,
                                                                     IPdfPageRenderOptionsLite* options,
                                                                     IAsyncActionLite** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE PreparePageAsync(IAsyncActionLite** op) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Index(UINT32* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(PdfSize* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Dimensions(IInspectable** value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Rotation(int* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PreferredZoom(float* value) = 0;
};

// Windows.Storage.Streams.IRandomAccessStream
const IID kIidRandomAccessStream = {0x905A0FE1, 0xBC53, 0x11DF, {0x8C, 0x49, 0x00, 0x1E, 0x4F, 0xC6, 0x86, 0xDA}};
const IID kIidAsyncInfo = {0x00000036, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIidPdfDocumentStatics = {0x433A0B5F, 0xC007, 0x4788, {0x90, 0xF2, 0x08, 0x14, 0x3D, 0x92, 0x25, 0x99}};
const IID kIidPdfPageRenderOptions = {0x3C98056F, 0xB7CF, 0x4C29, {0x9A, 0x04, 0x52, 0xD9, 0x02, 0x67, 0xF4, 0x25}};
constexpr UINT kMaxPixels = 4096;

class HString {
public:
    explicit HString(const wchar_t* text) {
        result = WindowsCreateString(text, static_cast<UINT32>(wcslen(text)), &value);
    }
    ~HString() { if (value) WindowsDeleteString(value); }
    HString(const HString&) = delete;
    HString& operator=(const HString&) = delete;
    HSTRING value = nullptr;
    HRESULT result = E_FAIL;
};

// Waits for a WinRT async operation on this worker thread.
HRESULT Wait(IInspectable* operation) {
    ComPtr<IAsyncInfoLite> info;
    HRESULT hr = operation->QueryInterface(kIidAsyncInfo, reinterpret_cast<void**>(info.GetAddressOf()));
    if (FAILED(hr)) return hr;
    // Renders take tens of milliseconds: yield for the first few, then
    // sleep (Sleep(1) can last a whole 15.6 ms timer tick).
    const ULONGLONG start = GetTickCount64();
    int status = 0;
    for (;;) {
        hr = info->get_Status(&status);
        if (FAILED(hr)) return hr;
        if (status != 0) break;
        const ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed > 60000) {  // give up on a stuck document
            info->Cancel();
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        if (elapsed < 20) {
            SwitchToThread();
        } else {
            Sleep(1);
        }
    }
    if (status == 1) return S_OK;
    HRESULT error = E_FAIL;
    if (status == 3 && SUCCEEDED(info->get_ErrorCode(&error)) && FAILED(error)) return error;
    return status == 2 ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : E_FAIL;
}

HRESULT Activate(const wchar_t* className, ComPtr<IInspectable>& out) {
    HString name(className);
    if (FAILED(name.result)) return name.result;
    return RoActivateInstance(name.value, out.GetAddressOf());
}

thread_local int g_lastStep = 0;

}  // namespace

int LastPdfRenderStep() noexcept { return g_lastStep; }

std::wstring PdfPagePath(const std::wstring& file, UINT page) {
    if (page == 0) return file;
    return file + L"|" + std::to_wstring(page);
}

bool SplitPdfPagePath(const std::wstring& path, std::wstring& file, UINT& page) {
    const auto bar = path.find_last_of(L'|');
    if (bar != std::wstring::npos && bar + 1 < path.size()) {
        const wchar_t* digits = path.c_str() + bar + 1;
        wchar_t* end = nullptr;
        const unsigned long value = wcstoul(digits, &end, 10);
        if (end && *end == L'\0' && value > 0 && value < 1000000) {
            file = path.substr(0, bar);
            page = static_cast<UINT>(value);
            return true;
        }
    }
    file = path;
    page = 0;
    return false;
}

bool IsPdfPath(const std::wstring& path) noexcept {
    std::wstring file;
    UINT page = 0;
    try {
        SplitPdfPagePath(path, file, page);
    } catch (...) {
        return false;
    }
    return _wcsicmp(PathFindExtensionW(file.c_str()), L".pdf") == 0;
}

HRESULT RenderPdfPage(const std::wstring& file, UINT page, UINT maxSide, ComPtr<IWICBitmapSource>& bitmap,
                      UINT& width, UINT& height, UINT& fullWidth, UINT& fullHeight, UINT& pageCount) noexcept {
    bitmap.Reset();
    g_lastStep = 0;
    width = height = fullWidth = fullHeight = pageCount = 0;
    try {
        // Open the file as a WinRT stream (no StorageFile broker needed).
        ComPtr<IInspectable> input;
        HRESULT hr = CreateRandomAccessStreamOnFile(file.c_str(), 0 /* FileAccessMode.Read */, kIidRandomAccessStream,
                                                    reinterpret_cast<void**>(input.GetAddressOf()));
        if (FAILED(hr)) { g_lastStep = 1; return hr; }

        HString className(L"Windows.Data.Pdf.PdfDocument");
        if (FAILED(className.result)) return className.result;
        ComPtr<IPdfDocumentStaticsLite> statics;
        hr = RoGetActivationFactory(className.value, kIidPdfDocumentStatics,
                                    reinterpret_cast<void**>(statics.GetAddressOf()));
        if (FAILED(hr)) { g_lastStep = 2; return hr; }
        ComPtr<IPdfDocumentOperation> load;
        hr = statics->LoadFromStreamAsync(input.Get(), load.GetAddressOf());
        if (FAILED(hr)) { g_lastStep = 3; return hr; }
        hr = Wait(load.Get());
        if (FAILED(hr)) { g_lastStep = 4; return hr; }
        ComPtr<IPdfDocumentLite> document;
        hr = load->GetResults(document.GetAddressOf());
        if (FAILED(hr)) { g_lastStep = 5; return hr; }
        if (!document) { g_lastStep = 90; return E_FAIL; }

        UINT32 count = 0;
        hr = document->get_PageCount(&count);
        if (FAILED(hr)) { g_lastStep = 6; return hr; }
        if (count == 0) return WINCODEC_ERR_BADIMAGE;
        if (page >= count) page = count - 1;
        ComPtr<IPdfPageLite> pdfPage;
        hr = document->GetPage(page, pdfPage.GetAddressOf());
        if (FAILED(hr)) { g_lastStep = 7; return hr; }
        PdfSize size{};
        hr = pdfPage->get_Size(&size);
        if (FAILED(hr)) { g_lastStep = 8; return hr; }
        if (!(size.width > 0.5f) || !(size.height > 0.5f)) return WINCODEC_ERR_BADIMAGE;

        // Full size: 2 px per DIP, capped; then fit maxSide without enlarging.
        const double longest = std::max(size.width, size.height);
        const double fullSide = std::min<double>(kMaxPixels, longest * 2.0);
        double full = fullSide;
        if (maxSide && maxSide < full) full = maxSide;
        const double scale = full / longest;
        const UINT w = std::max(1u, static_cast<UINT>(std::lround(size.width * scale)));
        const UINT h = std::max(1u, static_cast<UINT>(std::lround(size.height * scale)));

        ComPtr<IInspectable> optionsObject;
        hr = Activate(L"Windows.Data.Pdf.PdfPageRenderOptions", optionsObject);
        if (FAILED(hr)) { g_lastStep = 9; return hr; }
        ComPtr<IPdfPageRenderOptionsLite> options;
        hr = optionsObject->QueryInterface(kIidPdfPageRenderOptions, reinterpret_cast<void**>(options.GetAddressOf()));
        if (FAILED(hr)) { g_lastStep = 10; return hr; }
        options->put_DestinationWidth(w);
        options->put_DestinationHeight(h);

        // BMP is cheaper to write and read back than the default PNG, but not
        // every Windows build accepts it: fall back to PNG on failure.
        ComPtr<IInspectable> output;
        for (int attempt = 0; attempt < 3; ++attempt) {
            // BitmapEncoderId takes WinRT encoder ids, which are the WIC encoder CLSIDs.
            if (attempt < 2) options->put_BitmapEncoderId(attempt == 0 ? CLSID_WICBmpEncoder : CLSID_WICPngEncoder);
            ComPtr<IInspectable> memory;
            hr = Activate(L"Windows.Storage.Streams.InMemoryRandomAccessStream", memory);
            if (FAILED(hr)) { g_lastStep = 11; return hr; }
            output.Reset();
            hr = memory->QueryInterface(kIidRandomAccessStream, reinterpret_cast<void**>(output.GetAddressOf()));
            if (FAILED(hr)) { g_lastStep = 12; return hr; }
            ComPtr<IAsyncActionLite> render;
            // Last resort: default options (PNG at the page's own size).
            hr = attempt < 2 ? pdfPage->RenderWithOptionsToStreamAsync(output.Get(), options.Get(), render.GetAddressOf())
                             : pdfPage->RenderToStreamAsync(output.Get(), render.GetAddressOf());
            if (FAILED(hr)) { g_lastStep = 13 + 100 * attempt; continue; }
            hr = Wait(render.Get());
            if (FAILED(hr)) { g_lastStep = 14 + 100 * attempt; continue; }
            hr = render->GetResults();
            if (FAILED(hr)) { g_lastStep = 15 + 100 * attempt; continue; }
            break;
        }
        if (FAILED(hr)) return hr;

        // Read the rendered bitmap back with WIC.
        ComPtr<IStream> stream;
        hr = CreateStreamOverRandomAccessStream(output.Get(), IID_PPV_ARGS(stream.GetAddressOf()));
        if (FAILED(hr)) { g_lastStep = 16; return hr; }
        LARGE_INTEGER zero{};
        hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr);
        if (FAILED(hr)) { g_lastStep = 17; return hr; }
        ComPtr<IWICImagingFactory> factory;
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(hr)) { g_lastStep = 18; return hr; }
        ComPtr<IWICBitmapDecoder> decoder;
        hr = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf());
        if (FAILED(hr)) { g_lastStep = 19; return hr; }
        ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, frame.GetAddressOf());
        if (FAILED(hr)) { g_lastStep = 20; return hr; }
        ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(converter.GetAddressOf());
        if (FAILED(hr)) { g_lastStep = 21; return hr; }
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) { g_lastStep = 22; return hr; }
        ComPtr<IWICBitmap> cached;
        hr = factory->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, cached.GetAddressOf());
        if (FAILED(hr)) { g_lastStep = 23; return hr; }
        hr = cached->GetSize(&width, &height);
        if (FAILED(hr)) { g_lastStep = 24; return hr; }
        bitmap = cached;
        pageCount = count;
        fullWidth = std::max(1u, static_cast<UINT>(std::lround(size.width * fullSide / longest)));
        fullHeight = std::max(1u, static_cast<UINT>(std::lround(size.height * fullSide / longest)));
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        g_lastStep = 99;
        return E_FAIL;
    }
}
