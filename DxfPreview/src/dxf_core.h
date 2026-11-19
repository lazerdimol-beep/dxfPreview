// DXF reader that turns a drawing into flat 2D geometry (polylines, fills, texts)
// ready for rendering. Portable C++17, no platform dependencies.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dxf {

struct Vec2 { double x = 0, y = 0; };
struct Vec3 { double x = 0, y = 0, z = 0; };

struct BBox {
    double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
    bool valid() const { return minx <= maxx && miny <= maxy; }
    void add(double x, double y) {
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    void add(const BBox& b) {
        if (b.valid()) { add(b.minx, b.miny); add(b.maxx, b.maxy); }
    }
    bool intersects(const BBox& b) const {
        return !(b.minx > maxx || b.maxx < minx || b.miny > maxy || b.maxy < miny);
    }
    double width() const { return valid() ? maxx - minx : 0; }
    double height() const { return valid() ? maxy - miny : 0; }
};

// Colours are 0x00RRGGBB. kForeground marks ACI colour 7 ("white/black"),
// which the renderer draws as white on dark background and black on light.
constexpr uint32_t kForeground = 0xFF000000u;

struct Path {          // polyline in world XY (Y up)
    uint32_t color;
    uint32_t first;    // index into Drawing::points
    uint32_t count;
    bool closed;
    BBox box;
};

struct Ring { uint32_t first, count; };

struct Fill {          // filled area; rings combined with the even-odd rule
    uint32_t color;
    uint8_t alpha;     // 255 = opaque
    uint32_t firstRing, ringCount;
    BBox box;
};

struct Text {
    std::vector<std::string> lines;  // UTF-8, one entry per line
    Vec2 pos;                        // anchor point (world)
    double height = 1;               // cap height (world units)
    double rotation = 0;             // radians, counter-clockwise
    double widthFactor = 1;          // horizontal stretch
    double fitLength = 0;            // >0: stretch text to exactly this length (world)
    double baseY = 0;                // first baseline offset from anchor (world, Y up)
    double lineStep = 0;             // distance between baselines (world)
    int hAlign = 0;                  // 0 left, 1 center, 2 right
    bool flipX = false, flipY = false;
    uint32_t color = kForeground;
    BBox box;                        // approximate, for culling/extents
};

struct Drawing {
    std::vector<Vec2> points;
    std::vector<Path> paths;
    std::vector<Ring> rings;
    std::vector<Fill> fills;
    std::vector<Text> texts;
    BBox extents;                    // what "zoom to fit" should show
    bool paperSpace = false;         // true if model space was empty and paper space is shown
    bool truncated = false;          // geometry limit reached
    bool empty() const { return paths.empty() && fills.empty() && texts.empty(); }
    size_t pointCount() const { return points.size(); }
};

// Parses an ASCII DXF file held in memory. Returns false (and fills *error)
// if the data is not a readable DXF.
bool Load(const char* data, size_t size, Drawing& out, std::string* error = nullptr);

// ACI colour index (1..255) to 0xRRGGBB (7 -> kForeground).
uint32_t AciToRgb(int aci);

}  // namespace dxf
