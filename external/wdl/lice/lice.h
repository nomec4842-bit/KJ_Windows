#pragma once

#include <windows.h>
#include <cstdint>

namespace wdl {

using LICE_pixel = uint32_t;

inline LICE_pixel LICE_RGBA(int r, int g, int b, int a = 255)
{
    return (static_cast<LICE_pixel>(a & 0xFF) << 24) |
           (static_cast<LICE_pixel>(r & 0xFF) << 16) |
           (static_cast<LICE_pixel>(g & 0xFF) << 8) |
           static_cast<LICE_pixel>(b & 0xFF);
}

inline LICE_pixel LICE_ColorFromCOLORREF(COLORREF c, int alpha = 255)
{
    return LICE_RGBA(GetRValue(c), GetGValue(c), GetBValue(c), alpha);
}

class LICE_IBitmap
{
public:
    virtual ~LICE_IBitmap() = default;
    virtual LICE_pixel* getBits() = 0;
    virtual const LICE_pixel* getBits() const = 0;
    virtual int getWidth() const = 0;
    virtual int getHeight() const = 0;
    virtual int getRowSpan() const = 0;
};

class LICE_SysBitmap final : public LICE_IBitmap
{
public:
    LICE_SysBitmap(int width, int height);
    ~LICE_SysBitmap() override;

    LICE_SysBitmap(const LICE_SysBitmap&) = delete;
    LICE_SysBitmap& operator=(const LICE_SysBitmap&) = delete;

    LICE_pixel* getBits() override { return m_bits; }
    const LICE_pixel* getBits() const override { return m_bits; }
    int getWidth() const override { return m_width; }
    int getHeight() const override { return m_height; }
    int getRowSpan() const override { return m_stride; }

    HDC getDC() const { return m_dc; }
    void resize(int width, int height);
    void blitTo(HDC destDC, int x, int y) const;

private:
    void allocate(int width, int height);
    void release();

    HBITMAP m_bitmap {};
    HBITMAP m_oldBitmap {};
    HDC m_dc {};
    BITMAPINFO m_bmi {};
    LICE_pixel* m_bits {};
    int m_width {};
    int m_height {};
    int m_stride {};
};

enum class LICE_BlitMode
{
    Copy
};

void LICE_Clear(LICE_IBitmap* dest, LICE_pixel color);
void LICE_FillRect(LICE_IBitmap* dest, int x, int y, int w, int h, LICE_pixel color,
                   float alpha = 1.0f, LICE_BlitMode mode = LICE_BlitMode::Copy);
void LICE_DrawRect(LICE_IBitmap* dest, int x, int y, int w, int h, LICE_pixel color,
                   float alpha = 1.0f, bool aa = false);
void LICE_DrawText(LICE_SysBitmap& dest, const RECT& rect, const char* text, COLORREF color,
                   UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE);

} // namespace wdl

