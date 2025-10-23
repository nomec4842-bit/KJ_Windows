/*
  WDL - lice.h
  Copyright (C) 2005-2015 Cockos Incorporated
  http://www.cockos.com/wdl/

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/

#ifndef WDL_LICE_LICE_H
#define WDL_LICE_LICE_H

#include <windows.h>
#include <cstddef>
#include <cstdint>

typedef unsigned int LICE_pixel;
typedef unsigned char LICE_pixel_chan;

#define LICE_RGBA(r, g, b, a) (((b) & 0xff) | (((g) & 0xff) << 8) | (((r) & 0xff) << 16) | (((a) & 0xff) << 24))
#define LICE_GETB(v) ((v) & 0xff)
#define LICE_GETG(v) (((v) >> 8) & 0xff)
#define LICE_GETR(v) (((v) >> 16) & 0xff)
#define LICE_GETA(v) (((v) >> 24) & 0xff)

#define LICE_BLIT_MODE_MASK 0xff
#define LICE_BLIT_MODE_COPY 0

class LICE_IBitmap
{
public:
    virtual ~LICE_IBitmap() = default;

    virtual LICE_pixel* getBits() = 0;
    virtual int getWidth() = 0;
    virtual int getHeight() = 0;
    virtual int getRowSpan() = 0;
    virtual bool isFlipped() { return false; }
    virtual bool resize(int w, int h) = 0;
    virtual HDC getDC() { return nullptr; }
};

class LICE_SysBitmap : public LICE_IBitmap
{
public:
    explicit LICE_SysBitmap(int w = 0, int h = 0);
    ~LICE_SysBitmap() override;

    LICE_SysBitmap(const LICE_SysBitmap&) = delete;
    LICE_SysBitmap& operator=(const LICE_SysBitmap&) = delete;

    LICE_pixel* getBits() override { return m_bits; }
    int getWidth() override { return m_width; }
    int getHeight() override { return m_height; }
    int getRowSpan() override { return m_rowSpan; }
    bool resize(int w, int h) override { return resizeInternal(w, h); }
    HDC getDC() override { return m_dc; }

private:
    bool resizeInternal(int w, int h);
    void release();

    int m_width = 0;
    int m_height = 0;
    int m_rowSpan = 0;

    HDC m_dc = nullptr;
    LICE_pixel* m_bits = nullptr;
    HBITMAP m_bitmap = nullptr;
    HGDIOBJ m_oldBitmap = nullptr;
};

void LICE_Clear(LICE_IBitmap* dest, LICE_pixel color, float alpha = 1.0f, int mode = LICE_BLIT_MODE_COPY);
void LICE_FillRect(LICE_IBitmap* dest, int x, int y, int w, int h, LICE_pixel color, float alpha = 1.0f, int mode = LICE_BLIT_MODE_COPY);
void LICE_DrawRect(LICE_IBitmap* dest, int x, int y, int w, int h, LICE_pixel color, float alpha = 1.0f, int mode = LICE_BLIT_MODE_COPY);
void LICE_DrawText(LICE_IBitmap* bm, int x, int y, const char* string, LICE_pixel color, float alpha = 1.0f, int mode = LICE_BLIT_MODE_COPY);
void LICE_MeasureText(const char* string, int* w, int* h);

#define LICE_Scale_BitBlt(hdc, x, y, w, h, src, sx, sy, mode) StretchBlt((hdc), (x), (y), (w), (h), (src)->getDC(), (sx), (sy), (w), (h), (mode))

#endif // WDL_LICE_LICE_H
