// COM server plumbing: DllMain, class factory, self-registration.
#include "win_common.h"
#include "render_gdiplus.h"

#include <olectl.h>
#include <new>

HINSTANCE g_hInst = nullptr;
std::atomic<long> g_dllRef{0};

namespace {

typedef HRESULT (*CreateFn)(REFIID riid, void** ppv);

class ClassFactory : public IClassFactory {
public:
    explicit ClassFactory(CreateFn fn) : fn_(fn) { ++g_dllRef; }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        long r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return (ULONG)r;
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        return fn_(riid, ppv);
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) ++g_dllRef;
        else --g_dllRef;
        return S_OK;
    }

private:
    ~ClassFactory() { --g_dllRef; }
    long ref_ = 1;
    CreateFn fn_;
};

// ---- registry helpers (HKLM, requires administrator rights)

const wchar_t kThumbHandlerIID[] = L"{e357fccd-a995-4576-b01f-234630154e96}";
const wchar_t kPreviewHandlerIID[] = L"{8895b1c6-b41f-4c1c-a562-0d564250836f}";
const wchar_t kPrevhostAppId[] = L"{6d2b5079-2f0b-48dd-ab7f-97cec514d30b}";  // 64-bit prevhost.exe
const wchar_t kPreviewHandlersKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\PreviewHandlers";

HRESULT SetString(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY h = nullptr;
    LONG r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &h, nullptr);
    if (r != ERROR_SUCCESS) return HRESULT_FROM_WIN32(r);
    r = RegSetValueExW(h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
    return HRESULT_FROM_WIN32(r);
}

std::wstring GetDefaultString(const std::wstring& key) {
    wchar_t buf[256] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) return buf;
    return std::wstring();
}

void DeleteIfOurs(const std::wstring& key, const wchar_t* clsid) {
    if (_wcsicmp(GetDefaultString(key).c_str(), clsid) == 0) RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
}

std::wstring ModulePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(g_hInst, &path[0], (DWORD)path.size());
        if (n == 0) return std::wstring();
        if (n < path.size()) { path.resize(n); return path; }
        path.resize(path.size() * 2);
    }
}

HRESULT RegisterServer(const wchar_t* clsid, const wchar_t* name, const std::wstring& dll, bool preview) {
    std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + clsid;
    HRESULT hr = SetString(key, nullptr, name);
    if (SUCCEEDED(hr) && preview) hr = SetString(key, L"AppID", kPrevhostAppId);
    if (SUCCEEDED(hr) && preview) hr = SetString(key, L"DisplayName", name);
    if (SUCCEEDED(hr)) hr = SetString(key + L"\\InprocServer32", nullptr, dll);
    if (SUCCEEDED(hr)) hr = SetString(key + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    return hr;
}

}  // namespace

HRESULT ReadStreamAll(IStream* stream, std::vector<char>& out, unsigned long long maxBytes) {
    out.clear();
    STATSTG st = {};
    if (SUCCEEDED(stream->Stat(&st, STATFLAG_NONAME))) {
        if (st.cbSize.QuadPart > maxBytes) return E_OUTOFMEMORY;
        out.reserve((size_t)st.cbSize.QuadPart);
    }
    LARGE_INTEGER zero = {};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    std::vector<char> chunk(1 << 20);
    for (;;) {
        ULONG got = 0;
        HRESULT hr = stream->Read(chunk.data(), (ULONG)chunk.size(), &got);
        if (FAILED(hr)) return hr;
        if (got == 0) break;
        out.insert(out.end(), chunk.data(), chunk.data() + got);
        if (out.size() > maxBytes) return E_OUTOFMEMORY;
        if (hr == S_FALSE) break;
    }
    return S_OK;
}

BOOL APIENTRY DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    CreateFn fn = nullptr;
    if (IsEqualCLSID(rclsid, CLSID_DxfThumbnailProvider)) fn = &DxfThumbnailProvider_Create;
    else if (IsEqualCLSID(rclsid, CLSID_DxfPreviewHandler)) fn = &DxfPreviewHandler_Create;
    else return CLASS_E_CLASSNOTAVAILABLE;
    auto* f = new (std::nothrow) ClassFactory(fn);
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    // Once GDI+ has been started in this process keep the DLL loaded.
    return (g_dllRef.load() == 0 && !render::GdiplusStarted()) ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    std::wstring dll = ModulePath();
    if (dll.empty()) return HRESULT_FROM_WIN32(GetLastError());

    HRESULT hr = RegisterServer(DXF_THUMB_CLSID_STR, L"DXF Thumbnail Provider", dll, false);
    if (SUCCEEDED(hr)) hr = RegisterServer(DXF_PREVIEW_CLSID_STR, L"DXF Preview Handler", dll, true);
    if (FAILED(hr)) return hr == E_ACCESSDENIED ? SELFREG_E_CLASS : hr;

    const wchar_t* roots[] = {L"Software\\Classes\\.dxf", L"Software\\Classes\\SystemFileAssociations\\.dxf"};
    for (const wchar_t* root : roots) {
        std::wstring base = std::wstring(root) + L"\\shellex\\";
        hr = SetString(base + kThumbHandlerIID, nullptr, DXF_THUMB_CLSID_STR);
        if (SUCCEEDED(hr)) hr = SetString(base + kPreviewHandlerIID, nullptr, DXF_PREVIEW_CLSID_STR);
        if (FAILED(hr)) return hr;
    }
    hr =SetString(kPreviewHandlersKey, DXF_PREVIEW_CLSID_STR, L"DXF Preview Handler");
    if (FAILED(hr)) return hr;

    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    const wchar_t* roots[] = {L"Software\\Classes\\.dxf", L"Software\\Classes\\SystemFileAssociations\\.dxf"};
    for (const wchar_t* root : roots) {
        std::wstring base = std::wstring(root) + L"\\shellex\\";
        DeleteIfOurs(base + kThumbHandlerIID, DXF_THUMB_CLSID_STR);
        DeleteIfOurs(base + kPreviewHandlerIID, DXF_PREVIEW_CLSID_STR);
    }
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, kPreviewHandlersKey, DXF_PREVIEW_CLSID_STR);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID\\" DXF_THUMB_CLSID_STR);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"Software\\Classes\\CLSID\\" DXF_PREVIEW_CLSID_STR);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
