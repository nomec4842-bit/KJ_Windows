#include "gui/gui_main.h"
#include "core/audio_engine.h"
#include "core/sequencer.h"
#include <windows.h>

#include <array>
#include <algorithm>
#include <string>
#include <atomic>

namespace {

RECT playButton = {40, 40, 180, 110};
RECT bpmDownButton = {220, 55, 260, 95};
RECT bpmUpButton = {320, 55, 360, 95};
std::array<RECT, kNumSequencerSteps> stepRects;

void buildStepRects() {
    const int startX = 40;
    const int startY = 180;
    const int stepWidth = 35;
    const int stepHeight = 160;
    const int spacing = 10;

    for (int i = 0; i < kNumSequencerSteps; ++i) {
        RECT rect = {};
        rect.left = startX + i * (stepWidth + spacing);
        rect.top = startY;
        rect.right = rect.left + stepWidth;
        rect.bottom = rect.top + stepHeight;
        stepRects[i] = rect;
    }
}

bool pointInRect(const RECT& rect, int x, int y) {
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

void drawButton(HDC dc, const RECT& rect, COLORREF fill, COLORREF outline, const char* text) {
    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);

    HPEN pen = CreatePen(PS_SOLID, 2, outline);
    HPEN oldPen = (HPEN)SelectObject(dc, pen);
    HBRUSH oldBrush = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);

    SetTextColor(dc, RGB(230, 230, 230));
    SetBkMode(dc, TRANSPARENT);
    RECT textRect = rect;
    DrawTextA(dc, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE:
            buildStepRects();
            SetTimer(hwnd, 1, 60, NULL);
            return 0;
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            if (pointInRect(playButton, x, y)) {
                bool playing = isPlaying.load(std::memory_order_relaxed);
                isPlaying.store(!playing, std::memory_order_relaxed);
                requestSequencerReset();
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            if (pointInRect(bpmDownButton, x, y)) {
                int bpm = sequencerBPM.load(std::memory_order_relaxed);
                bpm = std::clamp(bpm - 5, 40, 240);
                sequencerBPM.store(bpm, std::memory_order_relaxed);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            if (pointInRect(bpmUpButton, x, y)) {
                int bpm = sequencerBPM.load(std::memory_order_relaxed);
                bpm = std::clamp(bpm + 5, 40, 240);
                sequencerBPM.store(bpm, std::memory_order_relaxed);
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }

            for (int i = 0; i < kNumSequencerSteps; ++i) {
                if (pointInRect(stepRects[i], x, y)) {
                    toggleSequencerStep(i);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }

            return 0;
        }
        case WM_TIMER:
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            HDC memDC = CreateCompatibleDC(hdc);
            RECT client;
            GetClientRect(hwnd, &client);
            HBITMAP memBM = CreateCompatibleBitmap(hdc, client.right, client.bottom);
            SelectObject(memDC, memBM);

            HBRUSH bg = CreateSolidBrush(RGB(20,20,20));
            FillRect(memDC, &client, bg);
            DeleteObject(bg);

            drawButton(memDC, playButton,
                       isPlaying.load(std::memory_order_relaxed) ? RGB(0, 150, 0) : RGB(120, 0, 0),
                       RGB(30, 30, 30),
                       isPlaying.load(std::memory_order_relaxed) ? "Stop" : "Play");

            drawButton(memDC, bpmDownButton, RGB(50, 50, 50), RGB(120, 120, 120), "-");
            drawButton(memDC, bpmUpButton, RGB(50, 50, 50), RGB(120, 120, 120), "+");

            SetTextColor(memDC, RGB(220, 220, 220));
            SetBkMode(memDC, TRANSPARENT);
            int bpm = sequencerBPM.load(std::memory_order_relaxed);
            std::string bpmText = "Tempo: " + std::to_string(bpm) + " BPM";
            TextOutA(memDC, 380, 65, bpmText.c_str(), static_cast<int>(bpmText.size()));

            bool playing = isPlaying.load(std::memory_order_relaxed);
            int currentStep = sequencerCurrentStep.load(std::memory_order_relaxed);

            for (int i = 0; i < kNumSequencerSteps; ++i) {
                RECT rect = stepRects[i];
                bool active = sequencerSteps[i].load(std::memory_order_relaxed);
                COLORREF fillColor = active ? RGB(0, 120, 200) : RGB(45, 45, 45);
                HBRUSH stepBrush = CreateSolidBrush(fillColor);
                FillRect(memDC, &rect, stepBrush);
                DeleteObject(stepBrush);

                COLORREF borderColor = RGB(70, 70, 70);
                int penWidth = 2;
                if (playing && i == currentStep) {
                    borderColor = RGB(255, 215, 0);
                    penWidth = 3;
                }

                HPEN pen = CreatePen(PS_SOLID, penWidth, borderColor);
                HPEN oldPen = (HPEN)SelectObject(memDC, pen);
                HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH));
                Rectangle(memDC, rect.left, rect.top, rect.right, rect.bottom);
                SelectObject(memDC, oldBrush);
                SelectObject(memDC, oldPen);
                DeleteObject(pen);

                std::string label = std::to_string(i + 1);
                TextOutA(memDC, rect.left + 8, rect.bottom - 20, label.c_str(), static_cast<int>(label.size()));
            }

            BitBlt(hdc, 0, 0, client.right, client.bottom, memDC, 0, 0, SRCCOPY);
            DeleteObject(memBM);
            DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void initGUI() {
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "KJWin32Class";
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, "KJWin32Class", "KJ",
                               WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
                               NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) {
        MessageBox(NULL, "Window creation failed!", "Error", MB_OK);
        return;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}
