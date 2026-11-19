#include "render_gdiplus.h"

#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace render {
namespace {

ULONG_PTR g_token = 0;
std::once_flag g_once;
std::atomic<bool> g_started{false};

const wchar_t kSettingsKey[] = L"Software\\DxfPreview";

struct Rgb { BYTE r, g, b; };

Rgb ResolveColor(uint32_t c, bool light) {
    if (c == dxf::kForeground) return light ? Rgb{0, 0, 0} : Rgb{255, 255, 255};
    Rgb v{(BYTE)((c >> 16) & 255), (BYTE)((c >> 8) & 255), (BYTE)(c & 255)};
    if (light && v.r >= 245 && v.g >= 245 && v.b >= 245) return {0, 0, 0};
    if (!light && v.r <= 12 && v.g <= 12 && v.b <= 12) return {255, 255, 255};
    return v;
}

// Liang-Barsky clipping of a segment to a rectangle.
bool ClipSegment(double& x0, double& y0, double& x1, double& y1, double xmin, double ymin, double xmax, double ymax) {
    double t0 = 0, t1 = 1, dx = x1 - x0, dy = y1 - y0;
    double p[4] = {-dx, dx, -dy, dy};
    double q[4] = {x0 - xmin, xmax - x0, y0 - ymin, ymax - y0};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0) {
            if (q[i] < 0) return false;
        } else {
            double t = q[i] / p[i];
            if (p[i] < 0) { if (t > t1) return false; if (t > t0) t0 = t; }
            else { if (t < t0) return false; if (t < t1) t1 = t; }
        }
    }
    double nx0 = x0 + t0 * dx, ny0 = y0 + t0 * dy, nx1 = x0 + t1 * dx, ny1 = y0 + t1 * dy;
    x0 = nx0; y0 = ny0; x1 = nx1; y1 = ny1;
    return true;
}

class Painter {
public:
    Painter(Gdiplus::Graphics& g, const View& v, const Options& o) : g_(g), v_(v), o_(o) {
        double hw = v.width / 2.0 / v.scale, hh = v.height / 2.0 / v.scale;
        double pad = 2.0 / v.scale;
        view_.add(v.cx - hw - pad, v.cy - hh - pad);
        view_.add(v.cx + hw + pad, v.cy + hh + pad);
    }

    double X(double x) const { return (x - v_.cx) * v_.scale + v_.width / 2.0; }
    double Y(double y) const { return (v_.cy - y) * v_.scale + v_.height / 2.0; }
    const dxf::BBox& ViewBox() const { return view_; }

    Gdiplus::Pen* PenFor(uint32_t c) {
        Rgb rgb = ResolveColor(c, o_.light);
        uint32_t key = (rgb.r << 16) | (rgb.g << 8) | rgb.b;
        auto it = pens_.find(key);
        if (it != pens_.end()) return it->second.get();
        auto pen = std::make_unique<Gdiplus::Pen>(Gdiplus::Color(255, rgb.r, rgb.g, rgb.b), 1.0f);
        Gdiplus::Pen* raw = pen.get();
        pens_[key] = std::move(pen);
        return raw;
    }

    Gdiplus::SolidBrush* BrushFor(uint32_t c, BYTE alpha = 255) {
        Rgb rgb = ResolveColor(c, o_.light);
        uint32_t key = ((uint32_t)alpha << 24) | (rgb.r << 16) | (rgb.g << 8) | rgb.b;
        auto it = brushes_.find(key);
        if (it != brushes_.end()) return it->second.get();
        auto br = std::make_unique<Gdiplus::SolidBrush>(Gdiplus::Color(alpha, rgb.r, rgb.g, rgb.b));
        Gdiplus::SolidBrush* raw = br.get();
        brushes_[key] = std::move(br);
        return raw;
    }

    void DrawPaths(const dxf::Drawing& d) {
        const double limit = 1e6;
        for (const dxf::Path& p : d.paths) {
            if (!p.box.intersects(view_)) continue;
            double bw = (p.box.maxx - p.box.minx) * v_.scale, bh = (p.box.maxy - p.box.miny) * v_.scale;
            if (bw < 1.0 && bh < 1.0) {
                g_.FillRectangle(BrushFor(p.color), (Gdiplus::REAL)X(p.box.minx), (Gdiplus::REAL)Y(p.box.maxy), 1.0f, 1.0f);
                continue;
            }
            Gdiplus::Pen* pen = PenFor(p.color);
            const dxf::Vec2* src = &d.points[p.first];
            bool huge = std::fabs(X(p.box.minx)) > limit || std::fabs(X(p.box.maxx)) > limit ||
                        std::fabs(Y(p.box.miny)) > limit || std::fabs(Y(p.box.maxy)) > limit;
            if (huge) {
                uint32_t n = p.count, segs = p.closed ? n : n - 1;
                for (uint32_t i = 0; i < segs; ++i) {
                    const dxf::Vec2& a = src[i];
                    const dxf::Vec2& b = src[(i + 1) % n];
                    double x0 = X(a.x), y0 = Y(a.y), x1 = X(b.x), y1 = Y(b.y);
                    if (ClipSegment(x0, y0, x1, y1, -10, -10, v_.width + 10.0, v_.height + 10.0))
                        g_.DrawLine(pen, (Gdiplus::REAL)x0, (Gdiplus::REAL)y0, (Gdiplus::REAL)x1, (Gdiplus::REAL)y1);
                }
                continue;
            }
            Convert(src, p.count);
            if (pts_.size() < 2) continue;
            if (p.closed && pts_.size() >= 3) g_.DrawPolygon(pen, pts_.data(), (INT)pts_.size());
            else g_.DrawLines(pen, pts_.data(), (INT)pts_.size());
        }
    }

    void DrawFills(const dxf::Drawing& d) {
        for (const dxf::Fill& f : d.fills) {
            if (!f.box.intersects(view_)) continue;
            double bw = (f.box.maxx - f.box.minx) * v_.scale, bh = (f.box.maxy - f.box.miny) * v_.scale;
            if (bw < 1.0 && bh < 1.0) {
                g_.FillRectangle(BrushFor(f.color, f.alpha), (Gdiplus::REAL)X(f.box.minx), (Gdiplus::REAL)Y(f.box.maxy), 1.0f, 1.0f);
                continue;
            }
            Gdiplus::GraphicsPath gp(Gdiplus::FillModeAlternate);
            for (uint32_t r = 0; r < f.ringCount; ++r) {
                const dxf::Ring& ring = d.rings[f.firstRing + r];
                Convert(&d.points[ring.first], ring.count, true);
                if (pts_.size() >= 3) gp.AddPolygon(pts_.data(), (INT)pts_.size());
            }
            g_.FillPath(BrushFor(f.color, f.alpha), &gp);
        }
    }

    void DrawTexts(const dxf::Drawing& d) {
        if (d.texts.empty()) return;
        Gdiplus::FontFamily arial(L"Arial");
        std::unique_ptr<Gdiplus::FontFamily> fallback;
        const Gdiplus::FontFamily* fam = &arial;
        if (arial.GetLastStatus() != Gdiplus::Ok) {
            fallback.reset(Gdiplus::FontFamily::GenericSansSerif()->Clone());
            fam = fallback.get();
        }
        const Gdiplus::REAL kEm = 100.0f;
        Gdiplus::Font font(fam, kEm, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        if (font.GetLastStatus() != Gdiplus::Ok) return;
        UINT16 emH = fam->GetEmHeight(Gdiplus::FontStyleRegular);
        Gdiplus::REAL ascent = emH ? kEm * fam->GetCellAscent(Gdiplus::FontStyleRegular) / emH : kEm * 0.9f;
        const Gdiplus::REAL capH = kEm * 0.716f;  // Arial cap height
        Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
        fmt.SetFormatFlags(fmt.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces | Gdiplus::StringFormatFlagsNoWrap);
        Gdiplus::Matrix saved;
        g_.GetTransform(&saved);
        std::vector<std::wstring> wl;
        std::vector<Gdiplus::REAL> widths;
        for (const dxf::Text& t : d.texts) {
            if (!t.box.intersects(view_) || t.height <= 0) continue;
            double hpx = t.height * v_.scale;
            if (hpx < o_.minTextPx || hpx > 50000) continue;
            wl.clear();
            widths.clear();
            Gdiplus::REAL maxW = 0;
            for (const std::string& s : t.lines) {
                wl.push_back(Utf8ToWide(s));
                Gdiplus::RectF box;
                Gdiplus::REAL w = 0;
                if (!wl.back().empty() &&
                    g_.MeasureString(wl.back().c_str(), (INT)wl.back().size(), &font, Gdiplus::PointF(0, 0), &fmt, &box) == Gdiplus::Ok)
                    w = box.Width;
                widths.push_back(w);
                maxW = std::max(maxW, w);
            }
            double k = hpx / capH;
            double sx = k * t.widthFactor, sy = k;
            if (t.fitLength > 0 && maxW > 0) sx = t.fitLength * v_.scale / maxW;
            if (t.flipX) sx = -sx;
            if (t.flipY) sy = -sy;
            Gdiplus::Matrix m;
            m.Translate((Gdiplus::REAL)X(t.pos.x), (Gdiplus::REAL)Y(t.pos.y));
            m.Rotate((Gdiplus::REAL)(-t.rotation * 180.0 / 3.14159265358979323846));
            m.Scale((Gdiplus::REAL)sx, (Gdiplus::REAL)sy);
            g_.SetTransform(&m);
            Gdiplus::SolidBrush* br = BrushFor(t.color);
            double base0 = -(t.baseY / t.height) * capH;
            double step = (t.lineStep / t.height) * capH;
            for (size_t i = 0; i < wl.size(); ++i) {
                if (wl[i].empty()) continue;
                Gdiplus::REAL w = widths[i];
                Gdiplus::REAL x = t.hAlign == 0 ? 0 : t.hAlign == 1 ? -w / 2 : -w;
                Gdiplus::REAL yb = (Gdiplus::REAL)(base0 + step * (double)i);
                g_.DrawString(wl[i].c_str(), (INT)wl[i].size(), &font, Gdiplus::PointF(x, yb - ascent), &fmt, br);
            }
        }
        g_.SetTransform(&saved);
    }

private:
    // Converts world points to pixels, dropping points closer than ~0.6 px to the previous one.
    void Convert(const dxf::Vec2* src, uint32_t n, bool clamp = false) {
        pts_.clear();
        float lx = 0, ly = 0;
        for (uint32_t i = 0; i < n; ++i) {
            double x = X(src[i].x), y = Y(src[i].y);
            if (clamp) {
                x = std::max(-4e5, std::min(4e5, x));
                y = std::max(-4e5, std::min(4e5, y));
            }
            float fx = (float)x, fy = (float)y;
            if (i == 0 || i + 1 == n || std::fabs(fx - lx) + std::fabs(fy - ly) >= 0.6f) {
                pts_.emplace_back(fx, fy);
                lx = fx;
                ly = fy;
            }
        }
    }

    Gdiplus::Graphics& g_;
    View v_;
    Options o_;
    dxf::BBox view_;
    std::vector<Gdiplus::PointF> pts_;
    std::unordered_map<uint32_t, std::unique_ptr<Gdiplus::Pen>> pens_;
    std::unordered_map<uint32_t, std::unique_ptr<Gdiplus::SolidBrush>> brushes_;
};

}  // namespace

bool EnsureGdiplus() {
    std::call_once(g_once, [] {
        Gdiplus::GdiplusStartupInput in;
        if (Gdiplus::GdiplusStartup(&g_token, &in, nullptr) == Gdiplus::Ok) g_started = true;
    });
    return g_started;
}

bool GdiplusStarted() { return g_started; }

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

View FitView(const dxf::Drawing& d, int width, int height, double marginPx) {
    View v;
    v.width = width;
    v.height = height;
    const dxf::BBox& e = d.extents;
    if (!e.valid()) return v;
    v.cx = (e.minx + e.maxx) / 2;
    v.cy = (e.miny + e.maxy) / 2;
    double aw = std::max(1.0, width - 2 * marginPx), ah = std::max(1.0, height - 2 * marginPx);
    double ew = e.maxx - e.minx, eh = e.maxy - e.miny;
    if (ew <= 0 && eh <= 0) { v.scale = 1; return v; }
    double sx = ew > 0 ? aw / ew : 1e300, sy = eh > 0 ? ah / eh : 1e300;
    v.scale = std::min(sx, sy);
    return v;
}

Gdiplus::Color Background(bool light) {
    return light ? Gdiplus::Color(255, 255, 255, 255) : Gdiplus::Color(255, 33, 40, 48);
}

COLORREF BackgroundRef(bool light) { return light ? RGB(255, 255, 255) : RGB(33, 40, 48); }

void Render(Gdiplus::Graphics& g, const dxf::Drawing& d, const View& v, const Options& o) {
    g.Clear(Background(o.light));
    if (v.width <= 0 || v.height <= 0 || !(v.scale > 0)) return;
    g.SetSmoothingMode(o.antialias ? Gdiplus::SmoothingModeAntiAlias : Gdiplus::SmoothingModeNone);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Painter p(g, v, o);
    p.DrawFills(d);
    p.DrawPaths(d);
    p.DrawTexts(d);
}

bool LoadLightSetting() {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, L"LightBackground", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
        return value != 0;
    return false;
}

void SaveLightSetting(bool light) {
    DWORD value = light ? 1 : 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, L"LightBackground", REG_DWORD, &value, sizeof(value));
}

}  // namespace render
