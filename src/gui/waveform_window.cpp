#include "gui/waveform_window.h"

#include "core/audio_engine.h"
#include "gui/gui_main.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

constexpr wchar_t kWaveformWindowClassName[] = L"KJWaveformWindow";
constexpr UINT_PTR kWaveformRefreshTimerId = 1;
constexpr UINT kWaveformRefreshIntervalMs = 33;
constexpr COLORREF kBackgroundColor = RGB(18, 18, 18);
constexpr COLORREF kAxisColor = RGB(70, 70, 70);
constexpr COLORREF kWaveformColor = RGB(0, 200, 255);
constexpr int kDefaultWaveformWidth = 640;
constexpr int kDefaultWaveformHeight = 240;

HWND gWaveformWindow = nullptr;
bool gWaveformWindowClassRegistered = false;

void drawWaveform(HDC hdc, const RECT& rect)
{
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0)
        return;

    HBRUSH backgroundBrush = CreateSolidBrush(kBackgroundColor);
    FillRect(hdc, &rect, backgroundBrush);
    DeleteObject(backgroundBrush);

    int midY = rect.top + height / 2;
    HPEN axisPen = CreatePen(PS_SOLID, 1, kAxisColor);
    HGDIOBJ oldPen = SelectObject(hdc, axisPen);
    MoveToEx(hdc, rect.left, midY, nullptr);
    LineTo(hdc, rect.right, midY);
    SelectObject(hdc, oldPen);
    DeleteObject(axisPen);

    std::size_t sampleCount = static_cast<std::size_t>(std::max(1, width));
    std::vector<float> samples = getMasterWaveformSnapshot(sampleCount);
    if (samples.empty())
        return;

    HPEN waveformPen = CreatePen(PS_SOLID, 2, kWaveformColor);
    oldPen = SelectObject(hdc, waveformPen);

    std::vector<POINT> points(samples.size());
    int amplitude = std::max(1, height / 2 - 8);

    if (samples.size() == 1)
    {
        int x = rect.left + width / 2;
        int y = midY - static_cast<int>(std::clamp(samples.front(), -1.0f, 1.0f) * amplitude);
        points[0] = {x, y};
    }
    else
    {
        for (std::size_t i = 0; i < samples.size(); ++i)
        {
            float clamped = std::clamp(samples[i], -1.0f, 1.0f);
            int x = rect.left + static_cast<int>((static_cast<long long>(i) * (width - 1)) / (static_cast<long long>(samples.size() - 1)));
            int y = midY - static_cast<int>(clamped * amplitude);
            points[i].x = x;
            points[i].y = y;
        }
    }

    if (points.size() == 1)
    {
        SetPixelV(hdc, points[0].x, points[0].y, kWaveformColor);
    }
    else
    {
        Polyline(hdc, points.data(), static_cast<int>(points.size()));
    }

    SelectObject(hdc, oldPen);
    DeleteObject(waveformPen);
}

LRESULT CALLBACK WaveformWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        SetTimer(hwnd, kWaveformRefreshTimerId, kWaveformRefreshIntervalMs, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == kWaveformRefreshTimerId)
        {
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect;
        GetClientRect(hwnd, &rect);

        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        if (width <= 0 || height <= 0)
        {
            EndPaint(hwnd, &ps);
            return 0;
        }

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBitmap = CreateCompatibleBitmap(hdc, width, height);
        HGDIOBJ oldBitmap = SelectObject(memDC, memBitmap);

        drawWaveform(memDC, rect);

        BitBlt(hdc, 0, 0, width, height, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBitmap);
        DeleteObject(memBitmap);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kWaveformRefreshTimerId);
        if (hwnd == gWaveformWindow)
        {
            gWaveformWindow = nullptr;
            requestMainMenuRefresh();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ensureWaveformWindowClass()
{
    if (gWaveformWindowClassRegistered)
        return;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WaveformWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kWaveformWindowClassName;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (RegisterClassW(&wc))
    {
        gWaveformWindowClassRegistered = true;
    }
}

}

void toggleWaveformWindow(HWND parent)
{
    if (gWaveformWindow && IsWindow(gWaveformWindow))
    {
        closeWaveformWindow();
        return;
    }

    ensureWaveformWindowClass();
    if (!gWaveformWindowClassRegistered)
        return;

    RECT parentRect {0, 0, 0, 0};
    if (parent && IsWindow(parent))
    {
        GetWindowRect(parent, &parentRect);
    }

    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if (parentRect.right > parentRect.left && parentRect.bottom > parentRect.top)
    {
        x = parentRect.left + 60;
        y = parentRect.top + 60;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW,
                                kWaveformWindowClassName,
                                L"Waveform Visualizer",
                                WS_OVERLAPPEDWINDOW,
                                x,
                                y,
                                kDefaultWaveformWidth,
                                kDefaultWaveformHeight,
                                parent,
                                nullptr,
                                GetModuleHandle(nullptr),
                                nullptr);
    if (hwnd)
    {
        gWaveformWindow = hwnd;
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        requestMainMenuRefresh();
    }
}

void closeWaveformWindow()
{
    if (gWaveformWindow && IsWindow(gWaveformWindow))
    {
        DestroyWindow(gWaveformWindow);
        gWaveformWindow = nullptr;
    }
}

bool isWaveformWindowOpen()
{
    return gWaveformWindow && IsWindow(gWaveformWindow);
}
