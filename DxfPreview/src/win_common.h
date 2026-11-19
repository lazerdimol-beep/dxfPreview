// Common Windows includes and shared declarations.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <thumbcache.h>

#include <atomic>
#include <string>
#include <vector>

// {A8A09E8F-F106-48AA-9FB8-8AA5180F15E5}
inline const CLSID CLSID_DxfThumbnailProvider = {0xa8a09e8f, 0xf106, 0x48aa, {0x9f, 0xb8, 0x8a, 0xa5, 0x18, 0x0f, 0x15, 0xe5}};
// {3BCD80D7-E025-414F-91DB-0BAC8D6E17BC}
inline const CLSID CLSID_DxfPreviewHandler = {0x3bcd80d7, 0xe025, 0x414f, {0x91, 0xdb, 0x0b, 0xac, 0x8d, 0x6e, 0x17, 0xbc}};

#define DXF_THUMB_CLSID_STR L"{A8A09E8F-F106-48AA-9FB8-8AA5180F15E5}"
#define DXF_PREVIEW_CLSID_STR L"{3BCD80D7-E025-414F-91DB-0BAC8D6E17BC}"

extern HINSTANCE g_hInst;
extern std::atomic<long> g_dllRef;

HRESULT DxfThumbnailProvider_Create(REFIID riid, void** ppv);
HRESULT DxfPreviewHandler_Create(REFIID riid, void** ppv);

// Reads a whole IStream into memory (up to maxBytes).
HRESULT ReadStreamAll(IStream* stream, std::vector<char>& out, unsigned long long maxBytes);
