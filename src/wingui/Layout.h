/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// port of https://gitlab.com/stone.code/goey

const int Inf = std::numeric_limits<int>::max();

RECT RectToRECT(Rect);

int Clamp(int v, int vmin, int vmax);
int Scale(int v, i64 num, i64 den);
int GuardInf(int a, int b);

struct Constraints {
    Size min{};
    Size max{};

    [[nodiscard]] Size Constrain(Size) const;
    [[nodiscard]] Size ConstrainAndAttemptToPreserveAspectRatio(Size) const;
    [[nodiscard]] int ConstrainHeight(int height) const;
    [[nodiscard]] int ConstrainWidth(int width) const;
    [[nodiscard]] bool HasBoundedHeight() const;
    [[nodiscard]] bool HasBoundedWidth() const;
    [[nodiscard]] bool HasTightWidth() const;
    [[nodiscard]] bool HasTightHeight() const;
    [[nodiscard]] Constraints Inset(int width, int height) const;
    [[nodiscard]] bool IsBounded() const;
    [[nodiscard]] bool IsNormalized() const;
    [[nodiscard]] bool IsTight() const;
    [[nodiscard]] bool IsSatisfiedBy(Size) const;
    [[nodiscard]] bool IsZero() const;
    [[nodiscard]] Constraints Loosen() const;
    [[nodiscard]] Constraints LoosenHeight() const;
    [[nodiscard]] Constraints LoosenWidth() const;
    [[nodiscard]] Constraints Tighten(Size) const;
    [[nodiscard]] Constraints TightenHeight(int height) const;
    [[nodiscard]] Constraints TightenWidth(int width) const;
};

Constraints ExpandInf();
Constraints ExpandHeight(int width);
Constraints ExpandWidth(int height);
Constraints Loose(Size size);
Constraints Tight(Size size);
Constraints TightHeight(int height);

using NeedLayout = std::function<void()>;

// works like css visibility property
enum class Visibility {
    Visible,
    // not visible but takes up space for purpose of layout
    Hidden,
    // not visible and doesn't take up space
    Collapse,
};

struct ILayout {
    virtual ~ILayout() = default;
    ;
    virtual Kind GetKind() = 0;
    virtual void SetVisibility(Visibility) = 0;
    virtual Visibility GetVisibility() = 0;
    virtual int MinIntrinsicHeight(int width) = 0;
    virtual int MinIntrinsicWidth(int height) = 0;
    virtual Size Layout(Constraints bc) = 0;
    virtual void SetBounds(Rect) = 0;
};

bool IsCollapsed(ILayout*);

struct LayoutBase : public ILayout {
    Kind kind = nullptr;
    // allows easy way to hide / show elements
    // without rebuilding the whole layout
    Visibility visibility = Visibility::Visible;
    // for easy debugging, remember last bounds
    Rect lastBounds{};

    LayoutBase() = default;
    explicit LayoutBase(Kind);

    Kind GetKind() override;
    void SetVisibility(Visibility) override;
    Visibility GetVisibility() override;
};

bool IsLayoutOfKind(ILayout*, Kind);

// padding.go

struct Insets {
    int top = 0;
    int right = 0;
    int bottom = 0;
    int left = 0;
};

Insets DefaultInsets();
Insets DpiScaledInsets(HWND, int top, int right = -1, int bottom = -1, int left = -1);

struct Padding : LayoutBase {
    ILayout* child = nullptr;
    Insets insets{};
    Size childSize{};

    Padding(ILayout*, const Insets&);
    ~Padding() override;
    Size Layout(Constraints bc) override;
    int MinIntrinsicHeight(int width) override;
    int MinIntrinsicWidth(int height) override;
    void SetBounds(Rect) override;
};

bool IsPadding(Kind);
bool IsPadding(ILayout*);

// vbox.go

// TODO: rename MainStart => Start, MainEnd => End, MainCenter => Center
// Homogeneous => Evenly
enum class MainAxisAlign : u8 {
    // Children will be packed together at the top or left of the box
    MainStart,
    // Children will be packed together and centered in the box.
    MainCenter,
    // Children will be packed together at the bottom or right of the box
    MainEnd,
    // Children will be spaced apart
    SpaceAround,
    // Children will be spaced apart, but the first and last children will but the ends of the box.
    SpaceBetween,
    // Children will be allocated equal space.
    Homogeneous,
};

inline bool IsPacked(MainAxisAlign a) {
    return a <= MainAxisAlign::MainEnd;
}

enum class CrossAxisAlign : u8 {
    Stretch,     // Children will be stretched so that the extend across box
    CrossStart,  // Children will be aligned to the left or top of the box
    CrossCenter, // Children will be aligned in the center of the box
    CrossEnd,    // Children will be aligned to the right or bottom of the box
};

struct boxElementInfo {
    ILayout* layout = nullptr;
    Size size = {};
    int flex = 0;
};

struct VBox : LayoutBase {
    Vec<boxElementInfo> children;
    MainAxisAlign alignMain = MainAxisAlign::MainStart;
    CrossAxisAlign alignCross = CrossAxisAlign::CrossStart;
    int totalHeight = 0;
    int totalFlex = 0;

    VBox();
    ~VBox() override;
    Size Layout(Constraints bc) override;
    int MinIntrinsicHeight(int width) override;
    int MinIntrinsicWidth(int height) override;
    void SetBounds(Rect bounds) override;

    void SetBoundsForChild(int i, ILayout* v, int posX, int posY, int posX2, int posY2) const;

    boxElementInfo& AddChild(ILayout* child);
    boxElementInfo& AddChild(ILayout* child, int flex);
    int ChildrenCount() const;
    int NonCollapsedChildrenCount();
};

// hbox.go

struct HBox : LayoutBase {
    Vec<boxElementInfo> children;
    MainAxisAlign alignMain = MainAxisAlign::MainStart;
    CrossAxisAlign alignCross = CrossAxisAlign::CrossStart;
    int totalWidth = 0;
    int totalFlex = 0;

    ~HBox() override;
    Size Layout(Constraints bc) override;
    int MinIntrinsicHeight(int width) override;
    int MinIntrinsicWidth(int height) override;
    void SetBounds(Rect bounds) override;

    void SetBoundsForChild(int i, ILayout* v, int posX, int posY, int posX2, int posY2) const;
    boxElementInfo& AddChild(ILayout* child);
    boxElementInfo& AddChild(ILayout* child, int flex);
    int ChildrenCount() const;
    int NonCollapsedChildrenCount();
};

// align.go

// defined as i64 but values are i32
typedef i64 Alignment;

constexpr Alignment AlignStart = -32768;
constexpr Alignment AlignCenter = 0;
constexpr Alignment AlignEnd = 0x7fff;

struct Align : LayoutBase {
    Alignment HAlign = AlignStart; // Horizontal alignment of child widget.
    Alignment VAlign = AlignStart; // Vertical alignment of child widget.
    float WidthFactor = 0;         // If greater than zero, ratio of container width to child width.
    float HeightFactor = 0;        // If greater than zero, ratio of container height to child height.
    ILayout* Child = nullptr;
    Size childSize{};

    explicit Align(ILayout*);
    ~Align() override;
    Size Layout(Constraints bc) override;
    int MinIntrinsicHeight(int width) override;
    int MinIntrinsicWidth(int height) override;
    void SetBounds(Rect) override;
};

// spacer is to be used to take space
// can be used for flexible
struct Spacer : LayoutBase {
    int dx = 0;
    int dy = 0;

    Spacer(int, int);
    ~Spacer() override;
    Size Layout(Constraints bc) override;
    int MinIntrinsicHeight(int width) override;
    int MinIntrinsicWidth(int height) override;
    void SetBounds(Rect) override;
};

void LayoutAndSizeToContent(ILayout* layout, int minDx, int minDy, HWND hwnd);
Size LayoutToSize(ILayout* layout, Size size);

void dbglayoutf(const char* fmt, ...);
void LogConstraints(Constraints c, const char* suffix);
