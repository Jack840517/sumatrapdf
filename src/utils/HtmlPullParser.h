/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

struct AttrInfo {
    const char* name;
    size_t nameLen;
    const char* val;
    size_t valLen;

    bool NameIs(const char* s) const;
    bool NameIsNS(const char* nameToCheck, const char* ns) const;
    bool ValIs(const char* s) const;
};

// TrivialHtmlParser needs to enumerate all attributes of an HtmlToken
class HtmlParser;

struct HtmlToken {
    friend HtmlParser;

    enum TokenType {
        StartTag,        // <foo>
        EndTag,          // </foo>
        EmptyElementTag, // <foo/>
        Text,            // <foo>text</foo> => "text"
        Error
    };

    enum ParsingError { NoError, ExpectedElement, UnclosedTag, InvalidTag };

    [[nodiscard]] bool IsStartTag() const {
        return type == StartTag;
    }
    [[nodiscard]] bool IsEndTag() const {
        return type == EndTag;
    }
    [[nodiscard]] bool IsEmptyElementEndTag() const {
        return type == EmptyElementTag;
    }
    [[nodiscard]] bool IsTag() const {
        return IsStartTag() || IsEndTag() || IsEmptyElementEndTag();
    }
    [[nodiscard]] bool IsText() const {
        return type == Text;
    }
    [[nodiscard]] bool IsError() const {
        return type == Error;
    }

    [[nodiscard]] const char* GetReparsePoint() const;
    void SetTag(TokenType new_type, const char* new_s, const char* end);
    void SetError(ParsingError err, const char* errContext);
    void SetText(const char* new_s, const char* end);

    TokenType type = Error;
    ParsingError error = NoError;
    const char* s = nullptr;
    size_t sLen = 0;

    // only for tags: type and name length
    HtmlTag tag = Tag_NotFound;
    size_t nLen = 0;

    bool NameIs(const char* name) const;
    bool NameIsNS(const char* name, const char* ns) const;
    AttrInfo* GetAttrByName(const char* name);
    AttrInfo* GetAttrByNameNS(const char* name, const char* attrNS);

  protected:
    AttrInfo* NextAttr();
    const char* nextAttr;
    AttrInfo attrInfo;
};

/* A very simple pull html parser. Call Next() to get the next HtmlToken,
which can be one one of 3 tag types or error. If a tag has attributes,
the caller has to parse them out (using HtmlToken::NextAttr()) */
class HtmlPullParser {
    const char* currPos = nullptr;
    const char* end = nullptr;

    const char* start = nullptr;
    size_t len = 0;

    HtmlToken currToken{};

  public:
    HtmlPullParser(const char* s, size_t len) : currPos(s), end(s + len), start(s), len(len) {
    }
    HtmlPullParser(const char* s, const char* end) : currPos(s), end(end), start(s), len(end - s) {
    }
    explicit HtmlPullParser(ByteSlice d)
        : currPos((char*)d.data()), end((char*)d.data() + d.size()), start((char*)d.data()), len(d.size()) {
    }

    void SetCurrPosOff(ptrdiff_t off) {
        currPos = start + off;
    }
    [[nodiscard]] size_t Len() const {
        return len;
    }
    [[nodiscard]] const char* Start() const {
        return start;
    }

    HtmlToken* Next();
};

bool SkipWs(const char*& s, const char* end);
bool SkipNonWs(const char*& s, const char* end);
bool SkipUntil(const char*& s, const char* end, char c);
bool SkipUntil(const char*& s, const char* end, const char* term);
bool IsSpaceOnly(const char* s, const char* end);

int HtmlEntityNameToRune(const char* name, size_t nameLen);
int HtmlEntityNameToRune(const WCHAR* name, size_t nameLen);

const char* ResolveHtmlEntity(const char* s, size_t len, int& rune);
const char* ResolveHtmlEntities(const char* s, const char* end, Allocator* alloc);
char* ResolveHtmlEntities(const char* s, size_t len);
