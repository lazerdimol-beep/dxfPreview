// Explorer preview pane handler for .dxf files (IPreviewHandler) with zoom and pan.
#include "win_common.h"
#include "render_gdiplus.h"

#include <windowsx.h>
#include <cmath>
#include <memory>
#include <new>

namespace {

const wchar_t kViewerClass[] = L"DxfPreviewViewerWindow";
const unsigned long long kMaxPreviewFileSize = 1024ull * 1024 * 1024;
const UINT_PTR kQualityTimer = 1;

class Viewer {
public:
    dxf::Drawing dwg;
    bool loaded = false;
    std::wstring error;
    bool light = false;
    IPreviewHandlerFrame* frame = nullptr;  // not owned
    HWND hwnd = nullptr;

    ~Viewer() {
        Destroy();
        if (buf_) DeleteObject(buf_);
    }

    bool CreateWnd(HWND parent, const RECT& rc) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        if (!GetClassInfoExW(g_hInst, kViewerClass, &wc)) {
            wc = {};
            wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
            wc.lpfnWndProc = &Viewer::WndProc;
            wc.hInstance = g_hInst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kViewerClass;
            if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        }
        heavy_ = dwg.points.size() > 400000 || dwg.texts.size() > 4000;
        hq_ = true;
        HWND h = CreateWindowExW(0, kViewerClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                 rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, parent, nullptr, g_hInst, this);
        return h != nullptr;
    }

    void Destroy() {
        if (hwnd) {
            HWND h = hwnd;
            DestroyWindow(h);
            hwnd = nullptr;
        }
    }

    void Move(const RECT& rc) {
        if (hwnd)
            SetWindowPos(hwnd, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Viewer* v = reinterpret_cast<Viewer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            v = static_cast<Viewer*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(v));
            if (v) v->hwnd = hwnd;
        }
        if (msg == WM_NCDESTROY) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            if (v) v->hwnd = nullptr;
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        if (v) {
            LRESULT r = 0;
            if (v->Handle(msg, wp, lp, r)) return r;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    bool Handle(UINT msg, WPARAM wp, LPARAM lp, LRESULT& r) {
        switch (msg) {
            case WM_ERASEBKGND:
                r = 1;
                return true;
            case WM_PAINT:
                Paint();
                return true;
            case WM_SIZE:
                dirty_ = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            case WM_MOUSEWHEEL: {
                POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(hwnd, &pt);
                int delta = GET_WHEEL_DELTA_WPARAM(wp);
                Zoom(std::pow(1.2, delta / 120.0), pt.x, pt.y);
                return true;
            }
            case WM_LBUTTONDOWN:
            case WM_MBUTTONDOWN:
                ::SetFocus(hwnd);
                SetCapture(hwnd);
                dragging_ = true;
                dragStart_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                dragOff_ = {0, 0};
                return true;
            case WM_MOUSEMOVE:
                if (dragging_) {
                    dragOff_ = {GET_X_LPARAM(lp) - dragStart_.x, GET_Y_LPARAM(lp) - dragStart_.y};
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return true;
            case WM_LBUTTONUP:
            case WM_MBUTTONUP:
                EndDrag();
                if (GetCapture() == hwnd) ReleaseCapture();
                return true;
            case WM_CAPTURECHANGED:
                EndDrag();
                return true;
            case WM_LBUTTONDBLCLK:
                FitToWindow();
                return true;
            case WM_RBUTTONUP:
                FitToWindow();
                return true;
            case WM_TIMER:
                if (wp == kQualityTimer) {
                    KillTimer(hwnd, kQualityTimer);
                    if (!hq_) {
                        hq_ = true;
                        dirty_ = true;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    return true;
                }
                return false;
            case WM_KEYDOWN:
                if (OnKey(wp)) return true;
                ForwardToFrame(msg, wp, lp);
                return true;
            case WM_SYSKEYDOWN:
                if (ForwardToFrame(msg, wp, lp)) return true;
                return false;
            case WM_GETDLGCODE:
                r = DLGC_WANTARROWS | DLGC_WANTCHARS;
                return true;
        }
        return false;
    }

    bool ForwardToFrame(UINT msg, WPARAM wp, LPARAM lp) {
        if (!frame) return false;
        MSG m = {};
        m.hwnd = hwnd;
        m.message = msg;
        m.wParam = wp;
        m.lParam = lp;
        m.time = GetMessageTime();
        DWORD pos = GetMessagePos();
        m.pt = {GET_X_LPARAM(pos), GET_Y_LPARAM(pos)};
        return frame->TranslateAccelerator(&m) == S_OK;
    }

    bool OnKey(WPARAM key) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        int cx = rc.right / 2, cy = rc.bottom / 2;
        switch (key) {
            case 'B':
                light = !light;
                render::SaveLightSetting(light);
                dirty_ = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                return true;
            case VK_HOME:
            case '0':
            case VK_NUMPAD0:
                FitToWindow();
                return true;
            case VK_ADD:
            case VK_OEM_PLUS:
                Zoom(1.25, cx, cy);
                return true;
            case VK_SUBTRACT:
            case VK_OEM_MINUS:
                Zoom(0.8, cx, cy);
                return true;
            case VK_LEFT: Pan(rc.right * 0.1, 0); return true;
            case VK_RIGHT: Pan(-rc.right * 0.1, 0); return true;
            case VK_UP: Pan(0, rc.bottom * 0.1); return true;
            case VK_DOWN: Pan(0, -rc.bottom * 0.1); return true;
        }
        return false;
    }

    void Interact() {
        userView_ = true;
        dirty_ = true;
        if (heavy_) {
            hq_ = false;
            SetTimer(hwnd, kQualityTimer, 350, nullptr);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void Zoom(double factor, int px, int py) {
        if (!loaded || view_.scale <= 0) return;
        double newScale = view_.scale * factor;
        if (fitScale_ > 0) newScale = std::max(fitScale_ * 1e-2, std::min(fitScale_ * 1e7, newScale));
        double wx = view_.cx + (px - view_.width / 2.0) / view_.scale;
        double wy = view_.cy - (py - view_.height / 2.0) / view_.scale;
        view_.scale = newScale;
        view_.cx = wx - (px - view_.width / 2.0) / view_.scale;
        view_.cy = wy + (py - view_.height / 2.0) / view_.scale;
        Interact();
    }

    void Pan(double dx, double dy) {
        if (!loaded || view_.scale <= 0) return;
        view_.cx -= dx / view_.scale;
        view_.cy += dy / view_.scale;
        Interact();
    }

    void EndDrag() {
        if (!dragging_) return;
        dragging_ = false;
        POINT off = dragOff_;
        dragOff_ = {0, 0};
        if (off.x != 0 || off.y != 0) Pan(off.x, off.y);
        else InvalidateRect(hwnd, nullptr, FALSE);
    }

    void FitToWindow() {
        userView_ = false;
        dirty_ = true;
        hq_ = true;
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void EnsureBuffer(int w, int h) {
        if (buf_ && w == bw_ && h == bh_) return;
        if (buf_) DeleteObject(buf_);
        buf_ = nullptr;
        bits_ = nullptr;
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        buf_ = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        bw_ = w;
        bh_ = h;
        dirty_ = true;
    }

    void Paint() {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right, h = rc.bottom;
        if (w > 0 && h > 0) {
            EnsureBuffer(w, h);
            if (loaded && (!userView_ || view_.width != w || view_.height != h)) {
                if (!userView_) {
                    double margin = std::max(8.0, std::min(w, h) * 0.03);
                    view_ = render::FitView(dwg, w, h, margin);
                    fitScale_ = view_.scale;
                } else {
                    view_.width = w;
                    view_.height = h;
                }
                dirty_ = true;
            }
            if (dirty_ && buf_ && bits_) {
                RenderBuffer();
                dirty_ = false;
            }
            if (buf_) {
                HDC mem = CreateCompatibleDC(hdc);
                HGDIOBJ old = SelectObject(mem, buf_);
                int ox = dragOff_.x, oy = dragOff_.y;
                if (ox || oy) {
                    HBRUSH br = CreateSolidBrush(render::BackgroundRef(light));
                    RECT s;
                    if (ox > 0) { s = {0, 0, ox, h}; FillRect(hdc, &s, br); }
                    if (ox < 0) { s = {w + ox, 0, w, h}; FillRect(hdc, &s, br); }
                    if (oy > 0) { s = {0, 0, w, oy}; FillRect(hdc, &s, br); }
                    if (oy < 0) { s = {0, h + oy, w, h}; FillRect(hdc, &s, br); }
                    DeleteObject(br);
                }
                BitBlt(hdc, ox, oy, w, h, mem, 0, 0, SRCCOPY);
                SelectObject(mem, old);
                DeleteDC(mem);
            }
        }
        EndPaint(hwnd, &ps);
    }

    void RenderBuffer() {
        Gdiplus::Bitmap bmp(bw_, bh_, bw_ * 4, PixelFormat32bppRGB, static_cast<BYTE*>(bits_));
        Gdiplus::Graphics g(&bmp);
        if (loaded) {
            render::Options o;
            o.light = light;
            o.antialias = hq_ || !heavy_;
            o.minTextPx = 1.5f;
            render::Render(g, dwg, view_, o);
            DrawHint(g);
        } else {
            g.Clear(render::Background(light));
            DrawMessage(g);
        }
        g.Flush(Gdiplus::FlushIntentionSync);
    }

    void DrawHint(Gdiplus::Graphics& g) {
        if (bw_ < 260 || bh_ < 120) return;
        std::wstring s;
        if (dwg.paperSpace) s += L"Аркуш (Paper space)  ·  ";
        if (dwg.truncated) s += L"Показано частково (дуже великий файл)  ·  ";
        s += L"Колесо: масштаб  ·  Перетягування: зсув  ·  Подвійний клік: усе  ·  B: фон";
        Gdiplus::Font font(L"Segoe UI", 11.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush br(light ? Gdiplus::Color(170, 110, 110, 110) : Gdiplus::Color(170, 150, 160, 170));
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        g.DrawString(s.c_str(), (INT)s.size(), &font, Gdiplus::PointF(6.0f, (Gdiplus::REAL)bh_ - 18.0f), &br);
    }

    void DrawMessage(Gdiplus::Graphics& g) {
        std::wstring s = L"Не вдалося показати DXF";
        if (!error.empty()) s += L"\n" + error;
        Gdiplus::Font font(L"Segoe UI", 14.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::SolidBrush br(light ? Gdiplus::Color(255, 80, 80, 80) : Gdiplus::Color(255, 200, 200, 200));
        Gdiplus::StringFormat fmt;
        fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::RectF rc(0, 0, (Gdiplus::REAL)bw_, (Gdiplus::REAL)bh_);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        g.DrawString(s.c_str(), (INT)s.size(), &font, rc, &fmt, &br);
    }

    HBITMAP buf_ = nullptr;
    void* bits_ = nullptr;
    int bw_ = 0, bh_ = 0;
    bool dirty_ = true, hq_ = true, heavy_ = false, userView_ = false, dragging_ = false;
    POINT dragStart_ = {0, 0}, dragOff_ = {0, 0};
    render::View view_;
    double fitScale_ = 0;
};

std::wstring TranslateError(const std::string& e) {
    if (e == "binary DXF is not supported") return L"Двійковий (binary) DXF поки не підтримується";
    if (e == "empty file") return L"Файл порожній";
    if (e == "out of memory") return L"Недостатньо пам'яті";
    if (e.empty()) return L"";
    return L"Файл пошкоджений або це не DXF (" + render::Utf8ToWide(e) + L")";
}

class DxfPreviewHandler : public IObjectWithSite, public IPreviewHandler, public IOleWindow, public IInitializeWithStream {
public:
    DxfPreviewHandler() { ++g_dllRef; }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == __uuidof(IPreviewHandler)) *ppv = static_cast<IPreviewHandler*>(this);
        else if (riid == __uuidof(IObjectWithSite)) *ppv = static_cast<IObjectWithSite*>(this);
        else if (riid == __uuidof(IOleWindow)) *ppv = static_cast<IOleWindow*>(this);
        else if (riid == __uuidof(IInitializeWithStream)) *ppv = static_cast<IInitializeWithStream*>(this);
        else return E_NOINTERFACE;
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

    // IObjectWithSite
    IFACEMETHODIMP SetSite(IUnknown* site) override {
        if (frame_) { frame_->Release(); frame_ = nullptr; }
        if (site_) { site_->Release(); site_ = nullptr; }
        if (site) {
            site_ = site;
            site_->AddRef();
            site->QueryInterface(IID_PPV_ARGS(&frame_));
        }
        if (viewer_) viewer_->frame = frame_;
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        return site_ ? site_->QueryInterface(riid, ppv) : E_FAIL;
    }

    // IOleWindow
    IFACEMETHODIMP GetWindow(HWND* phwnd) override {
        if (!phwnd) return E_INVALIDARG;
        *phwnd = parent_;
        return S_OK;
    }
    IFACEMETHODIMP ContextSensitiveHelp(BOOL) override { return E_NOTIMPL; }

    // IPreviewHandler
    IFACEMETHODIMP SetWindow(HWND hwnd, const RECT* prc) override {
        if (!hwnd || !prc) return E_INVALIDARG;
        parent_ = hwnd;
        rect_ = *prc;
        if (viewer_ && viewer_->hwnd) {
            ::SetParent(viewer_->hwnd, parent_);
            viewer_->Move(rect_);
        }
        return S_OK;
    }
    IFACEMETHODIMP SetRect(const RECT* prc) override {
        if (!prc) return E_INVALIDARG;
        rect_ = *prc;
        if (viewer_) viewer_->Move(rect_);
        return S_OK;
    }
    IFACEMETHODIMP DoPreview() override {
        if (viewer_) return S_OK;
        if (!parent_ || !stream_) return E_FAIL;
        try {
            render::EnsureGdiplus();
            auto v = std::make_unique<Viewer>();
            v->light = render::LoadLightSetting();
            v->frame = frame_;
            std::vector<char> data;
            HRESULT hr = ReadStreamAll(stream_, data, kMaxPreviewFileSize);
            if (SUCCEEDED(hr)) {
                std::string err;
                v->loaded = dxf::Load(data.data(), data.size(), v->dwg, &err);
                if (!v->loaded) v->error = TranslateError(err);
                else if (v->dwg.empty() || !v->dwg.extents.valid()) {
                    v->loaded = false;
                    v->error = L"Креслення не містить видимих об'єктів";
                }
            } else {
                v->error = hr == E_OUTOFMEMORY ? L"Файл завеликий" : L"Не вдалося прочитати файл";
            }
            if (!v->CreateWnd(parent_, rect_)) return HRESULT_FROM_WIN32(GetLastError());
            viewer_ = std::move(v);
            return S_OK;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        } catch (...) {
            return E_FAIL;
        }
    }
    IFACEMETHODIMP Unload() override {
        viewer_.reset();
        if (stream_) { stream_->Release(); stream_ = nullptr; }
        return S_OK;
    }
    IFACEMETHODIMP SetFocus() override {
        if (viewer_ && viewer_->hwnd) ::SetFocus(viewer_->hwnd);
        return S_OK;
    }
    IFACEMETHODIMP QueryFocus(HWND* phwnd) override {
        if (!phwnd) return E_INVALIDARG;
        *phwnd = ::GetFocus();
        return *phwnd ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }
    IFACEMETHODIMP TranslateAccelerator(MSG* pmsg) override {
        if (frame_) return frame_->TranslateAccelerator(pmsg);
        return S_FALSE;
    }

private:
    ~DxfPreviewHandler() {
        viewer_.reset();
        if (stream_) stream_->Release();
        if (frame_) frame_->Release();
        if (site_) site_->Release();
        --g_dllRef;
    }

    long ref_ = 1;
    IStream* stream_ = nullptr;
    IUnknown* site_ = nullptr;
    IPreviewHandlerFrame* frame_ = nullptr;
    HWND parent_ = nullptr;
    RECT rect_ = {0, 0, 0, 0};
    std::unique_ptr<Viewer> viewer_;
};

}  // namespace

HRESULT DxfPreviewHandler_Create(REFIID riid, void** ppv) {
    auto* p = new (std::nothrow) DxfPreviewHandler();
    if (!p) return E_OUTOFMEMORY;
    HRESULT hr = p->QueryInterface(riid, ppv);
    p->Release();
    return hr;
}
