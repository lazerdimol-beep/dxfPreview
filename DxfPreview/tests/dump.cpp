// Linux test harness: loads a DXF and dumps geometry as text for tests/render.py
#include "../src/dxf_core.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<char> d((std::istreambuf_iterator<char>(f)), {});
    dxf::Drawing dw; std::string err;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = dxf::Load(d.data(), d.size(), dw, &err);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    fprintf(stderr, "ok=%d err=%s paths=%zu fills=%zu texts=%zu points=%zu paper=%d trunc=%d time=%.1fms\n", ok, err.c_str(),
            dw.paths.size(), dw.fills.size(), dw.texts.size(), dw.points.size(), dw.paperSpace, dw.truncated, ms);
    if (!ok) return 1;
    printf("E %.9g %.9g %.9g %.9g\n", dw.extents.minx, dw.extents.miny, dw.extents.maxx, dw.extents.maxy);
    if (argc > 2) return 0;
    for (auto& p : dw.paths) {
        printf("P %u %d", p.color, p.closed);
        for (uint32_t i = 0; i < p.count; ++i) printf(" %.9g %.9g", dw.points[p.first + i].x, dw.points[p.first + i].y);
        printf("\n");
    }
    for (auto& fl : dw.fills) {
        printf("F %u %d", fl.color, fl.alpha);
        for (uint32_t r = 0; r < fl.ringCount; ++r) {
            auto& rg = dw.rings[fl.firstRing + r];
            printf(" |");
            for (uint32_t i = 0; i < rg.count; ++i) printf(" %.9g %.9g", dw.points[rg.first + i].x, dw.points[rg.first + i].y);
        }
        printf("\n");
    }
    for (auto& t : dw.texts) {
        printf("T %u %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %d %d %d", t.color, t.pos.x, t.pos.y, t.height, t.rotation,
               t.widthFactor, t.fitLength, t.baseY, t.lineStep, t.hAlign, t.flipX, t.flipY);
        for (auto& l : t.lines) printf("\t%s", l.c_str());
        printf("\n");
    }
}
