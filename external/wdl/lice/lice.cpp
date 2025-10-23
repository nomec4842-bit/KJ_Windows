/*
  WDL - lice.cpp (minimal implementation)
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

#include "lice.h"

#include <algorithm>

namespace {
HFONT GetDefaultFont()
{
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (!font)
        font = static_cast<HFONT>(GetStockObject(SYSTEM_FONT));
    return font;
}
} // namespace

LICE_SysBitmap::LICE_SysBitmap(int w, int h)
{
    resizeInternal(w, h);
}

LICE_SysBitmap::~LICE_SysBitmap()
{
    release();
}

void LICE_SysBitmap::release()
{
    if (m_dc && m_bitmap)
    {
        SelectObject(m_dc, m_oldBitmap);
        DeleteObject(m_bitmap);
        m_bitmap = nullptr;
    }

    if (m_dc)
    {
        DeleteDC(m_dc);
        m_dc = nullptr;
    }

    m_bits = nullptr;
    m_width = 0;
    m_height = 0;
    m_rowSpan = 0;
    m_oldBitmap = nullptr;
}

bool LICE_SysBitmap::resizeInternal(int w, int h)
{
    if (w == m_width && h == m_height && m_bitmap)
        return false;

    if (w <= 0 || h <= 0)
    {
        release();
        return true;
    }

    if (!m_dc)
    {
        m_dc = CreateCompatibleDC(nullptr);
        if (!m_dc)
            return false;
    }

    BITMAPINFO bmi {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP newBitmap = CreateDIBSection(m_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!newBitmap || !bits)
        return false;

    if (!m_oldBitmap)
    {
        m_oldBitmap = SelectObject(m_dc, newBitmap);
        if (!m_oldBitmap)
        {
            DeleteObject(newBitmap);
            return false;
        }
    }
    else
    {
        SelectObject(m_dc, m_oldBitmap);
        if (m_bitmap)
            DeleteObject(m_bitmap);
        if (!SelectObject(m_dc, newBitmap))
        {
            DeleteObject(newBitmap);
            m_bitmap = nullptr;
            m_bits = nullptr;
            m_width = 0;
            m_height = 0;
            m_rowSpan = 0;
            return false;
        }
    }

    m_bitmap = newBitmap;
    m_bits = static_cast<LICE_pixel*>(bits);
    m_width = w;
    m_height = h;
    m_rowSpan = w;

    const size_t totalPixels = static_cast<size_t>(m_rowSpan) * static_cast<size_t>(m_height);
    std::fill(m_bits, m_bits + totalPixels, 0);

    HFONT font = GetDefaultFont();
    if (font)
        SelectObject(m_dc, font);

    return true;
}

static void FillSpan(LICE_pixel* row, int start, int end, LICE_pixel color)
{
    for (int x = start; x < end; ++x)
        row[x] = color;
}

void LICE_Clear(LICE_IBitmap* dest, LICE_pixel color, float, int)
{
    if (!dest)
        return;
    LICE_pixel* bits = dest->getBits();
    if (!bits)
        return;

    const int width = dest->getWidth();
    const int height = dest->getHeight();
    const int span = dest->getRowSpan();

    for (int y = 0; y < height; ++y)
    {
        LICE_pixel* row = bits + static_cast<size_t>(span) * y;
        std::fill(row, row + width, color);
    }
}

void LICE_FillRect(LICE_IBitmap* dest, int x, int y, int w, int h, LICE_pixel color, float, int)
{
    if (!dest || w <= 0 || h <= 0)
        return;

    const int bmpWidth = dest->getWidth();
    const int bmpHeight = dest->getHeight();
    const int span = dest->getRowSpan();
    LICE_pixel* bits = dest->getBits();
    if (!bits)
        return;

    int left = std::max(0, x);
    int top = std::max(0, y);
    int right = std::min(x + w, bmpWidth);
    int bottom = std::min(y + h, bmpHeight);
    if (left >= right || top >= bottom)
        return;

    for (int py = top; py < bottom; ++py)
    {
        LICE_pixel* row = bits + static_cast<size_t>(span) * py;
        FillSpan(row, left, right, color);
    }
}

void LICE_DrawRect(LICE_IBitmap* dest, int x, int y, int w, int h, LICE_pixel color, float, int)
{
    if (!dest || w <= 0 || h <= 0)
        return;

    const int bmpWidth = dest->getWidth();
    const int bmpHeight = dest->getHeight();
    const int span = dest->getRowSpan();
    LICE_pixel* bits = dest->getBits();
    if (!bits)
        return;

    int left = std::max(0, x);
    int top = std::max(0, y);
    int right = std::min(x + w, bmpWidth);
    int bottom = std::min(y + h, bmpHeight);
    if (left >= right || top >= bottom)
        return;

    // top and bottom edges
    LICE_pixel* topRow = bits + static_cast<size_t>(span) * top;
    FillSpan(topRow, left, right, color);

    if (bottom - 1 != top)
    {
        LICE_pixel* bottomRow = bits + static_cast<size_t>(span) * (bottom - 1);
        FillSpan(bottomRow, left, right, color);
    }

    for (int py = top; py < bottom; ++py)
    {
        LICE_pixel* row = bits + static_cast<size_t>(span) * py;
        row[left] = color;
        if (right - 1 != left)
            row[right - 1] = color;
    }
}

namespace {
HDC GetMeasureDC()
{
    static HDC s_measureDC = []() -> HDC {
        HDC dc = CreateCompatibleDC(nullptr);
        if (dc)
        {
            HFONT font = GetDefaultFont();
            if (font)
                SelectObject(dc, font);
        }
        return dc;
    }();
    return s_measureDC;
}
} // namespace

void LICE_MeasureText(const char* string, int* w, int* h)
{
    if (w)
        *w = 0;
    if (h)
        *h = 0;

    if (!string || !*string)
        return;

    HDC dc = GetMeasureDC();
    if (!dc)
        return;

    TEXTMETRIC tm {};
    GetTextMetrics(dc, &tm);
    const int lineHeight = tm.tmHeight;

    int maxWidth = 0;
    int totalHeight = 0;

    const char* lineStart = string;
    const char* cursor = string;
    while (true)
    {
        if (*cursor == '\n' || *cursor == '\0')
        {
            int lineWidth = 0;
            int lineHeightUsed = lineHeight;
            if (cursor > lineStart)
            {
                SIZE size {};
                if (GetTextExtentPoint32A(dc, lineStart, static_cast<int>(cursor - lineStart), &size))
                {
                    lineWidth = size.cx;
                    lineHeightUsed = size.cy;
                }
            }
            maxWidth = std::max(maxWidth, lineWidth);
            totalHeight += lineHeightUsed;

            if (*cursor == '\0')
                break;

            lineStart = cursor + 1;
        }
        ++cursor;
    }

    if (w)
        *w = maxWidth;
    if (h)
        *h = totalHeight;
}

void LICE_DrawText(LICE_IBitmap* bm, int x, int y, const char* string, LICE_pixel color, float, int)
{
    if (!bm || !string || !*string)
        return;

    HDC hdc = bm->getDC();
    if (!hdc)
        return;

    HFONT font = GetDefaultFont();
    HFONT oldFont = nullptr;
    if (font)
        oldFont = static_cast<HFONT>(SelectObject(hdc, font));

    const COLORREF textColor = RGB(LICE_GETR(color), LICE_GETG(color), LICE_GETB(color));
    const COLORREF oldColor = SetTextColor(hdc, textColor);
    const int oldBkMode = SetBkMode(hdc, TRANSPARENT);

    TEXTMETRIC tm {};
    GetTextMetrics(hdc, &tm);
    int lineHeight = tm.tmHeight;

    int currentY = y;
    const char* lineStart = string;
    const char* cursor = string;
    while (true)
    {
        if (*cursor == '\n' || *cursor == '\0')
        {
            int len = static_cast<int>(cursor - lineStart);
            if (len > 0)
                TextOutA(hdc, x, currentY, lineStart, len);

            currentY += lineHeight;
            if (*cursor == '\0')
                break;
            lineStart = cursor + 1;
        }
        ++cursor;
    }

    SetBkMode(hdc, oldBkMode);
    SetTextColor(hdc, oldColor);
    if (oldFont)
        SelectObject(hdc, oldFont);
}
