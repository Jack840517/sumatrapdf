/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "BaseUtil.h"
#include "SettingsUtil.h"
#include "SquareTreeParser.h"

static inline const StructInfo* GetSubstruct(const FieldInfo& field) {
    return (const StructInfo*)field.value;
}

static int ParseInt(const char* bytes) {
    bool negative = *bytes == '-';
    if (negative) {
        bytes++;
    }
    int value = 0;
    for (; str::IsDigit(*bytes); bytes++) {
        value = value * 10 + (*bytes - '0');
        // return 0 on overflow
        if (value - (negative ? 1 : 0) < 0) {
            return 0;
        }
    }
    return negative ? -value : value;
}

// only escape characters which are significant to SquareTreeParser:
// newlines and leading/trailing whitespace (and escape characters)
static bool NeedsEscaping(const char* s) {
    return str::IsWs(*s) || *s && str::IsWs(*(s + str::Len(s) - 1)) || str::FindChar(s, '\n') ||
           str::FindChar(s, '\r') || str::FindChar(s, '$');
}

static void EscapeStr(str::Str& out, const char* s) {
    CrashIf(!NeedsEscaping(s));
    if (str::IsWs(*s) && *s != '\n' && *s != '\r') {
        out.AppendChar('$');
    }
    for (const char* c = s; *c; c++) {
        switch (*c) {
            case '$':
                out.Append("$$");
                break;
            case '\n':
                out.Append("$n");
                break;
            case '\r':
                out.Append("$r");
                break;
            default:
                out.AppendChar(*c);
        }
    }
    if (*s && str::IsWs(s[str::Len(s) - 1])) {
        out.AppendChar('$');
    }
}

static char* UnescapeStr(const char* s) {
    if (!str::FindChar(s, '$')) {
        return str::Dup(s);
    }

    str::Str ret;
    const char* end = s + str::Len(s);
    if ('$' == *s && str::IsWs(*(s + 1))) {
        s++; // leading whitespace
    }
    for (const char* c = s; c < end; c++) {
        if (*c != '$') {
            ret.AppendChar(*c);
            continue;
        }
        switch (*++c) {
            case '$':
                ret.AppendChar('$');
                break;
            case 'n':
                ret.AppendChar('\n');
                break;
            case 'r':
                ret.AppendChar('\r');
                break;
            case '\0':
                break; // trailing whitespace
            default:
                // keep all other instances of the escape character
                ret.AppendChar('$');
                ret.AppendChar(*c);
                break;
        }
    }
    return ret.StealData();
}

// string arrays are serialized by quoting strings containing spaces
// or quotation marks (doubling quotation marks within quotes);
// this is simpler than full command line serialization as read by ParseCmdLine
static char* SerializeUtf8StringArray(const Vec<char*>* strArray) {
    str::Str serialized;

    for (size_t i = 0; i < strArray->size(); i++) {
        if (i > 0) {
            serialized.Append(' ');
        }
        const char* str = strArray->at(i);
        bool needsQuotes = !*str;
        for (const char* c = str; !needsQuotes && *c; c++) {
            needsQuotes = str::IsWs(*c) || '"' == *c;
        }
        if (!needsQuotes) {
            serialized.Append(str);
        } else {
            serialized.Append('"');
            for (const char* c = str; *c; c++) {
                if ('"' == *c) {
                    serialized.Append('"');
                }
                serialized.Append(*c);
            }
            serialized.Append('"');
        }
    }

    return (char*)serialized.StealData();
}

static void DeserializeUtf8StringArray(Vec<char*>* strArray, const char* serialized) {
    char* str = (char*)serialized;
    const char* s = str;

    for (;;) {
        while (str::IsWs(*s)) {
            s++;
        }
        if (!*s) {
            return;
        }
        if ('"' == *s) {
            str::Str part;
            for (s++; *s && (*s != '"' || *(s + 1) == '"'); s++) {
                if ('"' == *s) {
                    s++;
                }
                part.Append(*s);
            }
            strArray->Append(part.StealData());
            if ('"' == *s) {
                s++;
            }
        } else {
            const char* e;
            for (e = s; *e && !str::IsWs(*e); e++) {
                ;
            }
            strArray->Append(str::Dup(s, e - s));
            s = e;
        }
    }
}

static void FreeUtf8StringArray(Vec<char*>* strArray) {
    if (!strArray) {
        return;
    }
    strArray->FreeMembers();
    delete strArray;
}

static void FreeArray(Vec<void*>* array, const FieldInfo& field) {
    for (size_t j = 0; array && j < array->size(); j++) {
        FreeStruct(GetSubstruct(field), array->at(j));
    }
    delete array;
}

bool IsCompactable(const StructInfo* info) {
    for (size_t i = 0; i < info->fieldCount; i++) {
        switch (info->fields[i].type) {
            case SettingType::Bool:
            case SettingType::Int:
            case SettingType::Float:
            case SettingType::Color:
                continue;
            default:
                return false;
        }
    }
    return info->fieldCount > 0;
}

static_assert(sizeof(float) == sizeof(int) && sizeof(COLORREF) == sizeof(int),
              "compact array code can't be simplified if int, float and colorref are of different sizes");

static bool SerializeField(str::Str& out, const u8* base, const FieldInfo& field) {
    const u8* fieldPtr = base + field.offset;
    AutoFree value;

    switch (field.type) {
        case SettingType::Bool:
            out.Append(*(bool*)fieldPtr ? "true" : "false");
            return true;
        case SettingType::Int:
            out.AppendFmt("%d", *(int*)fieldPtr);
            return true;
        case SettingType::Float:
            out.AppendFmt("%g", *(float*)fieldPtr);
            return true;
        case SettingType::String:
        case SettingType::Color:
            if (!*(const char**)fieldPtr) {
                CrashIf(field.value);
                return false; // skip empty strings
            }
            if (!NeedsEscaping(*(const char**)fieldPtr)) {
                out.Append(*(const char**)fieldPtr);
            } else {
                EscapeStr(out, *(const char**)fieldPtr);
            }
            return true;
        case SettingType::Compact:
            CrashIf(!IsCompactable(GetSubstruct(field)));
            for (size_t i = 0; i < GetSubstruct(field)->fieldCount; i++) {
                if (i > 0) {
                    out.AppendChar(' ');
                }
                SerializeField(out, fieldPtr, GetSubstruct(field)->fields[i]);
            }
            return true;
        case SettingType::FloatArray:
        case SettingType::IntArray:
            for (size_t i = 0; i < (*(Vec<int>**)fieldPtr)->size(); i++) {
                FieldInfo info = {0};
                info.type = SettingType::Int;
                if (field.type == SettingType::FloatArray) {
                    info.type = SettingType::Float;
                }
                if (i > 0) {
                    out.AppendChar(' ');
                }
                SerializeField(out, (const u8*)&(*(Vec<int>**)fieldPtr)->at(i), info);
            }
            // prevent empty arrays from being replaced with the defaults
            return (*(Vec<int>**)fieldPtr)->size() > 0 || field.value != 0;
        case SettingType::ColorArray:
        case SettingType::StringArray:
            value.Set(SerializeUtf8StringArray(*(Vec<char*>**)fieldPtr));
            if (!NeedsEscaping(value)) {
                out.Append(value);
            } else {
                EscapeStr(out, value);
            }
            // prevent empty arrays from being replaced with the defaults
            return (*(Vec<char*>**)fieldPtr)->size() > 0 || field.value != 0;
        default:
            CrashIf(true);
            return false;
    }
}

// boolean true are "true", "yes" and any non-zero integer
static bool parseBool(const char* value) {
    if (str::StartsWithI(value, "true") && (!value[4] || str::IsWs(value[4]))) {
        return true;
    }
    if (str::StartsWithI(value, "yes") && (!value[3] || str::IsWs(value[3]))) {
        return true;
    }

    int i = ParseInt(value);
    return i != 0;
}

static void DeserializeField(const FieldInfo& field, u8* base, const char* value) {
    u8* fieldPtr = base + field.offset;

    char** strPtr = (char**)fieldPtr;
    WCHAR** wstrPtr = (WCHAR**)fieldPtr;
    bool* boolPtr = (bool*)fieldPtr;
    int* intPtr = (int*)fieldPtr;

    switch (field.type) {
        case SettingType::Bool:
            if (value) {
                *boolPtr = parseBool(value);
            } else {
                *boolPtr = field.value != 0;
            }
            break;

        case SettingType::Int:
            if (value) {
                *intPtr = ParseInt(value);
            } else {
                *intPtr = (int)field.value;
            }
            break;

        case SettingType::Float: {
            const char* s = value ? value : (const char*)field.value;
            str::Parse(s, "%f", (float*)fieldPtr);
            break;
        }

        case SettingType::Color:
        case SettingType::String:
            free(*strPtr);
            if (value) {
                *strPtr = UnescapeStr(value);
            } else {
                *strPtr = str::Dup((const char*)field.value);
            }
            break;
        case SettingType::Compact:
            CrashIf(!IsCompactable(GetSubstruct(field)));
            for (size_t i = 0; i < GetSubstruct(field)->fieldCount; i++) {
                if (value) {
                    for (; str::IsWs(*value); value++) {
                        ;
                    }
                    if (!*value) {
                        value = nullptr;
                    }
                }
                DeserializeField(GetSubstruct(field)->fields[i], fieldPtr, value);
                if (value) {
                    for (; *value && !str::IsWs(*value); value++) {
                        ;
                    }
                }
            }
            break;
        case SettingType::FloatArray:
        case SettingType::IntArray:
            if (!value) {
                value = (const char*)field.value;
            }
            delete *(Vec<int>**)fieldPtr;
            *(Vec<int>**)fieldPtr = new Vec<int>();
            while (value && *value) {
                FieldInfo info = {0};
                info.type = SettingType::IntArray == field.type     ? SettingType::Int
                            : SettingType::FloatArray == field.type ? SettingType::Float
                                                                    : SettingType::Color;
                DeserializeField(info, (u8*)(*(Vec<int>**)fieldPtr)->AppendBlanks(1), value);
                for (; *value && !str::IsWs(*value); value++) {
                    ;
                }
                for (; str::IsWs(*value); value++) {
                    ;
                }
            }
            break;
        case SettingType::ColorArray:
        case SettingType::StringArray:
            FreeUtf8StringArray(*(Vec<char*>**)fieldPtr);
            *(Vec<char*>**)fieldPtr = new Vec<char*>();
            if (value) {
                DeserializeUtf8StringArray(*(Vec<char*>**)fieldPtr, AutoFree(UnescapeStr(value)));
            } else if (field.value) {
                DeserializeUtf8StringArray(*(Vec<char*>**)fieldPtr, (const char*)field.value);
            }
            break;
        default:
            CrashIf(true);
    }
}

static inline void Indent(str::Str& out, int indent) {
    while (indent-- > 0) {
        out.AppendChar('\t');
    }
}

static void MarkFieldKnown(SquareTreeNode* node, const char* fieldName, SettingType type) {
    if (!node) {
        return;
    }
    size_t off = 0;
    if (SettingType::Struct == type || SettingType::Prerelease == type) {
        if (node->GetChild(fieldName, &off)) {
            delete node->data.at(off - 1).value.child;
            node->data.RemoveAt(off - 1);
        }
    } else if (SettingType::Array == type) {
        while (node->GetChild(fieldName, &off)) {
            delete node->data.at(off - 1).value.child;
            node->data.RemoveAt(off - 1);
            off--;
        }
    } else if (node->GetValue(fieldName, &off)) {
        node->data.RemoveAt(off - 1);
    }
}

static void SerializeUnknownFields(str::Str& out, SquareTreeNode* node, int indent) {
    if (!node) {
        return;
    }
    for (size_t i = 0; i < node->data.size(); i++) {
        SquareTreeNode::DataItem& item = node->data.at(i);
        Indent(out, indent);
        out.Append(item.key);
        if (item.isChild) {
            out.Append(" [\r\n");
            SerializeUnknownFields(out, item.value.child, indent + 1);
            Indent(out, indent);
            out.Append("]\r\n");
        } else {
            out.Append(" = ");
            out.Append(item.value.str);
            out.Append("\r\n");
        }
    }
}

static void SerializeStructRec(str::Str& out, const StructInfo* info, const void* data, SquareTreeNode* prevNode,
                               int indent = 0) {
    const u8* base = (const u8*)data;
    const char* fieldName = info->fieldNames;
    for (size_t i = 0; i < info->fieldCount; i++, fieldName += str::Len(fieldName) + 1) {
        const FieldInfo& field = info->fields[i];
        CrashIf(str::FindChar(fieldName, '=') || str::FindChar(fieldName, ':') || str::FindChar(fieldName, '[') ||
                str::FindChar(fieldName, ']') || NeedsEscaping(fieldName));
        if (SettingType::Struct == field.type || SettingType::Prerelease == field.type) {
#if !(defined(PRE_RELEASE_VER) || defined(DEBUG))
            if (SettingType::Prerelease == field.type) {
                continue;
            }
#endif
            Indent(out, indent);
            out.Append(fieldName);
            out.Append(" [\r\n");
            SerializeStructRec(out, GetSubstruct(field), base + field.offset,
                               prevNode ? prevNode->GetChild(fieldName) : nullptr, indent + 1);
            Indent(out, indent);
            out.Append("]\r\n");
        } else if (SettingType::Array == field.type) {
            Indent(out, indent);
            out.Append(fieldName);
            out.Append(" [\r\n");
            Vec<void*>* array = *(Vec<void*>**)(base + field.offset);
            if (array && array->size() > 0) {
                for (size_t j = 0; j < array->size(); j++) {
                    Indent(out, indent + 1);
                    out.Append("[\r\n");
                    SerializeStructRec(out, GetSubstruct(field), array->at(j), nullptr, indent + 2);
                    Indent(out, indent + 1);
                    out.Append("]\r\n");
                }
            }
            Indent(out, indent);
            out.Append("]\r\n");
        } else if (SettingType::Comment == field.type) {
            if (field.value) {
                Indent(out, indent);
                out.Append("# ");
                out.Append((const char*)field.value);
            }
            out.Append("\r\n");
        } else {
            size_t offset = out.size();
            Indent(out, indent);
            out.Append(fieldName);
            out.Append(" = ");
            bool keep = SerializeField(out, base, field);
            if (keep) {
                out.Append("\r\n");
            } else {
                out.RemoveAt(offset, out.size() - offset);
            }
        }
        MarkFieldKnown(prevNode, fieldName, field.type);
    }
    SerializeUnknownFields(out, prevNode, indent);
}

static void* DeserializeStructRec(const StructInfo* info, SquareTreeNode* node, u8* base, bool useDefaults) {
    if (!base) {
        base = AllocArray<u8>(info->structSize);
    }

    const char* fieldName = info->fieldNames;
    for (size_t i = 0; i < info->fieldCount; i++, fieldName += str::Len(fieldName) + 1) {
        const FieldInfo& field = info->fields[i];
        u8* fieldPtr = base + field.offset;
        if (SettingType::Struct == field.type || SettingType::Prerelease == field.type) {
            SquareTreeNode* child = node ? node->GetChild(fieldName) : nullptr;
#if !(defined(PRE_RELEASE_VER) || defined(DEBUG))
            if (SettingType::Prerelease == field.type) {
                child = nullptr;
            }
#endif
            DeserializeStructRec(GetSubstruct(field), child, fieldPtr, useDefaults);
        } else if (SettingType::Array == field.type) {
            SquareTreeNode *parent = node, *child = nullptr;
            if (parent && (child = parent->GetChild(fieldName)) != nullptr &&
                (0 == child->data.size() || child->GetChild(""))) {
                parent = child;
                fieldName += str::Len(fieldName);
            }
            if (child || useDefaults || !*(Vec<void*>**)fieldPtr) {
                Vec<void*>* array = new Vec<void*>();
                size_t idx = 0;
                while (parent && (child = parent->GetChild(fieldName, &idx)) != nullptr) {
                    array->Append(DeserializeStructRec(GetSubstruct(field), child, nullptr, true));
                }
                FreeArray(*(Vec<void*>**)fieldPtr, field);
                *(Vec<void*>**)fieldPtr = array;
            }
        } else if (field.type != SettingType::Comment) {
            const char* value = node ? node->GetValue(fieldName) : nullptr;
            if (useDefaults || value) {
                DeserializeField(field, base, value);
            }
        }
    }
    return base;
}

ByteSlice SerializeStruct(const StructInfo* info, const void* strct, const char* prevData) {
    str::Str out;
    out.Append(UTF8_BOM);
    SquareTree prevSqt(prevData);
    SerializeStructRec(out, info, strct, prevSqt.root);
    auto sv = out.StealAsView();
    return ToSpanU8(sv);
}

void* DeserializeStruct(const StructInfo* info, const char* data, void* strct) {
    SquareTree sqt(data);
    return DeserializeStructRec(info, sqt.root, (u8*)strct, !strct);
}

static void FreeStructData(const StructInfo* info, u8* base) {
    for (size_t i = 0; i < info->fieldCount; i++) {
        const FieldInfo& field = info->fields[i];
        u8* fieldPtr = base + field.offset;
        if (SettingType::Struct == field.type || SettingType::Prerelease == field.type) {
            FreeStructData(GetSubstruct(field), fieldPtr);
        } else if (SettingType::Array == field.type) {
            FreeArray(*(Vec<void*>**)fieldPtr, field);
        } else if (SettingType::String == field.type) {
            void* m = *((void**)fieldPtr);
            free(m);
        } else if (SettingType::FloatArray == field.type || SettingType::IntArray == field.type) {
            Vec<int>* v = *((Vec<int>**)fieldPtr);
            delete v;
        } else if (SettingType::StringArray == field.type || SettingType::ColorArray == field.type) {
            FreeUtf8StringArray(*(Vec<char*>**)fieldPtr);
        }
    }
}

void FreeStruct(const StructInfo* info, void* strct) {
    if (strct) {
        FreeStructData(info, (u8*)strct);
    }
    free(strct);
}
