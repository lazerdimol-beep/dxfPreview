#include "dxf_core.h"
#include "codepages.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace dxf {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr size_t kMaxPoints = 25000000;   // safety cap for huge drawings
constexpr int kMaxDepth = 24;             // nested block limit
constexpr int kCircleSegs = 96;           // segments per full circle
constexpr size_t kMaxEmits = 30000000;    // entity expansions (nested blocks) cap

// ---------------------------------------------------------------- helpers

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

double ToD(std::string_view s) {
    s = Trim(s);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    double v = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || !std::isfinite(v)) return 0;
    return v;
}

int ToI(std::string_view s) {
    s = Trim(s);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    long long v = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return (int)ToD(s);
    if (v > 2147483647LL) v = 2147483647LL;
    if (v < -2147483647LL) v = -2147483647LL;
    return (int)v;
}

std::string Upper(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return r;
}

double Norm360(double a) {
    a = std::fmod(a, 360.0);
    if (a < 0) a += 360.0;
    return a;
}

Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Mul(Vec3 a, double k) { return {a.x * k, a.y * k, a.z * k}; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double Len(Vec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
Vec3 Normalize(Vec3 a) {
    double l = Len(a);
    return l > 1e-300 ? Mul(a, 1.0 / l) : Vec3{0, 0, 1};
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    }
}

// True if s is valid UTF-8 and contains at least one multi-byte sequence.
bool LooksUtf8(std::string_view s) {
    bool multi = false;
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { ++i; continue; }
        int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (len == 0 || i + len > n) return false;
        if (c == 0xC0 || c == 0xC1) return false;
        for (int k = 1; k < len; ++k)
            if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
        multi = true;
        i += len;
    }
    return multi;
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// ---------------------------------------------------------------- matrices

struct Mat {  // affine 3x4: p' = R*p + t
    double m[3][4];
    static Mat Identity() {
        Mat r{};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = 1;
        return r;
    }
    Vec3 Apply(Vec3 p) const {
        return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z + m[0][3],
                m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z + m[1][3],
                m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z + m[2][3]};
    }
    Vec3 ApplyVec(Vec3 p) const {
        return {m[0][0] * p.x + m[0][1] * p.y + m[0][2] * p.z,
                m[1][0] * p.x + m[1][1] * p.y + m[1][2] * p.z,
                m[2][0] * p.x + m[2][1] * p.y + m[2][2] * p.z};
    }
    Mat operator*(const Mat& o) const {
        Mat r{};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 4; ++j) {
                double v = m[i][0] * o.m[0][j] + m[i][1] * o.m[1][j] + m[i][2] * o.m[2][j];
                if (j == 3) v += m[i][3];
                r.m[i][j] = v;
            }
        }
        return r;
    }
    static Mat Translate(double x, double y, double z) {
        Mat r = Identity();
        r.m[0][3] = x; r.m[1][3] = y; r.m[2][3] = z;
        return r;
    }
    static Mat Scale(double x, double y, double z) {
        Mat r{};
        r.m[0][0] = x; r.m[1][1] = y; r.m[2][2] = z;
        return r;
    }
    static Mat RotZ(double a) {
        Mat r = Identity();
        double c = std::cos(a), s = std::sin(a);
        r.m[0][0] = c; r.m[0][1] = -s;
        r.m[1][0] = s; r.m[1][1] = c;
        return r;
    }
};

// Object Coordinate System -> WCS (AutoCAD "arbitrary axis algorithm").
Mat OcsMatrix(Vec3 n) {
    n = Normalize(n);
    if (std::fabs(n.x) < 1e-12 && std::fabs(n.y) < 1e-12 && n.z > 0) return Mat::Identity();
    Vec3 ax;
    if (std::fabs(n.x) < 1.0 / 64 && std::fabs(n.y) < 1.0 / 64) ax = Normalize(Cross({0, 1, 0}, n));
    else ax = Normalize(Cross({0, 0, 1}, n));
    Vec3 ay = Normalize(Cross(n, ax));
    Mat r{};
    r.m[0][0] = ax.x; r.m[0][1] = ay.x; r.m[0][2] = n.x;
    r.m[1][0] = ax.y; r.m[1][1] = ay.y; r.m[1][2] = n.y;
    r.m[2][0] = ax.z; r.m[2][1] = ay.z; r.m[2][2] = n.z;
    return r;
}

// ---------------------------------------------------------------- reader

struct Pair {
    int code = 0;
    std::string_view value;
};

class Reader {
public:
    Reader(const char* d, size_t n) : p_(d), end_(d + n) {
        if (n >= 3 && (unsigned char)d[0] == 0xEF && (unsigned char)d[1] == 0xBB && (unsigned char)d[2] == 0xBF) p_ += 3;
    }
    bool Next(Pair& out) {
        if (hasPushed_) { out = pushed_; hasPushed_ = false; return true; }
        std::string_view codeLine, valueLine;
        do {
            if (!ReadLine(codeLine)) return false;
        } while (Trim(codeLine).empty());
        if (!ReadLine(valueLine)) return false;
        std::string_view t = Trim(codeLine);
        int code = 0;
        auto r = std::from_chars(t.data(), t.data() + t.size(), code);
        if (r.ec != std::errc() || r.ptr != t.data() + t.size()) { bad_ = true; return false; }
        out.code = code;
        out.value = valueLine;
        ++count_;
        return true;
    }
    void PushBack(const Pair& p) { pushed_ = p; hasPushed_ = true; }
    bool Bad() const { return bad_; }
    size_t Count() const { return count_; }

private:
    bool ReadLine(std::string_view& line) {
        if (p_ >= end_) return false;
        const char* s = p_;
        const char* e = (const char*)std::memchr(s, '\n', (size_t)(end_ - s));
        if (!e) { line = std::string_view(s, (size_t)(end_ - s)); p_ = end_; }
        else { line = std::string_view(s, (size_t)(e - s)); p_ = e + 1; }
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        return true;
    }
    const char* p_;
    const char* end_;
    Pair pushed_;
    bool hasPushed_ = false;
    bool bad_ = false;
    size_t count_ = 0;
};

// ---------------------------------------------------------------- entities

enum class ET {
    Unknown, Line, Circle, Arc, LwPolyline, Polyline, Ellipse, Spline, Text, Attrib,
    MText, Insert, Dimension, Solid, Face3D, Hatch, Leader, MLine
};

ET TypeOf(std::string_view t) {
    t = Trim(t);
    if (t == "LINE") return ET::Line;
    if (t == "CIRCLE") return ET::Circle;
    if (t == "ARC") return ET::Arc;
    if (t == "LWPOLYLINE") return ET::LwPolyline;
    if (t == "POLYLINE") return ET::Polyline;
    if (t == "ELLIPSE") return ET::Ellipse;
    if (t == "SPLINE") return ET::Spline;
    if (t == "TEXT") return ET::Text;
    if (t == "ATTRIB") return ET::Attrib;
    if (t == "MTEXT") return ET::MText;
    if (t == "INSERT" || t == "ACAD_TABLE") return ET::Insert;
    if (t == "DIMENSION" || t == "ARC_DIMENSION" || t == "LARGE_RADIAL_DIMENSION") return ET::Dimension;
    if (t == "SOLID" || t == "TRACE") return ET::Solid;
    if (t == "3DFACE") return ET::Face3D;
    if (t == "HATCH") return ET::Hatch;
    if (t == "LEADER") return ET::Leader;
    if (t == "MLINE") return ET::MLine;
    return ET::Unknown;
}

struct Vertex {
    Vec3 p;
    double bulge = 0;
    int flags = 0;
    int idx[4] = {0, 0, 0, 0};
};

struct PatternLine {
    double angle = 0;  // degrees
    Vec2 base, offset;
    std::vector<double> dashes;
};

struct Entity {
    ET type = ET::Unknown;
    std::string layer = "0";
    int aci = 256;
    int32_t trueColor = -1;
    bool paper = false, invisible = false;
    Vec3 extr{0, 0, 1};
    Vec3 p[4];
    bool has11 = false, has13 = false;
    double r = 0, a0 = 0, a1 = 0;
    double h = 0, rot = 0, wf = 1, ratio = 1;
    double sx = 1, sy = 1, sz = 1, colSp = 0, rowSp = 0, elevation = 0;
    double lineSpacing = 1;
    int flags = 0, hj = 0, vj = 0, attach = 1, cols = 1, rows = 1, degree = 3, meshM = 0, meshN = 0;
    bool hasAttribs = false, solidFill = false;
    std::string text, name;
    std::vector<Vertex> verts;
    std::vector<double> knots, weights;
    std::vector<Vec3> ctrl, fit;
    std::vector<std::vector<Vec2>> rings;  // hatch boundaries, OCS
    std::vector<PatternLine> pattern;
    std::vector<Entity> attribs;
};

struct Block {
    Vec3 base;
    std::vector<Entity> entities;
};

struct Layer {
    uint32_t color = kForeground;
    bool visible = true;
};

// ---------------------------------------------------------------- geometry helpers

// Appends the points of a bulged segment a->b (excluding a, including b).
void AddBulge(Vec2 a, Vec2 b, double bulge, std::vector<Vec2>& out) {
    double dx = b.x - a.x, dy = b.y - a.y;
    double chord = std::hypot(dx, dy);
    if (std::fabs(bulge) < 1e-9 || chord < 1e-12) { out.push_back(b); return; }
    double theta = 4.0 * std::atan(bulge);
    double h = (chord / 2) / std::tan(theta / 2);
    double cx = (a.x + b.x) / 2 - dy / chord * h;
    double cy = (a.y + b.y) / 2 + dx / chord * h;
    double r = std::hypot(a.x - cx, a.y - cy);
    double sa = std::atan2(a.y - cy, a.x - cx);
    int n = std::max(2, (int)std::ceil(std::fabs(theta) / (2 * kPi) * kCircleSegs));
    for (int i = 1; i < n; ++i) {
        double ang = sa + theta * i / n;
        out.push_back({cx + r * std::cos(ang), cy + r * std::sin(ang)});
    }
    out.push_back(b);
}

// Points on an elliptical arc: c + M cos t + m sin t, t from t0 sweeping by `sweep`.
template <class V, class F>
void AddEllipticArc(F make, double t0, double sweep, std::vector<V>& out, bool includeFirst = true) {
    int n = std::max(2, (int)std::ceil(std::fabs(sweep) / (2 * kPi) * kCircleSegs));
    for (int i = includeFirst ? 0 : 1; i <= n; ++i) out.push_back(make(t0 + sweep * i / n));
}

// NURBS evaluation via de Boor. Falls back to control polygon if data is inconsistent.
void EvalSpline(int degree, std::vector<double> knots, const std::vector<Vec3>& ctrl,
                const std::vector<double>& weights, std::vector<Vec3>& out) {
    size_t n = ctrl.size();
    if (n < 2) return;
    int p = std::max(1, std::min(degree, (int)n - 1));
    if (knots.size() != n + p + 1) {  // build clamped uniform knots
        knots.clear();
        for (int i = 0; i <= p; ++i) knots.push_back(0);
        for (size_t i = 1; i + p < n; ++i) knots.push_back((double)i);
        for (int i = 0; i <= p; ++i) knots.push_back((double)(n - p));
    }
    bool rational = weights.size() == n;
    double u0 = knots[p], u1 = knots[n];
    if (!(u1 > u0)) { out = ctrl; return; }
    int samples = (int)std::min<size_t>(4000, std::max<size_t>(32, n * 10));
    struct H { double x, y, z, w; };
    std::vector<H> d(p + 1);
    for (int s = 0; s <= samples; ++s) {
        double u = u0 + (u1 - u0) * s / samples;
        size_t k = (size_t)(std::upper_bound(knots.begin() + p, knots.begin() + n + 1, u) - knots.begin()) - 1;
        if (k < (size_t)p) k = p;
        if (k > n - 1) k = n - 1;
        for (int j = 0; j <= p; ++j) {
            const Vec3& c = ctrl[j + k - p];
            double w = rational ? weights[j + k - p] : 1.0;
            if (w <= 0) w = 1e-9;
            d[j] = {c.x * w, c.y * w, c.z * w, w};
        }
        for (int r = 1; r <= p; ++r) {
            for (int j = p; j >= r; --j) {
                size_t i = j + k - p;
                double denom = knots[i + p - r + 1] - knots[i];
                double alpha = denom == 0 ? 0 : (u - knots[i]) / denom;
                d[j] = {(1 - alpha) * d[j - 1].x + alpha * d[j].x, (1 - alpha) * d[j - 1].y + alpha * d[j].y,
                        (1 - alpha) * d[j - 1].z + alpha * d[j].z, (1 - alpha) * d[j - 1].w + alpha * d[j].w};
            }
        }
        double w = d[p].w != 0 ? d[p].w : 1;
        out.push_back({d[p].x / w, d[p].y / w, d[p].z / w});
    }
}

// Smooth curve through fit points (Catmull-Rom), used when a spline has no control points.
void EvalFitPoints(const std::vector<Vec3>& f, bool closed, std::vector<Vec3>& out) {
    size_t n = f.size();
    if (n < 3) { out = f; return; }
    const int seg = 8;
    size_t last = closed ? n : n - 1;
    auto at = [&](long i) -> Vec3 {
        if (closed) return f[(size_t)((i % (long)n + (long)n) % (long)n)];
        if (i < 0) i = 0;
        if (i >= (long)n) i = (long)n - 1;
        return f[(size_t)i];
    };
    out.push_back(f[0]);
    for (size_t i = 0; i < last; ++i) {
        Vec3 p0 = at((long)i - 1), p1 = at((long)i), p2 = at((long)i + 1), p3 = at((long)i + 2);
        for (int s = 1; s <= seg; ++s) {
            double t = (double)s / seg, t2 = t * t, t3 = t2 * t;
            auto cr = [&](double a, double b, double c, double d) {
                return 0.5 * ((2 * b) + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t2 + (-a + 3 * b - 3 * c + d) * t3);
            };
            out.push_back({cr(p0.x, p1.x, p2.x, p3.x), cr(p0.y, p1.y, p2.y, p3.y), cr(p0.z, p1.z, p2.z, p3.z)});
        }
    }
}

// ---------------------------------------------------------------- parser

class Parser {
public:
    Parser(const char* d, size_t n, Drawing& model, Drawing& paper) : rd_(d, n), model_(model), paper_(paper) {}

    bool Run(std::string* err) {
        Pair p;
        bool sawSection = false;
        while (rd_.Next(p)) {
            if (p.code != 0) continue;
            std::string_view v = Trim(p.value);
            if (v == "EOF") break;
            if (v != "SECTION") continue;
            Pair n;
            if (!rd_.Next(n)) break;
            if (n.code != 2) { rd_.PushBack(n); continue; }
            sawSection = true;
            std::string_view name = Trim(n.value);
            if (name == "HEADER") ParseHeader();
            else if (name == "TABLES") ParseTables();
            else if (name == "BLOCKS") ParseBlocks();
            else if (name == "ENTITIES") ParseEntities();
            else SkipSection();
        }
        if (!sawSection) {
            if (err) *err = rd_.Bad() || rd_.Count() == 0 ? "not an ASCII DXF file" : "no sections found";
            return false;
        }
        for (Entity& e : pending_) EmitTop(e);
        pending_.clear();
        return true;
    }

private:
    // ---- sections
    void SkipSection() {
        Pair p;
        while (rd_.Next(p))
            if (p.code == 0 && Trim(p.value) == "ENDSEC") return;
    }

    void ParseHeader() {
        Pair p;
        std::string var;
        while (rd_.Next(p)) {
            if (p.code == 0) {
                if (Trim(p.value) == "ENDSEC") break;
                continue;
            }
            if (p.code == 9) var = std::string(Trim(p.value));
            else if (var == "$ACADVER" && p.code == 1) acadVer_ = std::string(Trim(p.value));
            else if (var == "$DWGCODEPAGE" && p.code == 3) codepage_ = ParseCodepage(Trim(p.value));
        }
        utf8_ = acadVer_.size() >= 6 && acadVer_.compare(0, 2, "AC") == 0 && acadVer_ >= "AC1021";
    }

    static int ParseCodepage(std::string_view s) {
        std::string u = Upper(s);
        if (u.find("UTF") != std::string::npos) return 65001;
        int v = 0;
        for (char c : u)
            if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
        return v ? v : 1252;
    }

    void ParseTables() {
        Pair p;
        while (rd_.Next(p)) {
            if (p.code != 0) continue;
            std::string_view v = Trim(p.value);
            if (v == "ENDSEC") return;
            if (v != "LAYER") continue;
            ReadRecord();
            std::string name;
            int color = 7, flags = 0;
            int32_t tc = -1;
            for (const Pair& q : buf_) {
                switch (q.code) {
                    case 2: name = Upper(Trim(q.value)); break;
                    case 62: color = ToI(q.value); break;
                    case 70: flags = ToI(q.value); break;
                    case 420: tc = ToI(q.value); break;
                }
            }
            Layer l;
            l.visible = color >= 0 && !(flags & 1);
            l.color = tc >= 0 ? (uint32_t)(tc & 0xFFFFFF) : AciToRgb(std::abs(color));
            layers_[name] = l;
        }
    }

    void ParseBlocks() {
        Pair p;
        while (rd_.Next(p)) {
            if (p.code != 0) continue;
            std::string_view v = Trim(p.value);
            if (v == "ENDSEC") break;
            if (v != "BLOCK") continue;
            ReadRecord();
            std::string name;
            Block b;
            for (const Pair& q : buf_) {
                switch (q.code) {
                    case 2: name = Upper(Trim(q.value)); break;
                    case 10: b.base.x = ToD(q.value); break;
                    case 20: b.base.y = ToD(q.value); break;
                    case 30: b.base.z = ToD(q.value); break;
                }
            }
            bool layout = name.size() > 1 && name[0] == '*' && (name.compare(0, 7, "*PAPER_") == 0 || name.compare(0, 7, "*MODEL_") == 0);
            while (rd_.Next(p)) {
                if (p.code != 0) continue;
                std::string_view t = Trim(p.value);
                if (t == "ENDBLK") { ReadRecord(); break; }
                if (t == "ENDSEC") { rd_.PushBack(p); break; }
                Entity e;
                if (ReadEntity(t, e) && !layout) b.entities.push_back(std::move(e));
            }
            if (!layout) blocks_[name] = std::move(b);
        }
        blocksSeen_ = true;
    }

    void ParseEntities() {
        Pair p;
        while (rd_.Next(p)) {
            if (p.code != 0) continue;
            std::string_view t = Trim(p.value);
            if (t == "ENDSEC") return;
            Entity e;
            if (!ReadEntity(t, e)) continue;
            if (blocksSeen_) EmitTop(e);
            else pending_.push_back(std::move(e));
        }
    }

    // Reads group pairs up to (not including) the next code 0.
    void ReadRecord() {
        buf_.clear();
        Pair p;
        while (rd_.Next(p)) {
            if (p.code == 0) { rd_.PushBack(p); return; }
            buf_.push_back(p);
        }
    }

    // ---- entity parsing
    bool ReadEntity(std::string_view type, Entity& e) {
        ReadRecord();
        e.type = TypeOf(type);
        if (e.type == ET::Unknown) return false;
        ParseCommon(e);
        switch (e.type) {
            case ET::LwPolyline: ParseLwPolyline(e); break;
            case ET::Spline: ParseSpline(e); break;
            case ET::Hatch: ParseHatch(e); break;
            case ET::Leader: ParsePointList(e, 10); break;
            case ET::MLine: ParsePointList(e, 11); break;
            default: ParseSimple(e); break;
        }
        if (e.type == ET::Insert && Trim(type) == "ACAD_TABLE") {  // table = block reference without scale
            e.rot = e.has11 ? std::atan2(e.p[1].y, e.p[1].x) / kDeg : 0;
            e.colSp = e.rowSp = 0;
            e.sx = e.sy = e.sz = 1;
            e.cols = e.rows = 1;
            e.hasAttribs = false;
        }
        if (e.type == ET::Polyline) ReadVertices(e);
        if (e.type == ET::Insert && e.hasAttribs) ReadAttribs(e);
        return true;
    }

    void ParseCommon(Entity& e) {
        for (const Pair& q : buf_) {
            switch (q.code) {
                case 8: e.layer = Upper(Trim(q.value)); break;
                case 62: e.aci = ToI(q.value); break;
                case 420: e.trueColor = ToI(q.value); break;
                case 67: e.paper = ToI(q.value) == 1; break;
                case 60: e.invisible = ToI(q.value) == 1; break;
                case 210: e.extr.x = ToD(q.value); break;
                case 220: e.extr.y = ToD(q.value); break;
                case 230: e.extr.z = ToD(q.value); break;
                case 101: return;  // embedded object data follows; ignore
            }
        }
    }

    void ParseSimple(Entity& e) {
        for (const Pair& q : buf_) {
            int c = q.code;
            if (c == 101) break;
            if (c >= 10 && c <= 13) { e.p[c - 10].x = ToD(q.value); if (c == 11) e.has11 = true; if (c == 13) e.has13 = true; continue; }
            if (c >= 20 && c <= 23) { e.p[c - 20].y = ToD(q.value); continue; }
            if (c >= 30 && c <= 33) { e.p[c - 30].z = ToD(q.value); continue; }
            switch (e.type) {
                case ET::Circle:
                case ET::Arc:
                    if (c == 40) e.r = ToD(q.value);
                    else if (c == 50) e.a0 = ToD(q.value);
                    else if (c == 51) e.a1 = ToD(q.value);
                    break;
                case ET::Ellipse:
                    if (c == 40) e.ratio = ToD(q.value);
                    else if (c == 41) e.a0 = ToD(q.value);
                    else if (c == 42) e.a1 = ToD(q.value);
                    break;
                case ET::Text:
                case ET::Attrib:
                    if (c == 1) e.text = std::string(q.value);
                    else if (c == 40) e.h = ToD(q.value);
                    else if (c == 50) e.rot = ToD(q.value);
                    else if (c == 41) e.wf = ToD(q.value);
                    else if (c == 72) e.hj = ToI(q.value);
                    else if (c == 73 && e.type == ET::Text) e.vj = ToI(q.value);
                    else if (c == 74 && e.type == ET::Attrib) e.vj = ToI(q.value);
                    else if (c == 71) e.flags = ToI(q.value);
                    else if (c == 70 && e.type == ET::Attrib && (ToI(q.value) & 1)) e.invisible = true;
                    break;
                case ET::MText:
                    if (c == 3 || c == 1) {
                        e.text.append(q.value.data(), q.value.size());
                    } else if (c == 40) e.h = ToD(q.value);
                    else if (c == 44) e.lineSpacing = ToD(q.value);
                    else if (c == 50) e.rot = ToD(q.value);
                    else if (c == 71) e.attach = ToI(q.value);
                    break;
                case ET::Insert:
                    if (c == 2) e.name = Upper(Trim(q.value));
                    else if (c == 41) e.sx = ToD(q.value);
                    else if (c == 42) e.sy = ToD(q.value);
                    else if (c == 43) e.sz = ToD(q.value);
                    else if (c == 50) e.rot = ToD(q.value);
                    else if (c == 70) e.cols = ToI(q.value);
                    else if (c == 71) e.rows = ToI(q.value);
                    else if (c == 44) e.colSp = ToD(q.value);
                    else if (c == 45) e.rowSp = ToD(q.value);
                    else if (c == 66) e.hasAttribs = ToI(q.value) != 0;
                    break;
                case ET::Dimension:
                    if (c == 2) e.name = Upper(Trim(q.value));
                    break;
                case ET::Face3D:
                    if (c == 70) e.flags = ToI(q.value);
                    break;
                case ET::Polyline:
                    if (c == 70) e.flags = ToI(q.value);
                    else if (c == 71) e.meshM = ToI(q.value);
                    else if (c == 72) e.meshN = ToI(q.value);
                    break;
                default:
                    break;
            }
        }
        if (e.type == ET::Polyline) e.elevation = e.p[0].z;
    }

    void ParseLwPolyline(Entity& e) {
        for (const Pair& q : buf_) {
            switch (q.code) {
                case 70: e.flags = ToI(q.value); break;
                case 38: e.elevation = ToD(q.value); break;
                case 10: e.verts.push_back({}); e.verts.back().p.x = ToD(q.value); break;
                case 20: if (!e.verts.empty()) e.verts.back().p.y = ToD(q.value); break;
                case 42: if (!e.verts.empty()) e.verts.back().bulge = ToD(q.value); break;
            }
        }
    }

    void ParsePointList(Entity& e, int xcode) {
        for (const Pair& q : buf_) {
            if (q.code == xcode) { e.verts.push_back({}); e.verts.back().p.x = ToD(q.value); }
            else if (q.code == xcode + 10 && !e.verts.empty()) e.verts.back().p.y = ToD(q.value);
            else if (q.code == xcode + 20 && !e.verts.empty()) e.verts.back().p.z = ToD(q.value);
            else if (q.code == 71 && e.type == ET::MLine) e.flags = ToI(q.value);
        }
    }

    void ParseSpline(Entity& e) {
        for (const Pair& q : buf_) {
            switch (q.code) {
                case 70: e.flags = ToI(q.value); break;
                case 71: e.degree = ToI(q.value); break;
                case 40: e.knots.push_back(ToD(q.value)); break;
                case 41: e.weights.push_back(ToD(q.value)); break;
                case 10: e.ctrl.push_back({ToD(q.value), 0, 0}); break;
                case 20: if (!e.ctrl.empty()) e.ctrl.back().y = ToD(q.value); break;
                case 30: if (!e.ctrl.empty()) e.ctrl.back().z = ToD(q.value); break;
                case 11: e.fit.push_back({ToD(q.value), 0, 0}); break;
                case 21: if (!e.fit.empty()) e.fit.back().y = ToD(q.value); break;
                case 31: if (!e.fit.empty()) e.fit.back().z = ToD(q.value); break;
            }
        }
    }

    void ReadVertices(Entity& e) {
        Pair p;
        while (rd_.Next(p)) {
            if (p.code != 0) continue;
            std::string_view t = Trim(p.value);
            if (t == "VERTEX") {
                ReadRecord();
                Vertex v;
                for (const Pair& q : buf_) {
                    switch (q.code) {
                        case 10: v.p.x = ToD(q.value); break;
                        case 20: v.p.y = ToD(q.value); break;
                        case 30: v.p.z = ToD(q.value); break;
                        case 42: v.bulge = ToD(q.value); break;
                        case 70: v.flags = ToI(q.value); break;
                        case 71: v.idx[0] = ToI(q.value); break;
                        case 72: v.idx[1] = ToI(q.value); break;
                        case 73: v.idx[2] = ToI(q.value); break;
                        case 74: v.idx[3] = ToI(q.value); break;
                    }
                }
                e.verts.push_back(v);
            } else if (t == "SEQEND") {
                ReadRecord();
                return;
            } else {
                rd_.PushBack(p);
                return;
            }
        }
    }

    void ReadAttribs(Entity& e) {
        Pair p;
        while (rd_.Next(p)) {
            if (p.code != 0) continue;
            std::string_view t = Trim(p.value);
            if (t == "ATTRIB") {
                Entity a;
                if (ReadEntity(t, a)) e.attribs.push_back(std::move(a));
            } else if (t == "SEQEND") {
                ReadRecord();
                return;
            } else {
                rd_.PushBack(p);
                return;
            }
        }
    }

    // ---- HATCH
    void ParseHatch(Entity& e) {
        const std::vector<Pair>& v = buf_;
        size_t n = v.size(), i = 0;
        int npaths = 0;
        bool gotElev = false;
        for (; i < n; ++i) {
            int c = v[i].code;
            if (c == 30 && !gotElev) { e.elevation = ToD(v[i].value); gotElev = true; }
            else if (c == 70) e.solidFill = ToI(v[i].value) == 1;
            else if (c == 91) { npaths = ToI(v[i].value); ++i; break; }
        }
        for (int pi = 0; pi < npaths && i < n; ++pi) {
            while (i < n && v[i].code != 92) ++i;
            if (i >= n) break;
            int ptype = ToI(v[i].value);
            ++i;
            std::vector<Vec2> ring;
            if (ptype & 2) {
                bool hasBulge = false;
                int nv = 0;
                while (i < n && (v[i].code == 72 || v[i].code == 73 || v[i].code == 93)) {
                    if (v[i].code == 72) hasBulge = ToI(v[i].value) != 0;
                    else if (v[i].code == 93) nv = ToI(v[i].value);
                    ++i;
                }
                std::vector<Vertex> pts;
                for (int k = 0; k < nv && i < n; ++k) {
                    Vertex vx;
                    if (v[i].code != 10) break;
                    vx.p.x = ToD(v[i].value); ++i;
                    if (i < n && v[i].code == 20) { vx.p.y = ToD(v[i].value); ++i; }
                    if (hasBulge && i < n && v[i].code == 42) { vx.bulge = ToD(v[i].value); ++i; }
                    pts.push_back(vx);
                }
                if (!pts.empty()) {
                    ring.push_back({pts[0].p.x, pts[0].p.y});
                    for (size_t k = 0; k < pts.size(); ++k) {
                        const Vertex& a = pts[k];
                        const Vertex& b = pts[(k + 1) % pts.size()];
                        if (k + 1 == pts.size() && std::fabs(a.bulge) < 1e-9) break;
                        AddBulge({a.p.x, a.p.y}, {b.p.x, b.p.y}, a.bulge, ring);
                    }
                }
            } else {
                int nedges = 0;
                if (i < n && v[i].code == 93) { nedges = ToI(v[i].value); ++i; }
                for (int k = 0; k < nedges && i < n; ++k) {
                    if (v[i].code != 72) break;
                    int et = ToI(v[i].value);
                    ++i;
                    ParseHatchEdge(et, v, i, ring);
                }
            }
            if (ring.size() >= 2) e.rings.push_back(std::move(ring));
        }
        // pattern definition
        for (; i < n; ++i) {
            if (v[i].code == 450 && ToI(v[i].value) == 1) e.solidFill = true;
            if (v[i].code != 78) continue;
            int nl = ToI(v[i].value);
            ++i;
            for (int l = 0; l < nl && i < n; ++l) {
                PatternLine pl;
                if (v[i].code != 53) break;
                pl.angle = ToD(v[i].value);
                ++i;
                while (i < n && v[i].code >= 43 && v[i].code <= 46) {
                    double d = ToD(v[i].value);
                    switch (v[i].code) {
                        case 43: pl.base.x = d; break;
                        case 44: pl.base.y = d; break;
                        case 45: pl.offset.x = d; break;
                        case 46: pl.offset.y = d; break;
                    }
                    ++i;
                }
                int nd = 0;
                if (i < n && v[i].code == 79) { nd = ToI(v[i].value); ++i; }
                for (int d = 0; d < nd && i < n && v[i].code == 49; ++d, ++i) pl.dashes.push_back(ToD(v[i].value));
                e.pattern.push_back(std::move(pl));
            }
            if (i > 0) --i;
        }
    }

    static void PushUnique(std::vector<Vec2>& ring, Vec2 p) {
        if (!ring.empty() && std::fabs(ring.back().x - p.x) < 1e-12 && std::fabs(ring.back().y - p.y) < 1e-12) return;
        ring.push_back(p);
    }

    void ParseHatchEdge(int et, const std::vector<Pair>& v, size_t& i, std::vector<Vec2>& ring) {
        size_t n = v.size();
        if (et == 1) {  // line
            Vec2 a, b;
            while (i < n && (v[i].code == 10 || v[i].code == 20 || v[i].code == 11 || v[i].code == 21)) {
                double d = ToD(v[i].value);
                switch (v[i].code) { case 10: a.x = d; break; case 20: a.y = d; break; case 11: b.x = d; break; case 21: b.y = d; break; }
                ++i;
            }
            PushUnique(ring, a);
            PushUnique(ring, b);
        } else if (et == 2 || et == 3) {  // circular / elliptic arc
            Vec2 c, m{1, 0};
            double r = 0, s = 0, en = 360, ratio = 1;
            bool ccw = true;
            while (i < n) {
                int code = v[i].code;
                double d = ToD(v[i].value);
                if (code == 10) c.x = d;
                else if (code == 20) c.y = d;
                else if (code == 11 && et == 3) m.x = d;
                else if (code == 21 && et == 3) m.y = d;
                else if (code == 40) { if (et == 2) r = d; else ratio = d; }
                else if (code == 50) s = d;
                else if (code == 51) en = d;
                else if (code == 73) ccw = ToI(v[i].value) != 0;
                else break;
                ++i;
            }
            Vec2 major = et == 2 ? Vec2{r, 0} : m;
            Vec2 minor = {-major.y * (et == 2 ? 1 : ratio), major.x * (et == 2 ? 1 : ratio)};
            double sweep = en - s;
            if (std::fabs(sweep) >= 360 - 1e-9) sweep = 360;
            else sweep = Norm360(sweep);
            double t0 = (ccw ? s : -s) * kDeg;
            double sw = (ccw ? sweep : -sweep) * kDeg;
            std::vector<Vec2> pts;
            AddEllipticArc<Vec2>([&](double t) { return Vec2{c.x + major.x * std::cos(t) + minor.x * std::sin(t),
                                                            c.y + major.y * std::cos(t) + minor.y * std::sin(t)}; },
                                 t0, sw, pts);
            for (const Vec2& p : pts) PushUnique(ring, p);
        } else if (et == 4) {  // spline
            int degree = 3;
            std::vector<double> knots, weights;
            std::vector<Vec3> ctrl, fit;
            while (i < n) {
                int code = v[i].code;
                double d = ToD(v[i].value);
                if (code == 94) degree = ToI(v[i].value);
                else if (code == 73 || code == 74 || code == 95 || code == 96 || code == 97 ||
                         code == 12 || code == 22 || code == 13 || code == 23) {}
                else if (code == 40) knots.push_back(d);
                else if (code == 42) weights.push_back(d);
                else if (code == 10) ctrl.push_back({d, 0, 0});
                else if (code == 20) { if (!ctrl.empty()) ctrl.back().y = d; }
                else if (code == 11) fit.push_back({d, 0, 0});
                else if (code == 21) { if (!fit.empty()) fit.back().y = d; }
                else break;
                ++i;
            }
            std::vector<Vec3> pts;
            if (ctrl.size() >= 2) EvalSpline(degree, knots, ctrl, weights, pts);
            else EvalFitPoints(fit, false, pts);
            for (const Vec3& p : pts) PushUnique(ring, {p.x, p.y});
        } else {
            // unknown edge type: skip its data
            while (i < n && v[i].code != 72 && v[i].code != 92 && v[i].code != 97) ++i;
        }
    }

    // ---- text decoding
    uint32_t CpChar(unsigned char b) const {
        const uint16_t* t = nullptr;
        switch (codepage_) {
            case 1250: t = kCp1250; break;
            case 1251: t = kCp1251; break;
            case 866: t = kCp866; break;
            default: t = kCp1252; break;
        }
        return t[b - 0x80];
    }

    std::string Decode(std::string_view raw) const {
        bool asUtf8 = utf8_ || codepage_ == 65001 || LooksUtf8(raw);
        std::string out;
        out.reserve(raw.size() + 8);
        size_t n = raw.size();
        for (size_t i = 0; i < n;) {
            char ch = raw[i];
            if (ch == '\\' && i + 6 < n + 0 && (raw[i + 1] == 'U' || raw[i + 1] == 'u') && raw[i + 2] == '+') {
                int a = HexVal(raw[i + 3]), b = HexVal(raw[i + 4]), c = HexVal(raw[i + 5]), d = HexVal(raw[i + 6]);
                if (a >= 0 && b >= 0 && c >= 0 && d >= 0) {
                    AppendUtf8(out, (uint32_t)((a << 12) | (b << 8) | (c << 4) | d));
                    i += 7;
                    continue;
                }
            }
            if (ch == '%' && i + 2 < n && raw[i + 1] == '%') {
                char c = raw[i + 2];
                if (c == 'c' || c == 'C') { AppendUtf8(out, 0x00D8); i += 3; continue; }
                if (c == 'd' || c == 'D') { AppendUtf8(out, 0x00B0); i += 3; continue; }
                if (c == 'p' || c == 'P') { AppendUtf8(out, 0x00B1); i += 3; continue; }
                if (c == '%') { out += '%'; i += 3; continue; }
                if (c == 'u' || c == 'U' || c == 'o' || c == 'O' || c == 'k' || c == 'K') { i += 3; continue; }
                if (c >= '0' && c <= '9' && i + 4 < n && raw[i + 3] >= '0' && raw[i + 3] <= '9' && raw[i + 4] >= '0' && raw[i + 4] <= '9') {
                    int code = (c - '0') * 100 + (raw[i + 3] - '0') * 10 + (raw[i + 4] - '0');
                    if (code >= 0x80) AppendUtf8(out, CpChar((unsigned char)code));
                    else if (code >= 32) out += (char)code;
                    i += 5;
                    continue;
                }
            }
            unsigned char b = (unsigned char)ch;
            if (b < 0x80 || asUtf8) { out += ch; ++i; continue; }
            AppendUtf8(out, CpChar(b));
            ++i;
        }
        for (char& c : out)
            if (c == '\x01') c = '\\';
        return out;
    }

    // Removes MTEXT formatting codes; returns raw bytes with '\n' line breaks.
    static std::string StripMText(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        size_t n = s.size();
        for (size_t i = 0; i < n;) {
            char ch = s[i];
            if (ch == '{' || ch == '}') { ++i; continue; }
            if (ch != '\\' || i + 1 >= n) {
                if (ch == '^' && i + 1 < n && (s[i + 1] == 'J' || s[i + 1] == 'M')) { out += '\n'; i += 2; continue; }
                if (ch == '^' && i + 1 < n && s[i + 1] == 'I') { out += ' '; i += 2; continue; }
                out += ch;
                ++i;
                continue;
            }
            char c = s[i + 1];
            switch (c) {
                case 'P': case 'N': case 'X': out += '\n'; i += 2; break;
                case '~': out += ' '; i += 2; break;
                case '\\': out += '\x01'; i += 2; break;
                case '{': case '}': out += c; i += 2; break;
                case 'U': case 'u': out += '\\'; out += c; i += 2; break;
                case 'M': case 'm': i = std::min(n, i + 8); break;
                case 'S': {
                    size_t j = i + 2;
                    std::string num, den;
                    bool second = false;
                    while (j < n && s[j] != ';') {
                        char d = s[j];
                        if (d == '\\' && j + 1 < n) { (second ? den : num) += s[j + 1]; j += 2; continue; }
                        if (!second && (d == '^' || d == '/' || d == '#')) { second = true; ++j; continue; }
                        (second ? den : num) += d;
                        ++j;
                    }
                    out += num;
                    if (second && !den.empty()) { out += '/'; out += den; }
                    i = j < n ? j + 1 : n;
                    break;
                }
                case 'f': case 'F': case 'H': case 'W': case 'Q': case 'T': case 'A':
                case 'C': case 'c': case 'p': {
                    size_t j = i + 2;
                    while (j < n && s[j] != ';') ++j;
                    i = j < n ? j + 1 : n;
                    break;
                }
                case 'L': case 'l': case 'O': case 'o': case 'K': case 'k':
                    i += 2;
                    break;
                default:
                    out += c;
                    i += 2;
                    break;
            }
        }
        return out;
    }

    // ---- emitting
    struct Ctx {
        Mat m;
        std::string parentLayer;   // empty at top level
        uint32_t blockColor = kForeground;
        int depth = 0;
        Drawing* out = nullptr;
    };

    const Layer* FindLayer(const std::string& name) const {
        auto it = layers_.find(name);
        return it == layers_.end() ? nullptr : &it->second;
    }

    const std::string& EffLayer(const Entity& e, const Ctx& ctx) const {
        if (!ctx.parentLayer.empty() && e.layer == "0") return ctx.parentLayer;
        return e.layer;
    }

    bool Visible(const Entity& e, const Ctx& ctx) const {
        if (e.invisible) return false;
        const Layer* l = FindLayer(EffLayer(e, ctx));
        return !l || l->visible;
    }

    uint32_t ColorOf(const Entity& e, const Ctx& ctx) const {
        if (e.trueColor >= 0) return (uint32_t)(e.trueColor & 0xFFFFFF);
        if (e.aci == 0) return ctx.blockColor;
        if (e.aci > 0 && e.aci < 256) return AciToRgb(e.aci);
        const Layer* l = FindLayer(EffLayer(e, ctx));
        return l ? l->color : kForeground;
    }

    void EmitTop(const Entity& e) {
        Ctx ctx;
        ctx.m = Mat::Identity();
        ctx.out = e.paper ? &paper_ : &model_;
        Emit(e, ctx);
    }

    bool OnStack(const Block* b) const { return std::find(stack_.begin(), stack_.end(), b) != stack_.end(); }

    bool Budget(const Ctx& ctx) {
        if (ctx.out->points.size() > kMaxPoints) { ctx.out->truncated = true; return false; }
        return true;
    }

    void AddPath(const Ctx& ctx, const std::vector<Vec3>& pts, bool closed, uint32_t color) {
        if (pts.size() < 2) return;
        Drawing& d = *ctx.out;
        Path p;
        p.color = color;
        p.first = (uint32_t)d.points.size();
        p.closed = closed;
        for (const Vec3& v : pts) {
            Vec3 w = ctx.m.Apply(v);
            if (!std::isfinite(w.x) || !std::isfinite(w.y)) continue;
            d.points.push_back({w.x, w.y});
            p.box.add(w.x, w.y);
        }
        p.count = (uint32_t)(d.points.size() - p.first);
        if (p.count < 2) { d.points.resize(p.first); return; }
        d.paths.push_back(p);
    }

    void AddFill(const Ctx& ctx, const std::vector<std::vector<Vec3>>& rings, uint32_t color, uint8_t alpha) {
        Drawing& d = *ctx.out;
        Fill f;
        f.color = color;
        f.alpha = alpha;
        f.firstRing = (uint32_t)d.rings.size();
        for (const auto& r : rings) {
            if (r.size() < 3) continue;
            Ring rg;
            rg.first = (uint32_t)d.points.size();
            for (const Vec3& v : r) {
                Vec3 w = ctx.m.Apply(v);
                if (!std::isfinite(w.x) || !std::isfinite(w.y)) continue;
                d.points.push_back({w.x, w.y});
                f.box.add(w.x, w.y);
            }
            rg.count = (uint32_t)(d.points.size() - rg.first);
            if (rg.count < 3) { d.points.resize(rg.first); continue; }
            d.rings.push_back(rg);
        }
        f.ringCount = (uint32_t)(d.rings.size() - f.firstRing);
        if (f.ringCount) d.fills.push_back(f);
    }

    void Emit(const Entity& e, const Ctx& ctx) {
        if (++emitted_ > kMaxEmits) { ctx.out->truncated = true; return; }
        if (!Visible(e, ctx) || !Budget(ctx)) return;
        uint32_t color = ColorOf(e, ctx);
        std::vector<Vec3> pts;
        switch (e.type) {
            case ET::Line:
                pts = {e.p[0], e.p[1]};
                AddPath(ctx, pts, false, color);
                break;

            case ET::Circle:
            case ET::Arc: {
                if (e.r <= 0) break;
                Ctx c = ctx;
                c.m = ctx.m * OcsMatrix(e.extr);
                double t0 = 0, sweep = 2 * kPi;
                if (e.type == ET::Arc) {
                    t0 = e.a0 * kDeg;
                    double s = Norm360(e.a1 - e.a0);
                    if (s < 1e-9) s = 360;
                    sweep = s * kDeg;
                }
                AddEllipticArc<Vec3>([&](double t) { return Vec3{e.p[0].x + e.r * std::cos(t), e.p[0].y + e.r * std::sin(t), e.p[0].z}; },
                                     t0, sweep, pts);
                if (e.type == ET::Circle) pts.pop_back();
                AddPath(c, pts, e.type == ET::Circle, color);
                break;
            }

            case ET::LwPolyline: {
                if (e.verts.empty()) break;
                Ctx c = ctx;
                c.m = ctx.m * OcsMatrix(e.extr);
                bool closed = (e.flags & 1) != 0;
                std::vector<Vec2> p2;
                p2.push_back({e.verts[0].p.x, e.verts[0].p.y});
                size_t nv = e.verts.size();
                for (size_t i = 0; i + 1 < nv || (closed && i < nv); ++i) {
                    const Vertex& a = e.verts[i];
                    const Vertex& b = e.verts[(i + 1) % nv];
                    if (i + 1 == nv && std::fabs(a.bulge) < 1e-9) break;  // straight closing segment drawn by "closed"
                    AddBulge({a.p.x, a.p.y}, {b.p.x, b.p.y}, a.bulge, p2);
                }
                for (const Vec2& v : p2) pts.push_back({v.x, v.y, e.elevation});
                if (nv == 1) pts.push_back(pts[0]);
                AddPath(c, pts, closed, color);
                break;
            }

            case ET::Polyline: EmitPolyline(e, ctx, color); break;

            case ET::Ellipse: {
                Vec3 n = Normalize(e.extr);
                Vec3 major = e.p[1];
                Vec3 minor = Mul(Cross(n, major), e.ratio);
                double t0 = e.a0, t1 = e.a1;
                double sweep = t1 - t0;
                if (std::fabs(sweep) < 1e-9 || std::fabs(sweep - 2 * kPi) < 1e-9) sweep = 2 * kPi;
                else { while (sweep < 0) sweep += 2 * kPi; while (sweep > 2 * kPi) sweep -= 2 * kPi; }
                AddEllipticArc<Vec3>([&](double t) { return Add(e.p[0], Add(Mul(major, std::cos(t)), Mul(minor, std::sin(t)))); },
                                     t0, sweep, pts);
                bool full = std::fabs(sweep - 2 * kPi) < 1e-9;
                if (full) pts.pop_back();
                AddPath(ctx, pts, full, color);
                break;
            }

            case ET::Spline: {
                bool closed = (e.flags & 1) != 0;
                if (e.ctrl.size() >= 2) EvalSpline(e.degree, e.knots, e.ctrl, e.weights, pts);
                else EvalFitPoints(e.fit, closed, pts);
                AddPath(ctx, pts, false, color);
                break;
            }

            case ET::Text:
            case ET::Attrib: EmitText(e, ctx, color); break;
            case ET::MText: EmitMText(e, ctx, color); break;

            case ET::Insert: {
                if (ctx.depth < kMaxDepth) {
                    auto it = blocks_.find(e.name);
                    if (it != blocks_.end() && !OnStack(&it->second)) {
                        const Block& b = it->second;
                        stack_.push_back(&b);
                        Ctx c;
                        c.out = ctx.out;
                        c.depth = ctx.depth + 1;
                        c.parentLayer = EffLayer(e, ctx);
                        c.blockColor = color;
                        int cols = std::max(1, std::min(e.cols, 1000));
                        int rows = std::max(1, std::min(e.rows, 1000));
                        if ((long long)cols * rows > 10000) { cols = std::min(cols, 100); rows = std::min(rows, 100); }
                        Mat base = ctx.m * OcsMatrix(e.extr) * Mat::Translate(e.p[0].x, e.p[0].y, e.p[0].z) * Mat::RotZ(e.rot * kDeg);
                        Mat tail = Mat::Scale(e.sx == 0 ? 1 : e.sx, e.sy == 0 ? 1 : e.sy, e.sz == 0 ? 1 : e.sz) *
                                   Mat::Translate(-b.base.x, -b.base.y, -b.base.z);
                        for (int r = 0; r < rows; ++r) {
                            for (int col = 0; col < cols; ++col) {
                                c.m = base * Mat::Translate(col * e.colSp, r * e.rowSp, 0) * tail;
                                for (const Entity& be : b.entities) Emit(be, c);
                            }
                        }
                        stack_.pop_back();
                    }
                }
                for (const Entity& a : e.attribs) Emit(a, ctx);
                break;
            }

            case ET::Dimension: {
                if (ctx.depth >= kMaxDepth) break;
                auto it = blocks_.find(e.name);
                if (it == blocks_.end() || OnStack(&it->second)) break;
                stack_.push_back(&it->second);
                Ctx c = ctx;
                c.depth = ctx.depth + 1;
                c.parentLayer = EffLayer(e, ctx);
                c.blockColor = color;
                for (const Entity& be : it->second.entities) Emit(be, c);
                stack_.pop_back();
                break;
            }

            case ET::Solid: {
                Ctx c = ctx;
                c.m = ctx.m * OcsMatrix(e.extr);
                Vec3 p3 = e.has13 ? e.p[3] : e.p[2];
                std::vector<std::vector<Vec3>> rings(1);
                rings[0] = {e.p[0], e.p[1], p3, e.p[2]};
                AddFill(c, rings, color, 255);
                break;
            }

            case ET::Face3D: {
                Vec3 p3 = e.has13 ? e.p[3] : e.p[2];
                Vec3 q[4] = {e.p[0], e.p[1], e.p[2], p3};
                for (int k = 0; k < 4; ++k) {
                    if (e.flags & (1 << k)) continue;
                    pts = {q[k], q[(k + 1) % 4]};
                    if (Len(Sub(pts[0], pts[1])) > 0) AddPath(ctx, pts, false, color);
                }
                break;
            }

            case ET::Hatch: EmitHatch(e, ctx, color); break;

            case ET::Leader:
            case ET::MLine: {
                for (const Vertex& v : e.verts) pts.push_back(v.p);
                AddPath(ctx, pts, e.type == ET::MLine && (e.flags & 2), color);
                break;
            }
            default: break;
        }
    }

    void EmitPolyline(const Entity& e, const Ctx& ctx, uint32_t color) {
        std::vector<Vec3> pts;
        bool closed = (e.flags & 1) != 0;
        if (e.flags & 64) {  // polyface mesh
            std::vector<Vec3> vtx;
            for (const Vertex& v : e.verts)
                if ((v.flags & 64) && (v.flags & 128)) vtx.push_back(v.p);
            for (const Vertex& v : e.verts) {
                if ((v.flags & 128) && !(v.flags & 64)) {
                    int idx[4];
                    int cnt = 0;
                    for (int k = 0; k < 4; ++k)
                        if (v.idx[k] != 0) idx[cnt++] = v.idx[k];
                    for (int k = 0; k < cnt; ++k) {
                        int a = idx[k], b = idx[(k + 1) % cnt];
                        if (a < 0) continue;  // invisible edge
                        int ia = std::abs(a) - 1, ib = std::abs(b) - 1;
                        if (ia < 0 || ib < 0 || ia >= (int)vtx.size() || ib >= (int)vtx.size()) continue;
                        pts = {vtx[(size_t)ia], vtx[(size_t)ib]};
                        AddPath(ctx, pts, false, color);
                    }
                }
            }
            return;
        }
        if (e.flags & 16) {  // polygon mesh M x N
            int m = e.meshM, n = e.meshN;
            if (m <= 0 || n <= 0 || (size_t)m * n > e.verts.size()) return;
            for (int i = 0; i < m; ++i) {
                pts.clear();
                for (int j = 0; j < n; ++j) pts.push_back(e.verts[(size_t)i * n + j].p);
                AddPath(ctx, pts, (e.flags & 32) != 0, color);
            }
            for (int j = 0; j < n; ++j) {
                pts.clear();
                for (int i = 0; i < m; ++i) pts.push_back(e.verts[(size_t)i * n + j].p);
                AddPath(ctx, pts, (e.flags & 1) != 0, color);
            }
            return;
        }
        std::vector<Vertex> vs;
        for (const Vertex& v : e.verts)
            if (!(v.flags & 16)) vs.push_back(v);  // skip spline frame control points
        if (vs.empty()) return;
        if (e.flags & 8) {  // 3D polyline, WCS
            for (const Vertex& v : vs) pts.push_back(v.p);
            if (pts.size() == 1) pts.push_back(pts[0]);
            AddPath(ctx, pts, closed, color);
            return;
        }
        Ctx c = ctx;
        c.m = ctx.m * OcsMatrix(e.extr);
        std::vector<Vec2> p2;
        p2.push_back({vs[0].p.x, vs[0].p.y});
        size_t nv = vs.size();
        for (size_t i = 0; i + 1 < nv || (closed && i < nv); ++i) {
            const Vertex& a = vs[i];
            const Vertex& b = vs[(i + 1) % nv];
            if (i + 1 == nv && std::fabs(a.bulge) < 1e-9) break;
            AddBulge({a.p.x, a.p.y}, {b.p.x, b.p.y}, a.bulge, p2);
        }
        for (const Vec2& v : p2) pts.push_back({v.x, v.y, e.elevation});
        if (pts.size() == 1) pts.push_back(pts[0]);
        AddPath(c, pts, closed, color);
    }

    // Common text placement. A = anchor (world), dx/dy = transformed unit X/Y of text frame.
    void PlaceText(const Ctx& ctx, Text& t, Vec3 A, Vec3 dx, Vec3 dy, double h, double wf, int nLines) {
        double lx = std::hypot(dx.x, dx.y), ly = std::hypot(dy.x, dy.y);
        if (lx < 1e-12 || ly < 1e-12 || h <= 0) return;
        t.pos = {A.x, A.y};
        t.rotation = std::atan2(dx.y, dx.x);
        t.height = h * ly;
        t.widthFactor = (wf > 0 ? wf : 1) * lx / ly;
        t.lineStep *= ly;
        t.baseY *= ly;
        t.fitLength *= lx;
        if (dx.x * dy.y - dx.y * dy.x < 0) t.flipY = !t.flipY;
        // approximate box
        size_t maxChars = 1;
        for (const auto& s : t.lines) {
            size_t cnt = 0;
            for (unsigned char ch : s)
                if ((ch & 0xC0) != 0x80) ++cnt;
            maxChars = std::max(maxChars, cnt);
        }
        double W = t.fitLength > 0 ? t.fitLength : t.height * t.widthFactor * 0.85 * (double)maxChars;
        double x0 = t.hAlign == 0 ? 0 : t.hAlign == 1 ? -W / 2 : -W;
        double top = t.baseY + t.height, bot = t.baseY - (nLines - 1) * t.lineStep - 0.3 * t.height;
        double c = std::cos(t.rotation), s = std::sin(t.rotation);
        double xs[2] = {x0, x0 + W}, ys[2] = {bot, top};
        for (double x : xs)
            for (double y : ys) t.box.add(t.pos.x + x * c - y * s, t.pos.y + x * s + y * c);
        ctx.out->texts.push_back(std::move(t));
    }

    static std::vector<std::string> SplitLines(const std::string& s) {
        std::vector<std::string> r;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == '\n') {
                r.push_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
        while (r.size() > 1 && r.back().empty()) r.pop_back();
        return r;
    }

    static bool Blank(const std::vector<std::string>& lines) {
        for (const auto& l : lines)
            for (char c : l)
                if (c != ' ') return false;
        return true;
    }

    void EmitText(const Entity& e, const Ctx& ctx, uint32_t color) {
        Text t;
        t.lines.push_back(Decode(e.text));
        if (Blank(t.lines) || e.h <= 0) return;
        t.color = color;
        Mat M = ctx.m * OcsMatrix(e.extr);
        double rot = e.rot * kDeg;
        Vec3 anchor = e.p[0];
        int hj = e.hj, vj = e.vj;
        if (hj == 3 || hj == 5) {  // aligned / fit: between p0 and p1
            if (e.has11) {
                double ddx = e.p[1].x - e.p[0].x, ddy = e.p[1].y - e.p[0].y;
                double len = std::hypot(ddx, ddy);
                if (len > 1e-12) { rot = std::atan2(ddy, ddx); t.fitLength = len; }
            }
            hj = 0; vj = 0;
        } else if (hj == 4) {
            hj = 1; vj = 2;
            if (e.has11) anchor = e.p[1];
        } else if ((hj != 0 || vj != 0) && e.has11) {
            anchor = e.p[1];
        }
        t.hAlign = std::max(0, std::min(hj, 2));
        double h = e.h;
        t.lineStep = h * 5.0 / 3.0;
        switch (vj) {
            case 1: t.baseY = 0.3 * h; break;
            case 2: t.baseY = -h / 2; break;
            case 3: t.baseY = -h; break;
            default: t.baseY = 0; break;
        }
        if (e.flags & 2) t.flipX = true;
        if (e.flags & 4) t.flipY = true;
        Vec3 A = M.Apply(anchor);
        Vec3 dx = M.ApplyVec({std::cos(rot), std::sin(rot), 0});
        Vec3 dy = M.ApplyVec({-std::sin(rot), std::cos(rot), 0});
        PlaceText(ctx, t, A, dx, dy, h, e.wf, 1);
    }

    void EmitMText(const Entity& e, const Ctx& ctx, uint32_t color) {
        if (e.h <= 0) return;
        Text t;
        t.lines = SplitLines(Decode(StripMText(e.text)));
        if (Blank(t.lines)) return;
        t.color = color;
        Vec3 n = Normalize(e.extr);
        Vec3 dir;
        if (e.has11 && Len(e.p[1]) > 1e-12) dir = Normalize(e.p[1]);
        else dir = OcsMatrix(n).ApplyVec({std::cos(e.rot), std::sin(e.rot), 0});
        Vec3 up = Normalize(Cross(n, dir));
        int attach = std::max(1, std::min(e.attach, 9));
        t.hAlign = (attach - 1) % 3;
        int vrow = (attach - 1) / 3;
        double h = e.h;
        int nl = (int)t.lines.size();
        t.lineStep = h * 5.0 / 3.0 * (e.lineSpacing > 0.1 ? e.lineSpacing : 1.0);
        double extra = (nl - 1) * t.lineStep;
        if (vrow == 0) t.baseY = -h;
        else if (vrow == 1) t.baseY = -(h - extra) / 2;
        else t.baseY = extra;
        Vec3 A = ctx.m.Apply(e.p[0]);
        PlaceText(ctx, t, A, ctx.m.ApplyVec(dir), ctx.m.ApplyVec(up), h, 1.0, nl);
    }

    void EmitHatch(const Entity& e, const Ctx& ctx, uint32_t color) {
        if (e.rings.empty()) return;
        Ctx c = ctx;
        c.m = ctx.m * OcsMatrix(e.extr);
        std::vector<std::vector<Vec3>> rings;
        BBox ob;
        for (const auto& r : e.rings) {
            std::vector<Vec3> rr;
            for (const Vec2& p : r) { rr.push_back({p.x, p.y, e.elevation}); ob.add(p.x, p.y); }
            rings.push_back(std::move(rr));
        }
        if (e.solidFill || e.pattern.empty()) {
            if (e.solidFill) AddFill(c, rings, color, 255);
            else for (const auto& r : rings) AddPath(c, r, true, color);
            return;
        }
        // Generate pattern lines clipped to the boundary (even-odd).
        size_t segBudget = 300000;
        std::vector<double> xs;
        std::vector<Vec3> seg(2);
        for (const PatternLine& pl : e.pattern) {
            double a = pl.angle * kDeg;
            Vec2 d{std::cos(a), std::sin(a)}, nrm{-d.y, d.x};
            double step = pl.offset.x * nrm.x + pl.offset.y * nrm.y;
            if (std::fabs(step) < 1e-12) continue;
            double shift = pl.offset.x * d.x + pl.offset.y * d.y;
            double nmin = 1e300, nmax = -1e300;
            double cs[4][2] = {{ob.minx, ob.miny}, {ob.maxx, ob.miny}, {ob.minx, ob.maxy}, {ob.maxx, ob.maxy}};
            for (auto& q : cs) {
                double v = q[0] * nrm.x + q[1] * nrm.y;
                nmin = std::min(nmin, v);
                nmax = std::max(nmax, v);
            }
            double b0 = pl.base.x * nrm.x + pl.base.y * nrm.y;
            double k0 = (nmin - b0) / step, k1 = (nmax - b0) / step;
            if (k0 > k1) std::swap(k0, k1);
            long long kStart = (long long)std::ceil(k0), kEnd = (long long)std::floor(k1);
            if (kEnd - kStart > 5000) {  // too dense to draw as lines: light tint instead
                AddFill(c, rings, color, 60);
                return;
            }
            double dashLen = 0;
            for (double v : pl.dashes) dashLen += std::fabs(v);
            for (long long k = kStart; k <= kEnd; ++k) {
                Vec2 P{pl.base.x + pl.offset.x * (double)k, pl.base.y + pl.offset.y * (double)k};
                xs.clear();
                for (const auto& r : e.rings) {
                    size_t m = r.size();
                    for (size_t j = 0; j < m; ++j) {
                        Vec2 A = r[j], B = r[(j + 1) % m];
                        double da = (A.x - P.x) * nrm.x + (A.y - P.y) * nrm.y;
                        double db = (B.x - P.x) * nrm.x + (B.y - P.y) * nrm.y;
                        if ((da > 0) == (db > 0)) continue;
                        double t = da / (da - db);
                        double X = A.x + (B.x - A.x) * t, Y = A.y + (B.y - A.y) * t;
                        xs.push_back((X - P.x) * d.x + (Y - P.y) * d.y);
                    }
                }
                std::sort(xs.begin(), xs.end());
                for (size_t j = 0; j + 1 < xs.size(); j += 2) {
                    double u0 = xs[j], u1 = xs[j + 1];
                    auto addSeg = [&](double s0, double s1) {
                        seg[0] = {P.x + d.x * s0, P.y + d.y * s0, e.elevation};
                        seg[1] = {P.x + d.x * s1, P.y + d.y * s1, e.elevation};
                        AddPath(c, seg, false, color);
                    };
                    if (pl.dashes.empty() || dashLen < 1e-12 || (u1 - u0) / dashLen > 2000) {
                        addSeg(u0, u1);
                        if (--segBudget == 0) return;
                        continue;
                    }
                    double phase = shift * (double)k;  // dash pattern origin along the line
                    double start = phase + std::floor((u0 - phase) / dashLen) * dashLen;
                    for (double pos = start; pos < u1;) {
                        for (double dv : pl.dashes) {
                            double len = std::fabs(dv);
                            double s0 = pos, s1 = pos + len;
                            if (dv >= 0) {
                                double a0 = std::max(s0, u0), a1 = std::min(s1, u1);
                                if (dv == 0) { a0 = s0; a1 = s0 + dashLen * 0.002; if (s0 < u0 || s0 > u1) a1 = a0 - 1; }
                                if (a1 > a0) {
                                    addSeg(a0, a1);
                                    if (--segBudget == 0) return;
                                }
                            }
                            pos = s1;
                            if (pos >= u1) break;
                        }
                    }
                }
            }
        }
    }

    Reader rd_;
    Drawing& model_;
    Drawing& paper_;
    std::vector<Pair> buf_;
    std::unordered_map<std::string, Layer> layers_;
    std::unordered_map<std::string, Block> blocks_;
    std::vector<Entity> pending_;
    bool blocksSeen_ = false;
    std::vector<const Block*> stack_;   // blocks being expanded (cycle guard)
    size_t emitted_ = 0;
    std::string acadVer_;
    int codepage_ = 1252;
    bool utf8_ = false;
};

// Extents with protection against a few stray far-away objects.
BBox ComputeExtents(const Drawing& d) {
    std::vector<BBox> boxes;
    boxes.reserve(d.paths.size() + d.fills.size() + d.texts.size());
    for (const Path& p : d.paths) boxes.push_back(p.box);
    for (const Fill& f : d.fills) boxes.push_back(f.box);
    for (const Text& t : d.texts) boxes.push_back(t.box);
    BBox full;
    for (const BBox& b : boxes) full.add(b);
    if (boxes.size() < 20 || !full.valid()) return full;
    std::vector<double> cx, cy;
    for (const BBox& b : boxes) {
        if (!b.valid()) continue;
        cx.push_back((b.minx + b.maxx) / 2);
        cy.push_back((b.miny + b.maxy) / 2);
    }
    size_t n = cx.size();
    if (n < 20) return full;
    size_t lo = n / 100, hi = n - 1 - n / 100;
    std::nth_element(cx.begin(), cx.begin() + lo, cx.end());
    double xlo = cx[lo];
    std::nth_element(cx.begin(), cx.begin() + hi, cx.end());
    double xhi = cx[hi];
    std::nth_element(cy.begin(), cy.begin() + lo, cy.end());
    double ylo = cy[lo];
    std::nth_element(cy.begin(), cy.begin() + hi, cy.end());
    double yhi = cy[hi];
    double mx = (xhi - xlo) * 0.5, my = (yhi - ylo) * 0.5;
    double m = std::max(mx, my);
    BBox core;
    for (const BBox& b : boxes) {
        if (!b.valid()) continue;
        double bx = (b.minx + b.maxx) / 2, by = (b.miny + b.maxy) / 2;
        if (bx >= xlo - m && bx <= xhi + m && by >= ylo - m && by <= yhi + m) core.add(b);
    }
    if (!core.valid()) return full;
    double eps = std::max(full.width(), full.height()) * 1e-9 + 1e-12;
    double aFull = (full.width() + eps) * (full.height() + eps);
    double aCore = (core.width() + eps) * (core.height() + eps);
    double sFull = std::max(full.width(), full.height()), sCore = std::max(core.width(), core.height());
    if (aFull > 50 * aCore && sFull > 8 * sCore) return core;
    return full;
}

}  // namespace

uint32_t AciToRgb(int aci) {
    static const uint32_t base[10] = {0x000000, 0xFF0000, 0xFFFF00, 0x00FF00, 0x00FFFF,
                                      0x0000FF, 0xFF00FF, kForeground, 0x808080, 0xC0C0C0};
    if (aci <= 0 || aci > 255) return kForeground;
    if (aci < 10) return base[aci];
    if (aci >= 250) {
        static const int g[6] = {0x33, 0x50, 0x69, 0x82, 0xBE, 0xFF};
        int v = g[aci - 250];
        return (uint32_t)((v << 16) | (v << 8) | v);
    }
    static const double levels[5] = {255, 165, 127, 76, 38};
    int hueIdx = aci / 10 - 1;           // 0..23, 15 degrees each
    int sub = aci % 10;
    double V = levels[sub / 2];
    double S = (sub & 1) ? 0.5 : 1.0;
    double h = hueIdx * 15.0 / 60.0;     // 0..6
    int hi = (int)std::floor(h) % 6;
    double f = h - std::floor(h);
    double p = V * (1 - S), q = V * (1 - S * f), t = V * (1 - S * (1 - f));
    double r, g, b;
    switch (hi) {
        case 0: r = V; g = t; b = p; break;
        case 1: r = q; g = V; b = p; break;
        case 2: r = p; g = V; b = t; break;
        case 3: r = p; g = q; b = V; break;
        case 4: r = t; g = p; b = V; break;
        default: r = V; g = p; b = q; break;
    }
    auto c = [](double v) { return (uint32_t)std::max(0.0, std::min(255.0, std::floor(v))); };
    return (c(r) << 16) | (c(g) << 8) | c(b);
}

bool Load(const char* data, size_t size, Drawing& out, std::string* error) {
    out = Drawing();
    if (!data || size == 0) {
        if (error) *error = "empty file";
        return false;
    }
    if (size >= 18 && std::memcmp(data, "AutoCAD Binary DXF", 18) == 0) {
        if (error) *error = "binary DXF is not supported";
        return false;
    }
    Drawing paper;
    try {
        Parser parser(data, size, out, paper);
        if (!parser.Run(error)) return false;
    } catch (const std::bad_alloc&) {
        if (error) *error = "out of memory";
        return false;
    }
    if (out.empty() && !paper.empty()) {
        out = std::move(paper);
        out.paperSpace = true;
    }
    out.extents = ComputeExtents(out);
    return true;
}

}  // namespace dxf
