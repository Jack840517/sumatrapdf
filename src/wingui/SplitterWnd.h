/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

enum class SplitterType {
    Horiz,
    Vert,
};

struct SplitterCtrl;

// called when user drags the splitter ('done' is false) and when drag is finished ('done' is
// true). the owner can constrain splitter by using current cursor
// position and setting resizeAllowed to false if it's not allowed to go there
struct SplitterMoveEvent {
    SplitterCtrl* w{nullptr};
    bool done{false};
    // user can set to false to forbid resizing here
    bool resizeAllowed{true};
};

using SplitterMoveHandler = std::function<void(SplitterMoveEvent*)>;

// TODO: maybe derive from WindowBase and allow registering custom classes
// for WindowBase
struct SplitterCtrl : public Window {
    SplitterType type = SplitterType::Horiz;
    bool isLive{true};
    SplitterMoveHandler onSplitterMove{nullptr};

    HBITMAP bmp{nullptr};
    HBRUSH brush{nullptr};

    Point prevResizeLinePos{};
    // if a parent clips children, DrawXorBar() doesn't work, so for
    // non-live resize, we need to remove WS_CLIPCHILDREN style from
    // parent and restore it when we're done
    bool parentClipsChildren{false};

    explicit SplitterCtrl(HWND parent);
    ~SplitterCtrl() override;

    bool Create() override;
};
