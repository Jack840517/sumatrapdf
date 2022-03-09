/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

struct ScopedCritSec {
    CRITICAL_SECTION* cs{nullptr};

    explicit ScopedCritSec(CRITICAL_SECTION* cs) : cs(cs) {
        EnterCriticalSection(cs);
    }
    ~ScopedCritSec() {
        LeaveCriticalSection(cs);
    }
};

class AutoCloseHandle {
    HANDLE handle{nullptr};

  public:
    AutoCloseHandle() = default;

    AutoCloseHandle(HANDLE h) { // NOLINT
        handle = h;
    }

    ~AutoCloseHandle() {
        if (IsValid()) {
            CloseHandle(handle);
        }
    }

    AutoCloseHandle& operator=(HANDLE h) {
        CrashIf(handle != nullptr);
        CrashIf(h == nullptr);
        handle = h;
        return *this;
    }

    [[nodiscard]] operator HANDLE() const { // NOLINT
        return handle;
    }

    [[nodiscard]] bool IsValid() const {
        return handle != nullptr && handle != INVALID_HANDLE_VALUE;
    }
};

template <class T>
class ScopedComPtr {
  protected:
    T* ptr{nullptr};

  public:
    ScopedComPtr() = default;

    explicit ScopedComPtr(T* ptr) : ptr(ptr) {
    }
    ~ScopedComPtr() {
        if (ptr) {
            ptr->Release();
        }
    }
    bool Create(const CLSID clsid) {
        CrashIf(ptr);
        if (ptr) {
            return false;
        }
        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&ptr));
        return SUCCEEDED(hr);
    }
    [[nodiscard]] T* Get() const {
        return ptr;
    }
    [[nodiscard]] operator T*() const { // NOLINT
        return ptr;
    }
    T** operator&() {
        return &ptr;
    }
    T* operator->() const {
        return ptr;
    }
    ScopedComPtr<T>& operator=(T* newPtr) {
        if (ptr) {
            ptr->Release();
        }
        ptr = newPtr;
        return *this;
    }
};

template <class T>
class ScopedComQIPtr {
  protected:
    T* ptr{nullptr};

  public:
    ScopedComQIPtr() = default;

    explicit ScopedComQIPtr(IUnknown* unk) {
        HRESULT hr = unk->QueryInterface(&ptr);
        if (FAILED(hr)) {
            ptr = nullptr;
        }
    }
    ~ScopedComQIPtr() {
        if (ptr) {
            ptr->Release();
        }
    }
    bool Create(const CLSID clsid) {
        CrashIf(ptr);
        if (ptr)
            return false;
        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&ptr));
        return SUCCEEDED(hr);
    }
    T* operator=(IUnknown* newUnk) {
        if (ptr)
            ptr->Release();
        HRESULT hr = newUnk->QueryInterface(&ptr);
        if (FAILED(hr))
            ptr = nullptr;
        return ptr;
    }
    [[nodiscard]] operator T*() const { // NOLINT
        return ptr;
    }
    T** operator&() {
        return &ptr;
    }
    T* operator->() const {
        return ptr;
    }
    T* operator=(T* newPtr) {
        if (ptr)
            ptr->Release();
        return (ptr = newPtr);
    }
};

class AutoDeleteDC {
    HDC hdc{nullptr};

  public:
    explicit AutoDeleteDC(HDC hdc) {
        this->hdc = hdc;
    }
    ~AutoDeleteDC() {
        DeleteDC(hdc);
    }
    [[nodiscard]] operator HDC() const { // NOLINT
        return hdc;
    }
};

template <typename T>
class ScopedGdiObj {
    T obj;

  public:
    ScopedGdiObj(T obj) { // NOLINT
        this->obj = obj;
    }
    ~ScopedGdiObj() {
        DeleteObject(obj);
    }
    [[nodiscard]] operator T() const { // NOLINT
        return obj;
    }
};
using AutoDeleteFont = ScopedGdiObj<HFONT>;
using AutoDeletePen = ScopedGdiObj<HPEN>;
using AutoDeleteBrush = ScopedGdiObj<HBRUSH>;

class ScopedGetDC {
    HDC hdc{nullptr};
    HWND hwnd{nullptr};

  public:
    explicit ScopedGetDC(HWND hwnd) {
        this->hwnd = hwnd;
        this->hdc = GetDC(hwnd);
    }
    ~ScopedGetDC() {
        ReleaseDC(hwnd, hdc);
    }
    [[nodiscard]] operator HDC() const { // NOLINT
        return hdc;
    }
};

class ScopedSelectObject {
    HDC hdc{nullptr};
    HGDIOBJ prev{nullptr};

  public:
    ScopedSelectObject(HDC hdc, HGDIOBJ obj) : hdc(hdc) {
        prev = SelectObject(hdc, obj);
    }
    ~ScopedSelectObject() {
        SelectObject(hdc, prev);
    }
};

class ScopedSelectFont {
    HDC hdc{nullptr};
    HFONT prevFont{nullptr};

  public:
    explicit ScopedSelectFont(HDC hdc, HFONT font) {
        prevFont = (HFONT)SelectObject(hdc, font);
    }

    ~ScopedSelectFont() {
        SelectObject(hdc, prevFont);
    }
};

class ScopedSelectPen {
    HDC hdc{nullptr};
    HPEN prevPen{nullptr};

  public:
    explicit ScopedSelectPen(HDC hdc, HPEN pen) {
        prevPen = (HPEN)SelectObject(hdc, pen);
    }

    ~ScopedSelectPen() {
        SelectObject(hdc, prevPen);
    }
};

class ScopedSelectBrush {
    HDC hdc{nullptr};
    HBRUSH prevBrush{nullptr};

  public:
    explicit ScopedSelectBrush(HDC hdc, HBRUSH pen) {
        prevBrush = (HBRUSH)SelectObject(hdc, pen);
    }

    ~ScopedSelectBrush() {
        SelectObject(hdc, prevBrush);
    }
};
class ScopedCom {
  public:
    ScopedCom() {
        (void)CoInitialize(nullptr);
    }
    ~ScopedCom() {
        CoUninitialize();
    }
};

class ScopedOle {
  public:
    ScopedOle() {
        (void)OleInitialize(nullptr);
    }
    ~ScopedOle() {
        OleUninitialize();
    }
};

class ScopedGdiPlus {
  protected:
    Gdiplus::GdiplusStartupInput si{};
    Gdiplus::GdiplusStartupOutput so{};
    ULONG_PTR token{0};
    ULONG_PTR hookToken{0};
    bool noBgThread{false};

  public:
    // suppress the GDI+ background thread when initiating in WinMain,
    // as that thread causes DDE messages to be sent too early and
    // thus causes unexpected timeouts
    explicit ScopedGdiPlus(bool inWinMain = false) : noBgThread(inWinMain) {
        si.SuppressBackgroundThread = noBgThread;
        Gdiplus::GdiplusStartup(&token, &si, &so);
        if (noBgThread) {
            so.NotificationHook(&hookToken);
        }
    }
    ~ScopedGdiPlus() {
        if (noBgThread) {
            so.NotificationUnhook(hookToken);
        }
        Gdiplus::GdiplusShutdown(token);
    }
};
