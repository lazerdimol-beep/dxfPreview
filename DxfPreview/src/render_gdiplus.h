// GDI+ renderer for dxf::Drawing (shared by thumbnails, preview pane and dxfthumb.exe).
#pragma once

#include "win_common.h"
#include "dxf_core.h"

namespace render {

struct View {
    double cx = 0, cy = 0;   // world point shown at the centre of the image
    double scale = 1;        // pixels per world unit
    int width = 0, height = 0;
};

struct Options {
    bool light = false;      // white background instead of dark
    bool antialias = true;
    float minTextPx = 1.5f;  // texts smaller than this (cap height in px) are skipped
};

bool EnsureGdiplus();        // thread-safe, starts GDI+ once per process
bool GdiplusStarted();

View FitView(const dxf::Drawing& d, int width, int height, double marginPx);
void Render(Gdiplus::Graphics& g, const dxf::Drawing& d, const View& v, const Options& o);

Gdiplus::Color Background(bool light);
COLORREF BackgroundRef(bool light);

// Per-user setting HKCU\Software\DxfPreview\LightBackground (DWORD).
bool LoadLightSetting();
void SaveLightSetting(bool light);

std::wstring Utf8ToWide(const std::string& s);

}  // namespace render
