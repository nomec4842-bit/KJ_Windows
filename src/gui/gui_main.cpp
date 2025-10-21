#include "gui/gui_main.h"
#include "core/audio_engine.h"
#include "core/sequencer.h"
#include "wdl/lice/lice.h"

#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

namespace {

constexpr int kWindowWidth = 800;
constexpr int kWindowHeight = 600;

RECT playButton = {40, 40, 180, 110};
RECT loadSampleButton = {200, 40, 340, 110};
RECT bpmDownButton = {360, 55, 400, 95};
RECT bpmUpButton = {410, 55, 450, 95};
RECT stepCountDownButton = {470, 55, 510, 95};
RECT stepCountUpButton = {520, 55, 560, 95};
RECT pageDownButton = {580, 55, 620, 95};
RECT pageUpButton = {630, 55, 670, 95};
std::array<RECT, kSequencerStepsPerPage> stepRects;
int currentStepPage = 0;

std::unique_ptr<wdl::LICE_SysBitmap> gSurface;

void buildStepRects()
{
    const int startX = 40;
    const int startY = 180;
    const int stepWidth = 35;
    const int stepHeight = stepWidth;
    const int spacing = 10;

    for (int i = 0; i < kSequencerStepsPerPage; ++i)
    {
        RECT rect {};
        rect.left = startX + i * (stepWidth + spacing);
        rect.top = startY;
        rect.right = rect.left + stepWidth;
        rect.bottom = rect.top + stepHeight;
        stepRects[i] = rect;
    }
}

void clampCurrentPage()
{
    int totalSteps = getSequencerStepCount();
    int totalPages = (totalSteps + kSequencerStepsPerPage - 1) / kSequencerStepsPerPage;
    if (totalPages < 1)
        totalPages = 1;
    if (currentStepPage >= totalPages)
        currentStepPage = totalPages - 1;
    if (currentStepPage < 0)
        currentStepPage = 0;
}

bool pointInRect(const RECT& rect, int x, int y)
{
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

void ensureSurfaceSize(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;

    if (!gSurface)
    {
        gSurface = std::make_unique<wdl::LICE_SysBitmap>(width, height);
        return;
    }

    gSurface->resize(width, height);
}

void drawButton(wdl::LICE_SysBitmap& surface, const RECT& rect, COLORREF fill, COLORREF outline, const char* text)
{
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    wdl::LICE_FillRect(&surface, rect.left, rect.top, width, height,
                       wdl::LICE_ColorFromCOLORREF(fill));
    wdl::LICE_DrawRect(&surface, rect.left, rect.top, width, height,
                       wdl::LICE_ColorFromCOLORREF(outline));

    RECT textRect = rect;
    wdl::LICE_DrawText(surface, textRect, text, RGB(230, 230, 230));
}

void drawSequencer(wdl::LICE_SysBitmap& surface)
{
    bool playing = isPlaying.load(std::memory_order_relaxed);
    clampCurrentPage();

    int currentStep = sequencerCurrentStep.load(std::memory_order_relaxed);
    int totalSteps = getSequencerStepCount();

    for (int i = 0; i < kSequencerStepsPerPage; ++i)
    {
        const RECT& rect = stepRects[i];
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        int stepIndex = currentStepPage * kSequencerStepsPerPage + i;
        bool inRange = stepIndex < totalSteps;
        bool active = inRange && sequencerSteps[stepIndex].load(std::memory_order_relaxed);

        COLORREF fill = active ? RGB(0, 120, 200) : RGB(45, 45, 45);
        if (!inRange)
        {
            fill = RGB(30, 30, 30);
        }
        wdl::LICE_FillRect(&surface, rect.left, rect.top, width, height,
                           wdl::LICE_ColorFromCOLORREF(fill));

        COLORREF borderColor = RGB(70, 70, 70);
        int penWidth = 2;
        if (playing && inRange && stepIndex == currentStep)
        {
            borderColor = RGB(255, 215, 0);
            penWidth = 3;
        }

        if (!inRange)
        {
            borderColor = RGB(50, 50, 50);
        }

        for (int p = 0; p < penWidth; ++p)
        {
            wdl::LICE_DrawRect(&surface, rect.left - p, rect.top - p,
                                width + p * 2, height + p * 2,
                                wdl::LICE_ColorFromCOLORREF(borderColor));
        }

        RECT labelRect = rect;
        labelRect.top = rect.bottom - 22;
        labelRect.left += 4;
        std::string label = inRange ? std::to_string(stepIndex + 1) : "-";
        wdl::LICE_DrawText(surface, labelRect, label.c_str(), RGB(220, 220, 220),
                           DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    }
}

void renderUI(wdl::LICE_SysBitmap& surface, const RECT& client)
{
    wdl::LICE_Clear(&surface, wdl::LICE_ColorFromCOLORREF(RGB(20, 20, 20)));

    drawButton(surface, playButton,
               isPlaying.load(std::memory_order_relaxed) ? RGB(0, 150, 0) : RGB(120, 0, 0),
               RGB(30, 30, 30),
               isPlaying.load(std::memory_order_relaxed) ? "Stop" : "Play");

    drawButton(surface, loadSampleButton,
               RGB(50, 50, 50), RGB(120, 120, 120),
               "Load Sample");

    drawButton(surface, bpmDownButton, RGB(50, 50, 50), RGB(120, 120, 120), "-");
    drawButton(surface, bpmUpButton, RGB(50, 50, 50), RGB(120, 120, 120), "+");
    drawButton(surface, stepCountDownButton, RGB(50, 50, 50), RGB(120, 120, 120), "-");
    drawButton(surface, stepCountUpButton, RGB(50, 50, 50), RGB(120, 120, 120), "+");
    drawButton(surface, pageDownButton, RGB(50, 50, 50), RGB(120, 120, 120), "<");
    drawButton(surface, pageUpButton, RGB(50, 50, 50), RGB(120, 120, 120), ">");

    int bpm = sequencerBPM.load(std::memory_order_relaxed);
    std::string bpmText = "Tempo: " + std::to_string(bpm) + " BPM";
    RECT bpmRect {470, 20, client.right - 40, 50};
    wdl::LICE_DrawText(surface, bpmRect, bpmText.c_str(), RGB(220, 220, 220),
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    int totalSteps = getSequencerStepCount();
    std::string stepText = "Steps: " + std::to_string(totalSteps);
    RECT stepRect {470, 95, client.right - 40, 125};
    wdl::LICE_DrawText(surface, stepRect, stepText.c_str(), RGB(220, 220, 220),
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    clampCurrentPage();
    int totalPages = (totalSteps + kSequencerStepsPerPage - 1) / kSequencerStepsPerPage;
    if (totalPages < 1)
        totalPages = 1;
    std::string pageText = "Page: " + std::to_string(currentStepPage + 1) + "/" + std::to_string(totalPages);
    RECT pageRect {470, 130, client.right - 40, 160};
    wdl::LICE_DrawText(surface, pageRect, pageText.c_str(), RGB(220, 220, 220),
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    drawSequencer(surface);
}

} // namespace

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        buildStepRects();
        SetTimer(hwnd, 1, 60, nullptr);
        return 0;
    case WM_LBUTTONDOWN:
    {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        if (pointInRect(playButton, x, y))
        {
            bool playing = isPlaying.load(std::memory_order_relaxed);
            isPlaying.store(!playing, std::memory_order_relaxed);
            requestSequencerReset();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (pointInRect(loadSampleButton, x, y))
        {
            wchar_t fileBuffer[MAX_PATH] = {0};
            OPENFILENAMEW ofn = {0};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"WAV Files\0*.wav\0All Files\0*.*\0";
            ofn.lpstrFile = fileBuffer;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            ofn.lpstrDefExt = L"wav";

            if (GetOpenFileNameW(&ofn))
            {
                std::filesystem::path selectedPath(fileBuffer);
                if (!loadSampleFile(selectedPath))
                {
                    MessageBoxW(hwnd,
                                L"Failed to load selected sample.",
                                L"Load Sample",
                                MB_OK | MB_ICONERROR);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            return 0;
        }

        if (pointInRect(bpmDownButton, x, y))
        {
            int bpm = sequencerBPM.load(std::memory_order_relaxed);
            bpm = std::clamp(bpm - 5, 40, 240);
            sequencerBPM.store(bpm, std::memory_order_relaxed);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (pointInRect(bpmUpButton, x, y))
        {
            int bpm = sequencerBPM.load(std::memory_order_relaxed);
            bpm = std::clamp(bpm + 5, 40, 240);
            sequencerBPM.store(bpm, std::memory_order_relaxed);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (pointInRect(stepCountDownButton, x, y))
        {
            int steps = getSequencerStepCount();
            setSequencerStepCount(steps - 1);
            clampCurrentPage();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (pointInRect(stepCountUpButton, x, y))
        {
            int steps = getSequencerStepCount();
            setSequencerStepCount(steps + 1);
            clampCurrentPage();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (pointInRect(pageDownButton, x, y))
        {
            if (currentStepPage > 0)
            {
                --currentStepPage;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        if (pointInRect(pageUpButton, x, y))
        {
            int totalSteps = getSequencerStepCount();
            int totalPages = (totalSteps + kSequencerStepsPerPage - 1) / kSequencerStepsPerPage;
            if (totalPages < 1)
                totalPages = 1;
            if (currentStepPage < totalPages - 1)
            {
                ++currentStepPage;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        for (int i = 0; i < kSequencerStepsPerPage; ++i)
        {
            if (pointInRect(stepRects[i], x, y))
            {
                int stepIndex = currentStepPage * kSequencerStepsPerPage + i;
                if (stepIndex < getSequencerStepCount())
                {
                    toggleSequencerStep(stepIndex);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
        }

        return 0;
    }
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SIZE:
    {
        int width = LOWORD(lParam);
        int height = HIWORD(lParam);
        ensureSurfaceSize(width, height);
        return 0;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        ensureSurfaceSize(client.right, client.bottom);
        if (gSurface)
        {
            renderUI(*gSurface, client);
            gSurface->blitTo(hdc, 0, 0);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        gSurface.reset();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

void initGUI()
{
    WNDCLASS wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "KJWDLWindow";
    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(0, "KJWDLWindow", "KJ",
                               WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, kWindowWidth, kWindowHeight,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd)
    {
        MessageBox(nullptr, "Window creation failed!", "Error", MB_OK);
        return;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg = {0};
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

