/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

struct FrameRateWnd {
    FrameRateWnd() = default;
    ~FrameRateWnd();

    bool Create(HWND);

    void ShowFrameRate(int frameRate);
    void ShowFrameRateDur(double durMs);

    HWND hwndAssociatedWith = nullptr;
    HWND hwndAssociatedWithTopLevel = nullptr;

    HWND hwnd = nullptr;
    HFONT font = nullptr;

    SIZE maxSizeSoFar = {0, 0};
    int frameRate = -1;
};

int FrameRateFromDuration(double durMs);
