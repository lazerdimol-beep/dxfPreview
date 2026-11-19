// Command-line test tool: renders a DXF to PNG with the same code the Explorer
// extension uses. Handy to check a problem file without installing anything.
//   dxfthumb.exe drawing.dxf out.png [size] [--light]
#include "../src/win_common.h"
#include "../src/render_gdiplus.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>

static bool GetEncoderClsid(const wchar_t* mime, CLSID* clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<BYTE> buf(size);
    auto* info = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(info[i].MimeType, mime) == 0) { *clsid = info[i].Clsid; return true; }
    }
    return false;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fwprintf(stderr, L"Usage: dxfthumb.exe input.dxf output.png [size] [--light]\n");
        return 2;
    }
    int size = 1024;
    bool light = false;
    for (int i = 3; i < argc; ++i) {
        if (wcscmp(argv[i], L"--light") == 0) light = true;
        else size = _wtoi(argv[i]);
    }
    if (size < 16) size = 16;
    if (size > 8192) size = 8192;

    std::ifstream f(argv[1], std::ios::binary);
    if (!f) { fwprintf(stderr, L"Cannot open %ls\n", argv[1]); return 1; }
    std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    auto t0 = std::chrono::steady_clock::now();
    dxf::Drawing dwg;
    std::string err;
    if (!dxf::Load(data.data(), data.size(), dwg, &err)) {
        fprintf(stderr, "DXF load failed: %s\n", err.c_str());
        return 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    printf("paths=%zu fills=%zu texts=%zu points=%zu paperSpace=%d truncated=%d\n", dwg.paths.size(), dwg.fills.size(),
           dwg.texts.size(), dwg.points.size(), (int)dwg.paperSpace, (int)dwg.truncated);
    printf("extents: (%g, %g) - (%g, %g)\n", dwg.extents.minx, dwg.extents.miny, dwg.extents.maxx, dwg.extents.maxy);

    if (!render::EnsureGdiplus()) { fprintf(stderr, "GDI+ failed\n"); return 1; }
    int ret = 0;
    {
        Gdiplus::Bitmap bmp(size, size, PixelFormat32bppARGB);
        Gdiplus::Graphics g(&bmp);
        render::View v = render::FitView(dwg, size, size, size * 0.03);
        render::Options o;
        o.light = light;
        render::Render(g, dwg, v, o);
        auto t2 = std::chrono::steady_clock::now();
        CLSID png;
        if (!GetEncoderClsid(L"image/png", &png) || bmp.Save(argv[2], &png, nullptr) != Gdiplus::Ok) {
            fprintf(stderr, "Cannot save PNG\n");
            ret = 1;
        }
        printf("parse %.0f ms, render %.0f ms\n", std::chrono::duration<double, std::milli>(t1 - t0).count(),
               std::chrono::duration<double, std::milli>(t2 - t1).count());
    }
    return ret;
}
