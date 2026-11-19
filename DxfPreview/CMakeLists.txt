# Alternative to build.bat: cmake -B build -A x64 && cmake --build build --config Release
cmake_minimum_required(VERSION 3.16)
project(DxfPreview CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options(/utf-8)
add_compile_definitions(UNICODE _UNICODE NOMINMAX)
set(LIBS gdiplus shlwapi ole32 oleaut32 uuid advapi32 user32 gdi32 shell32)

add_library(DxfPreview SHARED
    src/dxf_core.cpp src/render_gdiplus.cpp src/dll_main.cpp
    src/thumbnail_provider.cpp src/preview_handler.cpp src/DxfPreview.def)
target_link_libraries(DxfPreview PRIVATE ${LIBS})

add_executable(dxfthumb tools/dxfthumb.cpp src/dxf_core.cpp src/render_gdiplus.cpp)
target_link_libraries(dxfthumb PRIVATE ${LIBS})
