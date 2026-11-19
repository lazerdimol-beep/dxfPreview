// Explorer thumbnail provider for .dxf files (IThumbnailProvider).
#include "win_common.h"
#include "render_gdiplus.h"

#include <cmath>
#include <new>

namespace {

const unsigned long long kMaxThumbFileSize = 300ull * 1024 * 1024;

HBITMAP CreateDib(int w, int h, void** bits) {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, bits, nullptr, 0);
}

class DxfThumbnailProvider : public IInitializeWithStream, public IThumbnailProvider {
public:
    DxfThumbnailProvider() { ++g_dllRef; }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == __uuidof(IInitializeWithStream))
            *ppv = static_cast<IInitializeWithStream*>(this);
        else if (riid == __uuidof(IThumbnailProvider))
            *ppv = static_cast<IThumbnailProvider*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        long r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return (ULONG)r;
    }

    // IInitializeWithStream
    IFACEMETHODIMP Initialize(IStream* stream, DWORD) override {
        if (stream_) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        if (!stream) return E_INVALIDARG;
        return stream->QueryInterface(IID_PPV_ARGS(&stream_));
    }

    // IThumbnailProvider
    IFACEMETHODIMP GetThumbnail(UINT cx, HBITMAP* phbmp, WTS_ALPHATYPE* alpha) override {
        if (!phbmp || !alpha) return E_POINTER;
        *phbmp = nullptr;
        *alpha = WTSAT_RGB;
        if (!stream_) return E_UNEXPECTED;
        if (cx == 0) cx = 256;
        if (cx > 2560) cx = 2560;
        try {
            std::vector<char> data;
            HRESULT hr = ReadStreamAll(stream_, data, kMaxThumbFileSize);
            if (FAILED(hr)) return hr;
            dxf::Drawing dwg;
            if (!dxf::Load(data.data(), data.size(), dwg) || dwg.empty() || !dwg.extents.valid()) return E_FAIL;
            std::vector<char>().swap(data);

            if (!render::EnsureGdiplus()) return E_FAIL;

            // Keep the drawing's aspect ratio (but not thinner than 1:2).
            int w = (int)cx, h = (int)cx;
            double ew = dwg.extents.width(), eh = dwg.extents.height();
            if (ew > 0 && eh > 0) {
                double aspect = ew / eh;
                if (aspect > 1) h = std::max((int)cx / 2, (int)std::lround(cx / aspect));
                else w = std::max((int)cx / 2, (int)std::lround(cx * aspect));
            }
            w = std::max(w, 1);
            h = std::max(h, 1);

            void* bits = nullptr;
            HBITMAP dib = CreateDib(w, h, &bits);
            if (!dib || !bits) return E_OUTOFMEMORY;
            {
                Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppRGB, static_cast<BYTE*>(bits));
                Gdiplus::Graphics g(&bmp);
                double margin = std::max(2.0, cx * 0.03);
                render::View v = render::FitView(dwg, w, h, margin);
                render::Options o;
                o.light = render::LoadLightSetting();
                o.antialias = true;
                o.minTextPx = 2.0f;
                render::Render(g, dwg, v, o);
                g.Flush(Gdiplus::FlushIntentionSync);
            }
            // Make the alpha channel opaque (GDI+ may leave it untouched in RGB mode).
            uint32_t* px = static_cast<uint32_t*>(bits);
            for (size_t i = 0, n = (size_t)w * h; i < n; ++i) px[i] |= 0xFF000000u;
            *phbmp = dib;
            return S_OK;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return E_FAIL;
        }
    }

private:
    ~DxfThumbnailProvider() {
        if (stream_) stream_->Release();
        --g_dllRef;
    }
    long ref_ = 1;
    IStream* stream_ = nullptr;
};

}  // namespace

HRESULT DxfThumbnailProvider_Create(REFIID riid, void** ppv) {
    auto* p = new (std::nothrow) DxfThumbnailProvider();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
}
