/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include "utils/ScopedWin.h"
#include "utils/FileUtil.h"
#include "utils/WinUtil.h"

#include "AppTools.h"
#include "wingui/TreeModel.h"
#include "DisplayMode.h"
#include "Controller.h"
#include "EngineBase.h"
#include "EngineAll.h"
#include "SettingsStructs.h"
#include "DisplayModel.h"
#include "AppColors.h"
#include "ProgressUpdateUI.h"
#include "Notifications.h"
#include "SumatraPDF.h"
#include "WindowInfo.h"
#include "resource.h"
#include "Commands.h"
#include "SumatraAbout.h"
#include "SumatraProperties.h"
#include "Translations.h"

#define PROPERTIES_LEFT_RIGHT_SPACE_DX 8
#define PROPERTIES_RECT_PADDING 8
#define PROPERTIES_TXT_DY_PADDING 2
#define PROPERTIES_WIN_TITLE _TR("Document Properties")

class PropertyEl {
  public:
    PropertyEl(const WCHAR* leftTxt, WCHAR* rightTxt, bool isPath = false) : leftTxt(leftTxt), isPath(isPath) {
        this->rightTxt.Set(rightTxt);
    }

    // A property is always in format: Name (left): Value (right)
    // (leftTxt is static, rightTxt will be freed)
    const WCHAR* leftTxt;
    AutoFreeWstr rightTxt;

    // data calculated by the layout
    Rect leftPos;
    Rect rightPos;

    // overlong paths get the ellipsis in the middle instead of at the end
    bool isPath;
};

class PropertiesLayout : public Vec<PropertyEl*> {
  public:
    PropertiesLayout() = default;
    ~PropertiesLayout() {
        DeleteVecMembers(*this);
    }

    void AddProperty(const WCHAR* key, WCHAR* value, bool isPath = false) {
        // don't display value-less properties
        if (!str::IsEmpty(value)) {
            Append(new PropertyEl(key, value, isPath));
        } else {
            free(value);
        }
    }
    bool HasProperty(const WCHAR* key) {
        for (size_t i = 0; i < size(); i++) {
            if (str::Eq(key, at(i)->leftTxt)) {
                return true;
            }
        }
        return false;
    }

    HWND hwnd{nullptr};
    HWND hwndParent{nullptr};
};

static Vec<PropertiesLayout*> gPropertiesWindows;

static PropertiesLayout* FindPropertyWindowByParent(HWND hwndParent) {
    for (PropertiesLayout* pl : gPropertiesWindows) {
        if (pl->hwndParent == hwndParent) {
            return pl;
        }
    }
    return nullptr;
}

static PropertiesLayout* FindPropertyWindowByHwnd(HWND hwnd) {
    for (PropertiesLayout* pl : gPropertiesWindows) {
        if (pl->hwnd == hwnd) {
            return pl;
        }
    }
    return nullptr;
}

void DeletePropertiesWindow(HWND hwndParent) {
    PropertiesLayout* pl = FindPropertyWindowByParent(hwndParent);
    if (pl) {
        DestroyWindow(pl->hwnd);
    }
}

// See: http://www.verypdf.com/pdfinfoeditor/pdf-date-format.htm
// Format:  "D:YYYYMMDDHHMMSSxxxxxxx"
// Example: "D:20091222171933-05'00'"
static bool PdfDateParse(const WCHAR* pdfDate, SYSTEMTIME* timeOut) {
    ZeroMemory(timeOut, sizeof(SYSTEMTIME));
    // "D:" at the beginning is optional
    if (str::StartsWith(pdfDate, L"D:")) {
        pdfDate += 2;
    }
    return str::Parse(pdfDate,
                      L"%4d%2d%2d"
                      L"%2d%2d%2d",
                      &timeOut->wYear, &timeOut->wMonth, &timeOut->wDay, &timeOut->wHour, &timeOut->wMinute,
                      &timeOut->wSecond) != nullptr;
    // don't bother about the day of week, we won't display it anyway
}

// See: ISO 8601 specification
// Format:  "YYYY-MM-DDTHH:MM:SSZ"
// Example: "2011-04-19T22:10:48Z"
static bool IsoDateParse(const WCHAR* isoDate, SYSTEMTIME* timeOut) {
    ZeroMemory(timeOut, sizeof(SYSTEMTIME));
    const WCHAR* end = str::Parse(isoDate, L"%4d-%2d-%2d", &timeOut->wYear, &timeOut->wMonth, &timeOut->wDay);
    if (end) { // time is optional
        str::Parse(end, L"T%2d:%2d:%2dZ", &timeOut->wHour, &timeOut->wMinute, &timeOut->wSecond);
    }
    return end != nullptr;
    // don't bother about the day of week, we won't display it anyway
}

static WCHAR* FormatSystemTime(SYSTEMTIME& date) {
    WCHAR buf[512] = {0};
    int cchBufLen = dimof(buf);
    int ret = GetDateFormat(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &date, nullptr, buf, cchBufLen);
    if (ret < 2) { // GetDateFormat() failed or returned an empty result
        return nullptr;
    }

    // don't add 00:00:00 for dates without time
    if (0 == date.wHour && 0 == date.wMinute && 0 == date.wSecond) {
        return str::Dup(buf);
    }

    WCHAR* tmp = buf + ret;
    tmp[-1] = ' ';
    ret = GetTimeFormat(LOCALE_USER_DEFAULT, 0, &date, nullptr, tmp, cchBufLen - ret);
    if (ret < 2) { // GetTimeFormat() failed or returned an empty result
        tmp[-1] = '\0';
    }

    return str::Dup(buf);
}

// Convert a date in PDF or XPS format, e.g. "D:20091222171933-05'00'" to a display
// format e.g. "12/22/2009 5:19:33 PM"
// See: http://www.verypdf.com/pdfinfoeditor/pdf-date-format.htm
// The conversion happens in place
static void ConvDateToDisplay(WCHAR** s, bool (*DateParse)(const WCHAR* date, SYSTEMTIME* timeOut)) {
    if (!s || !*s || !DateParse) {
        return;
    }

    SYSTEMTIME date = {0};
    bool ok = DateParse(*s, &date);
    if (!ok) {
        return;
    }

    WCHAR* formatted = FormatSystemTime(date);
    if (formatted) {
        free(*s);
        *s = formatted;
    }
}

struct PaperSizeDesc {
    float minDx, maxDx;
    float minDy, maxDy;
    PaperFormat paperFormat;
};

// clang-format off
static PaperSizeDesc paperSizes[] = {
    // common ISO 216 formats (metric)
    {
        16.53f, 16.55f,
        23.38f, 23.40f,
        PaperFormat::A2,
    },
    {
        11.68f, 11.70f,
        16.53f, 16.55f,
        PaperFormat::A3,
    },
    {
        8.26f, 8.28f,
        11.68f, 11.70f,
        PaperFormat::A4,
    },
    {
        5.82f, 5.85f,
        8.26f, 8.28f,
        PaperFormat::A5,
    },
    {
        4.08f, 4.10f,
        5.82f, 5.85f,
        PaperFormat::A6,
    },
    // common US/ANSI formats (imperial)
    {
        8.49f, 8.51f,
        10.99f, 11.01f,
        PaperFormat::Letter,
    },
    {
        8.49f, 8.51f,
        13.99f, 14.01f,
        PaperFormat::Legal,
    },
    {
        10.99f, 11.01f,
        16.99f, 17.01f,
        PaperFormat::Tabloid,
    },
    {
        5.49f, 5.51f,
        8.49f, 8.51f,
        PaperFormat::Statement,
    }
};
// clang-format on

static bool fInRange(float x, float min, float max) {
    return x >= min && x <= max;
}

PaperFormat GetPaperFormat(SizeF size) {
    float dx = size.dx;
    float dy = size.dy;
    if (dx > dy) {
        std::swap(dx, dy);
    }
    for (auto&& desc : paperSizes) {
        bool ok = fInRange(dx, desc.minDx, desc.maxDx) && fInRange(dy, desc.minDy, desc.maxDy);
        if (ok) {
            return desc.paperFormat;
        }
    }
    return PaperFormat::Other;
}

// format page size according to locale (e.g. "29.7 x 21.0 cm" or "11.69 x 8.27 in")
// Caller needs to free the result
static WCHAR* FormatPageSize(EngineBase* engine, int pageNo, int rotation) {
    RectF mediabox = engine->PageMediabox(pageNo);
    SizeF size = engine->Transform(mediabox, pageNo, 1.0f / engine->GetFileDPI(), rotation).Size();

    const WCHAR* formatName = L"";
    switch (GetPaperFormat(size)) {
        case PaperFormat::A2:
            formatName = L" (A2)";
            break;
        case PaperFormat::A3:
            formatName = L" (A3)";
            break;
        case PaperFormat::A4:
            formatName = L" (A4)";
            break;
        case PaperFormat::A5:
            formatName = L" (A5)";
            break;
        case PaperFormat::A6:
            formatName = L" (A6)";
            break;
        case PaperFormat::Letter:
            formatName = L" (Letter)";
            break;
        case PaperFormat::Legal:
            formatName = L" (Legal)";
            break;
        case PaperFormat::Tabloid:
            formatName = L" (Tabloid)";
            break;
        case PaperFormat::Statement:
            formatName = L" (Statement)";
            break;
    }

    bool isMetric = GetMeasurementSystem() == 0;
    double unitsPerInch = isMetric ? 2.54 : 1.0;
    const WCHAR* unit = isMetric ? L"cm" : L"in";

    double width = size.dx * unitsPerInch;
    double height = size.dy * unitsPerInch;
    if (((int)(width * 100)) % 100 == 99) {
        width += 0.01;
    }
    if (((int)(height * 100)) % 100 == 99) {
        height += 0.01;
    }

    AutoFreeWstr strWidth(str::FormatFloatWithThousandSep(width));
    AutoFreeWstr strHeight(str::FormatFloatWithThousandSep(height));

    return str::Format(L"%s x %s %s%s", strWidth.Get(), strHeight.Get(), unit, formatName);
}

static WCHAR* FormatPdfFileStructure(Controller* ctrl) {
    AutoFreeWstr fstruct(ctrl->GetProperty(DocumentProperty::PdfFileStructure));
    if (str::IsEmpty(fstruct.Get())) {
        return nullptr;
    }
    WStrVec parts;
    parts.Split(fstruct, L",", true);

    WStrVec props;

    if (parts.Contains(L"linearized")) {
        props.Append(str::Dup(_TR("Fast Web View")));
    }
    if (parts.Contains(L"tagged")) {
        props.Append(str::Dup(_TR("Tagged PDF")));
    }
    if (parts.Contains(L"PDFX")) {
        props.Append(str::Dup(L"PDF/X (ISO 15930)"));
    }
    if (parts.Contains(L"PDFA1")) {
        props.Append(str::Dup(L"PDF/A (ISO 19005)"));
    }
    if (parts.Contains(L"PDFE1")) {
        props.Append(str::Dup(L"PDF/E (ISO 24517)"));
    }

    return props.Join(L", ");
}

// returns a list of permissions denied by this document
// Caller needs to free the result
static WCHAR* FormatPermissions(Controller* ctrl) {
    if (!ctrl->AsFixed()) {
        return nullptr;
    }

    WStrVec denials;

    EngineBase* engine = ctrl->AsFixed()->GetEngine();
    if (!engine->AllowsPrinting()) {
        denials.Append(str::Dup(_TR("printing document")));
    }
    if (!engine->AllowsCopyingText()) {
        denials.Append(str::Dup(_TR("copying text")));
    }

    return denials.Join(L", ");
}

static void UpdatePropertiesLayout(PropertiesLayout* layoutData, HDC hdc, Rect* rect) {
    AutoDeleteFont fontLeftTxt(CreateSimpleFont(hdc, kLeftTextFont, kLeftTextFontSize));
    AutoDeleteFont fontRightTxt(CreateSimpleFont(hdc, kRightTextFont, kRightTextFontSize));
    HGDIOBJ origFont = SelectObject(hdc, fontLeftTxt);

    /* calculate text dimensions for the left side */
    SelectObject(hdc, fontLeftTxt);
    int leftMaxDx = 0;
    for (PropertyEl* el : *layoutData) {
        const WCHAR* txt = el->leftTxt;
        RECT rc = {0};
        DrawTextW(hdc, txt, -1, &rc, DT_NOPREFIX | DT_CALCRECT);
        el->leftPos.dx = rc.right - rc.left;
        // el->leftPos.dy is set below to be equal to el->rightPos.dy

        if (el->leftPos.dx > leftMaxDx) {
            leftMaxDx = el->leftPos.dx;
        }
    }

    /* calculate text dimensions for the right side */
    SelectObject(hdc, fontRightTxt);
    int rightMaxDx = 0;
    int lineCount = 0;
    int textDy = 0;
    for (PropertyEl* el : *layoutData) {
        const WCHAR* txt = el->rightTxt;
        RECT rc = {0};
        DrawTextW(hdc, txt, -1, &rc, DT_NOPREFIX | DT_CALCRECT);
        el->rightPos.dx = rc.right - rc.left;
        el->leftPos.dy = el->rightPos.dy = rc.bottom - rc.top;
        textDy += el->rightPos.dy;

        if (el->rightPos.dx > rightMaxDx) {
            rightMaxDx = el->rightPos.dx;
        }
        lineCount++;
    }

    CrashIf(!(lineCount > 0 && textDy > 0));
    int totalDx = leftMaxDx + PROPERTIES_LEFT_RIGHT_SPACE_DX + rightMaxDx;

    int totalDy = 4;
    totalDy += textDy + (lineCount - 1) * PROPERTIES_TXT_DY_PADDING;
    totalDy += 4;

    int offset = PROPERTIES_RECT_PADDING;
    if (rect) {
        *rect = Rect(0, 0, totalDx + 2 * offset, totalDy + offset);
    }

    int currY = 0;
    for (PropertyEl* el : *layoutData) {
        el->leftPos = Rect(offset, offset + currY, leftMaxDx, el->leftPos.dy);
        el->rightPos.x = offset + leftMaxDx + PROPERTIES_LEFT_RIGHT_SPACE_DX;
        el->rightPos.y = offset + currY;
        currY += el->rightPos.dy + PROPERTIES_TXT_DY_PADDING;
    }

    SelectObject(hdc, origFont);
}

static bool CreatePropertiesWindow(HWND hParent, PropertiesLayout* layoutData) {
    CrashIf(layoutData->hwnd);
    auto h = GetModuleHandleW(nullptr);
    DWORD dwStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    auto clsName = PROPERTIES_CLASS_NAME;
    auto title = PROPERTIES_WIN_TITLE;
    HWND hwnd = CreateWindowW(clsName, title, dwStyle, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                              nullptr, nullptr, h, nullptr);
    if (!hwnd) {
        return false;
    }

    layoutData->hwnd = hwnd;
    layoutData->hwndParent = hParent;
    SetRtl(hwnd, IsUIRightToLeft());

    // get the dimensions required for the about box's content
    Rect rc;
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    UpdatePropertiesLayout(layoutData, hdc, &rc);
    EndPaint(hwnd, &ps);

    // resize the new window to just match these dimensions
    // (as long as they fit into the current monitor's work area)
    Rect wRc = WindowRect(hwnd);
    Rect cRc = ClientRect(hwnd);
    Rect work = GetWorkAreaRect(WindowRect(hParent), hwnd);
    wRc.dx = std::min(rc.dx + wRc.dx - cRc.dx, work.dx);
    wRc.dy = std::min(rc.dy + wRc.dy - cRc.dy, work.dy);
    MoveWindow(hwnd, wRc.x, wRc.y, wRc.dx, wRc.dy, FALSE);
    CenterDialog(hwnd, hParent);

    ShowWindow(hwnd, SW_SHOW);
    return true;
}

static void GetProps(Controller* ctrl, PropertiesLayout* layoutData, bool extended) {
    CrashIf(!ctrl);

    WCHAR* str = str::Dup(gPluginMode ? gPluginURL : ctrl->GetFilePath());
    layoutData->AddProperty(_TR("File:"), str, true);

    str = ctrl->GetProperty(DocumentProperty::Title);
    layoutData->AddProperty(_TR("Title:"), str);

    str = ctrl->GetProperty(DocumentProperty::Subject);
    layoutData->AddProperty(_TR("Subject:"), str);

    str = ctrl->GetProperty(DocumentProperty::Author);
    layoutData->AddProperty(_TR("Author:"), str);

    str = ctrl->GetProperty(DocumentProperty::Copyright);
    layoutData->AddProperty(_TR("Copyright:"), str);

    DisplayModel* dm = ctrl->AsFixed();
    str = ctrl->GetProperty(DocumentProperty::CreationDate);
    if (str && dm && kindEngineMupdf == dm->engineType) {
        ConvDateToDisplay(&str, PdfDateParse);
    } else {
        ConvDateToDisplay(&str, IsoDateParse);
    }
    layoutData->AddProperty(_TR("Created:"), str);

    str = ctrl->GetProperty(DocumentProperty::ModificationDate);
    if (str && dm && kindEngineMupdf == dm->engineType) {
        ConvDateToDisplay(&str, PdfDateParse);
    } else {
        ConvDateToDisplay(&str, IsoDateParse);
    }
    layoutData->AddProperty(_TR("Modified:"), str);

    str = ctrl->GetProperty(DocumentProperty::CreatorApp);
    layoutData->AddProperty(_TR("Application:"), str);

    str = ctrl->GetProperty(DocumentProperty::PdfProducer);
    layoutData->AddProperty(_TR("PDF Producer:"), str);

    str = ctrl->GetProperty(DocumentProperty::PdfVersion);
    layoutData->AddProperty(_TR("PDF Version:"), str);

    str = FormatPdfFileStructure(ctrl);
    layoutData->AddProperty(_TR("PDF Optimizations:"), str);

    auto path = ToUtf8Temp(ctrl->GetFilePath());
    i64 fileSize = file::GetSize(path.AsView());
    if (-1 == fileSize && dm) {
        EngineBase* engine = dm->GetEngine();
        AutoFree d = engine->GetFileData();
        if (!d.empty()) {
            fileSize = d.size();
        }
    }
    if (-1 != fileSize) {
        str = FormatFileSize((size_t)fileSize);
        layoutData->AddProperty(_TR("File Size:"), str);
    }

    str = str::Format(L"%d", ctrl->PageCount());
    layoutData->AddProperty(_TR("Number of Pages:"), str);

    if (dm) {
        str = FormatPageSize(dm->GetEngine(), ctrl->CurrentPageNo(), dm->GetRotation());
        if (IsUIRightToLeft() && IsWindowsVistaOrGreater()) {
            // ensure that the size remains ungarbled left-to-right
            // (note: XP doesn't know about \u202A...\u202C)
            WCHAR* tmp = str;
            str = str::Format(L"\u202A%s\u202C", tmp);
            free(tmp);
        }
        layoutData->AddProperty(_TR("Page Size:"), str);
    }

    str = FormatPermissions(ctrl);
    layoutData->AddProperty(_TR("Denied Permissions:"), str);

    if (extended) {
        // TODO: FontList extraction can take a while
        str = ctrl->GetProperty(DocumentProperty::FontList);
        if (str) {
            // add a space between basic and extended file properties
            layoutData->AddProperty(L" ", str::Dup(L" "));
        }
        layoutData->AddProperty(_TR("Fonts:"), str);
    }
}

static void ShowProperties(HWND parent, Controller* ctrl, bool extended = false) {
    PropertiesLayout* layoutData = FindPropertyWindowByParent(parent);
    if (layoutData) {
        SetActiveWindow(layoutData->hwnd);
        return;
    }

    if (!ctrl) {
        return;
    }
    layoutData = new PropertiesLayout();
    gPropertiesWindows.Append(layoutData);
    GetProps(ctrl, layoutData, extended);

    if (!CreatePropertiesWindow(parent, layoutData)) {
        delete layoutData;
    }
}

void OnMenuProperties(WindowInfo* win) {
    ShowProperties(win->hwndFrame, win->ctrl);
}

static void DrawProperties(HWND hwnd, HDC hdc) {
    PropertiesLayout* layoutData = FindPropertyWindowByHwnd(hwnd);

    AutoDeleteFont fontLeftTxt(CreateSimpleFont(hdc, kLeftTextFont, kLeftTextFontSize));
    AutoDeleteFont fontRightTxt(CreateSimpleFont(hdc, kRightTextFont, kRightTextFontSize));

    HGDIOBJ origFont = SelectObject(hdc, fontLeftTxt); /* Just to remember the orig font */

    SetBkMode(hdc, TRANSPARENT);

    Rect rcClient = ClientRect(hwnd);
    RECT rTmp = ToRECT(rcClient);
    auto col = GetAppColor(AppColor::MainWindowBg);
    ScopedGdiObj<HBRUSH> brushAboutBg(CreateSolidBrush(col));
    FillRect(hdc, &rTmp, brushAboutBg);

    col = GetAppColor(AppColor::MainWindowText);
    SetTextColor(hdc, col);

    /* render text on the left*/
    SelectObject(hdc, fontLeftTxt);
    for (PropertyEl* el : *layoutData) {
        const WCHAR* txt = el->leftTxt;
        rTmp = ToRECT(el->leftPos);
        DrawTextW(hdc, txt, -1, &rTmp, DT_RIGHT | DT_NOPREFIX);
    }

    /* render text on the right */
    SelectObject(hdc, fontRightTxt);
    for (PropertyEl* el : *layoutData) {
        const WCHAR* txt = el->rightTxt;
        Rect rc = el->rightPos;
        if (rc.x + rc.dx > rcClient.x + rcClient.dx - PROPERTIES_RECT_PADDING) {
            rc.dx = rcClient.x + rcClient.dx - PROPERTIES_RECT_PADDING - rc.x;
        }
        rTmp = ToRECT(rc);
        uint format = DT_LEFT | DT_NOPREFIX | (el->isPath ? DT_PATH_ELLIPSIS : DT_WORD_ELLIPSIS);
        DrawTextW(hdc, txt, -1, &rTmp, format);
    }

    SelectObject(hdc, origFont);
}

static void OnPaintProperties(HWND hwnd) {
    PAINTSTRUCT ps;
    Rect rc;
    HDC hdc = BeginPaint(hwnd, &ps);
    UpdatePropertiesLayout(FindPropertyWindowByHwnd(hwnd), hdc, &rc);
    DrawProperties(hwnd, hdc);
    EndPaint(hwnd, &ps);
}

static void CopyPropertiesToClipboard(HWND hwnd) {
    PropertiesLayout* layoutData = FindPropertyWindowByHwnd(hwnd);
    if (!layoutData) {
        return;
    }

    // concatenate all the properties into a multi-line string
    str::WStr lines(256);
    for (PropertyEl* el : *layoutData) {
        lines.AppendFmt(L"%s %s\r\n", el->leftTxt, el->rightTxt.Get());
    }

    CopyTextToClipboard(lines.LendData());
}

static void PropertiesOnCommand(HWND hwnd, WPARAM wp) {
    auto cmd = LOWORD(wp);
    switch (cmd) {
        case CmdCopySelection:
            CopyPropertiesToClipboard(hwnd);
            break;

        case CmdProperties:
            // make a repeated Ctrl+D display some extended properties
            // TODO: expose this through a UI button or similar
            PropertiesLayout* pl = FindPropertyWindowByHwnd(hwnd);
            if (pl) {
                WindowInfo* win = FindWindowInfoByHwnd(pl->hwndParent);
                if (win && !pl->HasProperty(_TR("Fonts:"))) {
                    DestroyWindow(hwnd);
                    ShowProperties(win->hwndFrame, win->ctrl, true);
                }
            }
            break;
    }
}

LRESULT CALLBACK WndProcProperties(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PropertiesLayout* pl;

    switch (msg) {
        case WM_CREATE:
            break;

        case WM_ERASEBKGND:
            // do nothing, helps to avoid flicker
            return TRUE;

        case WM_PAINT:
            OnPaintProperties(hwnd);
            break;

        case WM_CHAR:
            if (VK_ESCAPE == wp) {
                DestroyWindow(hwnd);
            }
            break;

        case WM_DESTROY:
            pl = FindPropertyWindowByHwnd(hwnd);
            CrashIf(!pl);
            gPropertiesWindows.Remove(pl);
            delete pl;
            break;

        case WM_COMMAND:
            PropertiesOnCommand(hwnd, wp);
            break;

        /* TODO: handle mouse move/down/up so that links work (?) */
        default:
            return DefWindowProc(hwnd, msg, wp, lp);
    }
    return 0;
}
