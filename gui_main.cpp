#include "gui/gui_main.h"
#include "core/audio_engine.h"
#include <windows.h>

RECT button = {300, 250, 500, 350};

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            int x = LOWORD(lParam);
            int y = HIWORD(lParam);
            if (x >= button.left && x <= button.right && y >= button.top && y <= button.bottom) {
                isPlaying = !isPlaying;
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            HDC memDC = CreateCompatibleDC(hdc);
            RECT client;
            GetClientRect(hwnd, &client);
            HBITMAP memBM = CreateCompatibleBitmap(hdc, client.right, client.bottom);
            SelectObject(memDC, memBM);

            HBRUSH bg = CreateSolidBrush(RGB(30,30,30)); // dark gray
            FillRect(memDC, &client, bg);
            DeleteObject(bg);

            HBRUSH brush = CreateSolidBrush(isPlaying ? RGB(0,200,0) : RGB(200,0,0));
            FillRect(memDC, &button, brush);
            DeleteObject(brush);

            BitBlt(hdc, 0, 0, client.right, client.bottom, memDC, 0, 0, SRCCOPY);
            DeleteObject(memBM);
            DeleteDC(memDC);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DESTROY:
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