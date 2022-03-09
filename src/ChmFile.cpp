/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "utils/BaseUtil.h"
#include <chm_lib.h>
#include "utils/ByteReader.h"
#include "utils/FileUtil.h"
#include "utils/GuessFileType.h"
#include "utils/HtmlParserLookup.h"
#include "utils/TrivialHtmlParser.h"
#include "utils/ScopedWin.h"
#include "utils/WinUtil.h"

#include "wingui/TreeModel.h"
#include "DisplayMode.h"
#include "Controller.h"
#include "EngineBase.h"
#include "EbookBase.h"
#include "ChmFile.h"

ChmFile::~ChmFile() {
    chm_close(chmHandle);
}

bool ChmFile::HasData(const char* fileName) const {
    if (!fileName) {
        return false;
    }

    if (!str::StartsWith(fileName, "/")) {
        fileName = str::JoinTemp("/", fileName);
    } else if (str::StartsWith(fileName, "///")) {
        fileName += 2;
    }

    struct chmUnitInfo info {};
    return chm_resolve_object(chmHandle, fileName, &info) == CHM_RESOLVE_SUCCESS;
}

ByteSlice ChmFile::GetData(const char* fileName) const {
    if (!str::StartsWith(fileName, "/")) {
        fileName = str::JoinTemp("/", fileName);
    } else if (str::StartsWith(fileName, "///")) {
        fileName = fileName + 2;
    }

    struct chmUnitInfo info;
    int res = chm_resolve_object(chmHandle, fileName, &info);
    if (CHM_RESOLVE_SUCCESS != res && str::FindChar(fileName, '\\')) {
        // Microsoft's HTML Help CHM viewer tolerates backslashes in URLs
        auto fileNameTemp = str::DupTemp(fileName);
        str::TransCharsInPlace(fileNameTemp, "\\", "/");
        res = chm_resolve_object(chmHandle, fileNameTemp, &info);
    }

    if (CHM_RESOLVE_SUCCESS != res) {
        return {};
    }
    size_t len = (size_t)info.length;
    if (len > 128 * 1024 * 1024) {
        // limit to 128 MB
        return {};
    }

    // +1 for 0 terminator for C string compatibility
    u8* data = AllocArray<u8>(len + 1);
    if (!data) {
        return {};
    }
    if (!chm_retrieve_object(chmHandle, &info, data, 0, len)) {
        return {};
    }

    return {data, len};
}

char* ChmFile::ToUtf8(const u8* text, uint overrideCP) const {
    const char* s = (char*)text;
    if (str::StartsWith(s, UTF8_BOM)) {
        return str::Dup(s + 3);
    }
    if (overrideCP) {
        return (char*)strconv::ToMultiByteV(s, overrideCP, CP_UTF8).data();
    }
    if (CP_UTF8 == codepage) {
        return str::Dup(s);
    }
    return (char*)strconv::ToMultiByteV(s, codepage, CP_UTF8).data();
}

WCHAR* ChmFile::ToStr(const char* text) const {
    return strconv::StrToWstr(text, codepage);
}

static char* GetCharZ(ByteSlice d, size_t off) {
    u8* data = d.data();
    size_t len = d.size();
    if (off >= len) {
        return nullptr;
    }
    CrashIf(!memchr(data + off, '\0', len - off + 1)); // data is zero-terminated
    u8* str = data + off;
    if (str::IsEmpty((const char*)str)) {
        return nullptr;
    }
    return str::Dup((const char*)str);
}

// http://www.nongnu.org/chmspec/latest/Internal.html#WINDOWS
void ChmFile::ParseWindowsData() {
    AutoFree windowsData = GetData("/#WINDOWS");
    auto stringsData = GetData("/#STRINGS");
    AutoFree stringsDataFree(stringsData);
    if (windowsData.empty() || stringsData.empty()) {
        return;
    }
    size_t windowsLen = windowsData.size();
    if (windowsLen <= 8) {
        return;
    }

    ByteReader rw(windowsData, windowsLen);
    size_t entries = rw.DWordLE(0);
    size_t entrySize = rw.DWordLE(4);
    if (entrySize < 188) {
        return;
    }

    for (size_t i = 0; i < entries && ((i + (size_t)1) * entrySize) <= windowsLen; i++) {
        size_t off = 8 + i * entrySize;
        if (!title) {
            DWORD strOff = rw.DWordLE(off + (size_t)0x14);
            title.Set(GetCharZ(stringsData, strOff));
        }
        if (!tocPath) {
            DWORD strOff = rw.DWordLE(off + (size_t)0x60);
            tocPath.Set(GetCharZ(stringsData, strOff));
        }
        if (!indexPath) {
            DWORD strOff = rw.DWordLE(off + (size_t)0x64);
            indexPath.Set(GetCharZ(stringsData, strOff));
        }
        if (!homePath) {
            DWORD strOff = rw.DWordLE(off + (size_t)0x68);
            homePath.Set(GetCharZ(stringsData, strOff));
        }
    }
}

#define CP_CHM_DEFAULT 1252

static uint LcidToCodepage(DWORD lcid) {
    // cf. http://msdn.microsoft.com/en-us/library/bb165625(v=VS.90).aspx
    static struct {
        DWORD lcid;
        uint codepage;
    } lcidToCodepage[] = {
        {1025, 1256}, {2052, 936},  {1028, 950},  {1029, 1250}, {1032, 1253}, {1037, 1255}, {1038, 1250}, {1041, 932},
        {1042, 949},  {1045, 1250}, {1049, 1251}, {1051, 1250}, {1060, 1250}, {1055, 1254}, {1026, 1251}, {4, 936},
    };

    for (int i = 0; i < dimof(lcidToCodepage); i++) {
        if (lcid == lcidToCodepage[i].lcid) {
            return lcidToCodepage[i].codepage;
        }
    }

    return CP_CHM_DEFAULT;
}

// http://www.nongnu.org/chmspec/latest/Internal.html#SYSTEM
bool ChmFile::ParseSystemData() {
    auto data = GetData("/#SYSTEM");
    if (data.empty()) {
        return false;
    }
    AutoFree dataFree = data;

    ByteReader r(data);
    DWORD len = 0;
    // Note: skipping DWORD version at offset 0. It's supposed to be 2 or 3.
    for (size_t off = 4; off + 4 < data.size(); off += len + (size_t)4) {
        // Note: at some point we seem to get off-sync i.e. I'm seeing
        // many entries with type == 0 and len == 0. Seems harmless.
        len = r.WordLE(off + 2);
        if (len == 0) {
            continue;
        }
        WORD type = r.WordLE(off);
        switch (type) {
            case 0:
                if (!tocPath) {
                    tocPath.Set(GetCharZ(data, off + 4));
                }
                break;
            case 1:
                if (!indexPath) {
                    indexPath.Set(GetCharZ(data, off + 4));
                }
                break;
            case 2:
                if (!homePath) {
                    homePath.Set(GetCharZ(data, off + 4));
                }
                break;
            case 3:
                if (!title) {
                    title.Set(GetCharZ(data, off + 4));
                }
                break;
            case 4:
                if (!codepage && len >= 4) {
                    codepage = LcidToCodepage(r.DWordLE(off + 4));
                }
                break;
            case 6:
                // compiled file - ignore
                break;
            case 9:
                if (!creator) {
                    creator.Set(GetCharZ(data, off + 4));
                }
                break;
            case 16:
                // default font - ignore
                break;
        }
    }

    return true;
}

char* ChmFile::ResolveTopicID(unsigned int id) const {
    AutoFree ivbData = GetData("/#IVB");
    size_t ivbLen = ivbData.size();
    ByteReader br(ivbData.AsView());
    if ((ivbLen % 8) != 4 || ivbLen - 4 != br.DWordLE(0)) {
        return nullptr;
    }

    for (size_t off = 4; off < ivbLen; off += 8) {
        if (br.DWordLE(off) == id) {
            AutoFree stringsData(GetData("/#STRINGS"));
            return GetCharZ(stringsData.AsSpan(), br.DWordLE(off + 4));
        }
    }
    return nullptr;
}

void ChmFile::FixPathCodepage(AutoFree& path, uint& fileCP) {
    if (!path || HasData(path)) {
        return;
    }

    AutoFree utf8Path(ToUtf8((u8*)path.Get()));
    if (HasData(utf8Path)) {
        path.Set(utf8Path.Release());
        fileCP = codepage;
    } else if (fileCP != codepage) {
        utf8Path.Set(ToUtf8((u8*)path.Get(), fileCP));
        if (HasData(utf8Path)) {
            path.Set(utf8Path.Release());
            codepage = fileCP;
        }
    }
}

bool ChmFile::Load(const char* fileNameA) {
    WCHAR* fileName = ToWstrTemp(fileNameA);
    data = file::ReadFile(fileName);
    chmHandle = chm_open((const char*)data.Get(), data.size());
    if (!chmHandle) {
        return false;
    }

    ParseWindowsData();
    if (!ParseSystemData()) {
        return false;
    }

    uint fileCodepage = codepage;
    char header[24] = {0};
    int n = file::ReadN(fileName, header, sizeof(header));
    if (n < (int)sizeof(header)) {
        ByteReader r(header, sizeof(header));
        DWORD lcid = r.DWordLE(20);
        fileCodepage = LcidToCodepage(lcid);
    }
    if (!codepage) {
        codepage = fileCodepage;
    }
    // if file and #SYSTEM codepage disagree, prefer #SYSTEM's (unless it leads to wrong paths)
    FixPathCodepage(homePath, fileCodepage);
    FixPathCodepage(tocPath, fileCodepage);
    FixPathCodepage(indexPath, fileCodepage);
    if (GetACP() == codepage) {
        codepage = CP_ACP;
    }

    if (!HasData(homePath)) {
        const char* pathsToTest[] = {"/index.htm", "/index.html", "/default.htm", "/default.html"};
        for (int i = 0; i < dimof(pathsToTest); i++) {
            if (HasData(pathsToTest[i])) {
                homePath.SetCopy(pathsToTest[i]);
            }
        }
        if (!HasData(homePath)) {
            return false;
        }
    }

    return true;
}

WCHAR* ChmFile::GetProperty(DocumentProperty prop) const {
    AutoFreeWstr result;
    if (DocumentProperty::Title == prop && title) {
        result.Set(ToStr(title));
    } else if (DocumentProperty::CreatorApp == prop && creator) {
        result.Set(ToStr(creator));
    }
    // TODO: shouldn't it be up to the front-end to normalize whitespace?
    if (result) {
        // TODO: original code called str::RemoveCharsInPlace(result, "\n\r\t")
        str::NormalizeWSInPlace(result);
    }
    return result.StealData();
}

const char* ChmFile::GetHomePath() const {
    return homePath;
}

static int ChmEnumerateEntry(struct chmFile* chmHandle, struct chmUnitInfo* info, void* data) {
    if (str::IsEmpty(info->path)) {
        return CHM_ENUMERATOR_CONTINUE;
    }
    Vec<char*>* paths = (Vec<char*>*)data;
    paths->Append(str::Dup(info->path));
    return CHM_ENUMERATOR_CONTINUE;
}

Vec<char*>* ChmFile::GetAllPaths() const {
    Vec<char*>* paths = new Vec<char*>();
    chm_enumerate(chmHandle, CHM_ENUMERATE_FILES | CHM_ENUMERATE_NORMAL, ChmEnumerateEntry, paths);
    return paths;
}

/* The html looks like:
<li>
  <object type="text/sitemap">
    <param name="Name" value="Main Page">
    <param name="Local" value="0789729717_main.html">
    <param name="ImageNumber" value="12">
  </object>
  <ul> ... children ... </ul>
<li>
  ... siblings ...
*/
static bool VisitChmTocItem(EbookTocVisitor* visitor, HtmlElement* el, uint cp, int level) {
    CrashIf(el->tag != Tag_Object || level > 1 && (!el->up || el->up->tag != Tag_Li));

    AutoFreeWstr name, local;
    for (el = el->GetChildByTag(Tag_Param); el; el = el->next) {
        if (Tag_Param != el->tag) {
            continue;
        }
        AutoFreeWstr attrName(el->GetAttribute("name"));
        AutoFreeWstr attrVal(el->GetAttribute("value"));
        if (attrName && attrVal && cp != CP_CHM_DEFAULT) {
            AutoFree bytes(strconv::WstrToCodePageV(CP_CHM_DEFAULT, attrVal));
            attrVal.Set(strconv::StrToWstr(bytes.Get(), cp));
        }
        if (!attrName || !attrVal) {
            /* ignore incomplete/unneeded <param> */;
        } else if (str::EqI(attrName, L"Name")) {
            name.Set(attrVal.StealData());
        } else if (str::EqI(attrName, L"Local")) {
            // remove the ITS protocol and any filename references from the URLs
            if (str::Find(attrVal, L"::/")) {
                attrVal.SetCopy(str::Find(attrVal, L"::/") + 3);
            }
            local.Set(attrVal.StealData());
        }
    }
    if (!name) {
        return false;
    }

    visitor->Visit(name, local, level);
    return true;
}

/* The html looks like:
<li>
  <object type="text/sitemap">
    <param name="Keyword" value="- operator">
    <param name="Name" value="Subtraction Operator (-)">
    <param name="Local" value="html/vsoprsubtract.htm">
    <param name="Name" value="Subtraction Operator (-)">
    <param name="Local" value="html/js56jsoprsubtract.htm">
  </object>
  <ul> ... optional children ... </ul>
<li>
  ... siblings ...
*/
static bool VisitChmIndexItem(EbookTocVisitor* visitor, HtmlElement* el, uint cp, int level) {
    CrashIf(el->tag != Tag_Object || level > 1 && (!el->up || el->up->tag != Tag_Li));

    WStrVec references;
    AutoFreeWstr keyword, name;
    for (el = el->GetChildByTag(Tag_Param); el; el = el->next) {
        if (Tag_Param != el->tag) {
            continue;
        }
        AutoFreeWstr attrName(el->GetAttribute("name"));
        AutoFreeWstr attrVal(el->GetAttribute("value"));
        if (attrName && attrVal && cp != CP_CHM_DEFAULT) {
            AutoFree bytes(strconv::WstrToCodePageV(CP_CHM_DEFAULT, attrVal));
            attrVal.Set(strconv::StrToWstr(bytes.Get(), cp));
        }
        if (!attrName || !attrVal) {
            /* ignore incomplete/unneeded <param> */;
        } else if (str::EqI(attrName, L"Keyword")) {
            keyword.Set(attrVal.StealData());
        } else if (str::EqI(attrName, L"Name")) {
            name.Set(attrVal.StealData());
            // some CHM documents seem to use a lonely Name instead of Keyword
            if (!keyword) {
                keyword.SetCopy(name);
            }
        } else if (str::EqI(attrName, L"Local") && name) {
            // remove the ITS protocol and any filename references from the URLs
            if (str::Find(attrVal, L"::/")) {
                attrVal.SetCopy(str::Find(attrVal, L"::/") + 3);
            }
            references.Append(name.StealData());
            references.Append(attrVal.StealData());
        }
    }
    if (!keyword) {
        return false;
    }

    if (references.size() == 2) {
        visitor->Visit(keyword, references.at(1), level);
        return true;
    }
    visitor->Visit(keyword, nullptr, level);
    for (size_t i = 0; i < references.size(); i += 2) {
        visitor->Visit(references.at(i), references.at(i + 1), level + 1);
    }
    return true;
}

static void WalkChmTocOrIndex(EbookTocVisitor* visitor, HtmlElement* list, uint cp, bool isIndex, int level = 1) {
    CrashIf(Tag_Ul != list->tag);

    // some broken ToCs wrap every <li> into its own <ul>
    for (; list && Tag_Ul == list->tag; list = list->next) {
        for (HtmlElement* el = list->down; el; el = el->next) {
            if (Tag_Li != el->tag) {
                continue; // ignore unexpected elements
            }

            bool valid;
            HtmlElement* elObj = el->GetChildByTag(Tag_Object);
            if (!elObj) {
                valid = false;
            } else if (isIndex) {
                valid = VisitChmIndexItem(visitor, elObj, cp, level);
            } else {
                valid = VisitChmTocItem(visitor, elObj, cp, level);
            }
            if (!valid) {
                continue; // skip incomplete elements and all their children
            }

            HtmlElement* nested = el->GetChildByTag(Tag_Ul);
            // some broken ToCs have the <ul> follow right *after* a <li>
            if (!nested && el->next && Tag_Ul == el->next->tag) {
                nested = el->next;
            }
            if (nested) {
                WalkChmTocOrIndex(visitor, nested, cp, isIndex, level + 1);
            }
        }
    }
}

// ignores any <ul><li> list structure and just extracts a linear list of <object type="text/sitemap">...</object>
static bool WalkBrokenChmTocOrIndex(EbookTocVisitor* visitor, HtmlParser& p, uint cp, bool isIndex) {
    bool hadOne = false;

    HtmlElement* el = p.FindElementByName("body");
    while ((el = p.FindElementByName("object", el)) != nullptr) {
        AutoFreeWstr type(el->GetAttribute("type"));
        if (!str::EqI(type, L"text/sitemap")) {
            continue;
        }
        if (isIndex) {
            hadOne |= VisitChmIndexItem(visitor, el, cp, 1);
        } else {
            hadOne |= VisitChmTocItem(visitor, el, cp, 1);
        }
    }

    return hadOne;
}

bool ChmFile::ParseTocOrIndex(EbookTocVisitor* visitor, const char* path, bool isIndex) const {
    if (!path) {
        return false;
    }
    AutoFree htmlData = GetData(path);
    if (htmlData.empty()) {
        return false;
    }
    const char* html = htmlData.Get();

    HtmlParser p;
    uint cp = codepage;
    // detect UTF-8 content by BOM
    if (str::StartsWith(html, UTF8_BOM)) {
        html += 3;
        cp = CP_UTF8;
    }
    // enforce the default codepage, so that pre-encoded text and
    // entities are in the same codepage and VisitChmTocItem yields
    // consistent results
    HtmlElement* el = p.Parse(str::ToSpan(html), CP_CHM_DEFAULT);
    if (!el) {
        return false;
    }
    el = p.FindElementByName("body");
    // since <body> is optional, also continue without one
    el = p.FindElementByName("ul", el);
    if (!el) {
        return WalkBrokenChmTocOrIndex(visitor, p, cp, isIndex);
    }
    WalkChmTocOrIndex(visitor, el, cp, isIndex);
    return true;
}

bool ChmFile::HasToc() const {
    return tocPath != nullptr;
}

bool ChmFile::ParseToc(EbookTocVisitor* visitor) const {
    return ParseTocOrIndex(visitor, tocPath, false);
}

bool ChmFile::HasIndex() const {
    return indexPath != nullptr;
}

bool ChmFile::ParseIndex(EbookTocVisitor* visitor) const {
    return ParseTocOrIndex(visitor, indexPath, true);
}

bool ChmFile::IsSupportedFileType(Kind kind) {
    return kind == kindFileChm;
}

ChmFile* ChmFile::CreateFromFile(const WCHAR* path) {
    ChmFile* chmFile = new ChmFile();
    char* pathA = ToUtf8Temp(path);
    if (!chmFile || !chmFile->Load(pathA)) {
        delete chmFile;
        return nullptr;
    }
    return chmFile;
}
