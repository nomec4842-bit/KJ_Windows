#include "lice.h"

#include <algorithm>

namespace wdl {
namespace {

void alphaBlendPixel(LICE_pixel& dest, LICE_pixel src, float alpha)
{
    if (alpha <= 0.0f)
        return;
    if (alpha >= 1.0f)
    {
        dest = src;
        return;
    }

    const int srcA = static_cast<int>((src >> 24) & 0xFF);
    const int srcR = static_cast<int>((src >> 16) & 0xFF);
    const int srcG = static_cast<int>((src >> 8) & 0xFF);
    const int srcB = static_cast<int>(src & 0xFF);

    int destA = static_cast<int>((dest >> 24) & 0xFF);
    int destR = static_cast<int>((dest >> 16) & 0xFF);
    int destG = static_cast<int>((dest >> 8) & 0xFF);
    int destB = static_cast<int>(dest & 0xFF);

    const float srcAlpha = (static_cast<float>(srcA) / 255.0f) * alpha;
    const float invAlpha = 1.0f - srcAlpha;

    destA = static_cast<int>(srcAlpha * 255.0f + invAlpha * destA + 0.5f);
    destR = static_cast<int>(srcAlpha * srcR + invAlpha * destR + 0.5f);
    destG = static_cast<int>(srcAlpha * srcG + invAlpha * destG + 0.5f);
    destB = static_cast<int>(srcAlpha * srcB + invAlpha * destB + 0.5f);

    dest = (static_cast<LICE_pixel>(destA & 0xFF) << 24) |
           (static_cast<LICE_pixel>(destR & 0xFF) << 16) |
           (static_cast<LICE_pixel>(destG & 0xFF) << 8) |
           static_cast<LICE_pixel>(destB & 0xFF);
}

} // namespace

LICE_SysBitmap::LICE_SysBitmap(int width, int height)
{
    allocate(width, height);
}

LICE_SysBitmap::~LICE_SysBitmap()
{
    release();
}

void LICE_SysBitmap::allocate(int width, int height)
{
    release();

    m_dc = CreateCompatibleDC(nullptr);

    std::fill(reinterpret_cast<char*>(&m_bmi), reinterpret_cast<char*>(&m_bmi) + sizeof(m_bmi), 0);
    m_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    m_bmi.bmiHeader.biWidth = width;
    m_bmi.bmiHeader.biHeight = -height; // top-down
    m_bmi.bmiHeader.biPlanes = 1;
    m_bmi.bmiHeader.biBitCount = 32;
    m_bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    m_bitmap = CreateDIBSection(m_dc, &m_bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    m_bits = static_cast<LICE_pixel*>(bits);

    m_oldBitmap = static_cast<HBITMAP>(SelectObject(m_dc, m_bitmap));

    m_width = width;
    m_height = height;
    m_stride = width;
}

void LICE_SysBitmap::release()
{
    if (m_dc && m_bitmap)
    {
        SelectObject(m_dc, m_oldBitmap);
    }

    if (m_bitmap)
    {
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
    m_stride = 0;
    m_oldBitmap = nullptr;
    std::fill(reinterpret_cast<char*>(&m_bmi), reinterpret_cast<char*>(&m_bmi) + sizeof(m_bmi), 0);
}

void LICE_SysBitmap::resize(int width, int height)
{
    if (width == m_width && height == m_height)
        return;

    allocate(width, height);
}

void LICE_SysBitmap::blitTo(HDC destDC, int x, int y) const
{
    BitBlt(destDC, x, y, m_width, m_height, m_dc, 0, 0, SRCCOPY);
}

void LICE_Clear(LICE_IBitmap* dest, LICE_pixel color)
{
    if (!dest)
        return;

    const int height = dest->getHeight();
    const int width = dest->getWidth();
    LICE_pixel* bits = dest->getBits();

    for (int y = 0; y < height; ++y)
    {
        LICE_pixel* row = bits + y * dest->getRowSpan();
        std::fill(row, row + width, color);
    }
}

void LICE_FillRect(LICE_IBitmap* dest, int x, int y, int w, int h, LICE_pixel color,
                   float alpha, LICE_BlitMode)
{
    if (!dest || w <= 0 || h <= 0)
        return;

    const int width = dest->getWidth();
    const int height = dest->getHeight();

    int x0 = std::max(0, x);
    int y0 = std::max(0, y);
    int x1 = std::min(width, x + w);
    int y1 = std::min(height, y + h);

    if (x1 <= x0 || y1 <= y0)
        return;

    LICE_pixel* bits = dest->getBits();
    for (int yy = y0; yy < y1; ++yy)
    {
        LICE_pixel* row = bits + yy * dest->getRowSpan();
        for (int xx = x0; xx < x1; ++xx)
        {
            alphaBlendPixel(row[xx], color, alpha);
        }
    }
}

void LICE_DrawRect(LICE_IBitmap* dest, int x, int y, int w, int h, LICE_pixel color,
                   float alpha, bool)
{
    if (!dest || w <= 0 || h <= 0)
        return;

    LICE_FillRect(dest, x, y, w, 1, color, alpha, LICE_BlitMode::Copy);
    LICE_FillRect(dest, x, y + h - 1, w, 1, color, alpha, LICE_BlitMode::Copy);
    LICE_FillRect(dest, x, y, 1, h, color, alpha, LICE_BlitMode::Copy);
    LICE_FillRect(dest, x + w - 1, y, 1, h, color, alpha, LICE_BlitMode::Copy);
}

void LICE_DrawText(LICE_SysBitmap& dest, const RECT& rect, const char* text, COLORREF color, UINT format)
{
    HDC dc = dest.getDC();
    if (!dc)
        return;

    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    DrawTextA(dc, text, -1, const_cast<RECT*>(&rect), format);
}

} // namespace wdl

