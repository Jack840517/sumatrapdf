/* Copyright 2021 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

// must be #included from Regress.cpp

// test that a given epub file loads correctly. crash otherwise
static void RegressTestEpubLoading(const WCHAR *fileName)
{
    WCHAR *filePath = path::Join(TestFilesDir(), fileName);
    VerifyFileExists(filePath);
    Kind kind = GuessFileType(filePath, true);
    CrashAlwaysIf(!EpubDoc::IsSupportedFileType(kind));
    EpubDoc *doc = EpubDoc::CreateFromFile(filePath);
    CrashAlwaysIf(!doc);
    delete doc;
}

// http://code.google.com/p/sumatrapdf/issues/detail?id=2102
static void Regress02()
{
    RegressTestEpubLoading(L"epub\\sumatra-crash-nov-23-2012.epub");
}

// http://code.google.com/p/sumatrapdf/issues/detail?id=2091
static void Regress01()
{
    RegressTestEpubLoading(L"epub\\sumatra-crash-nov-12-2012.epub");
}

// http://code.google.com/p/sumatrapdf/issues/detail?id=1926
static void Regress00()
{
    WCHAR *filePath = path::Join(TestFilesDir(), L"epub\\widget-figure-gallery-20120405.epub");
    VerifyFileExists(filePath);
    Kind kind = GuessFileType(filePath, true);
    CrashAlwaysIf(!EpubDoc::IsSupportedFileType(kind));
    EpubDoc *doc = EpubDoc::CreateFromFile(filePath);
    CrashAlwaysIf(!doc);

    PoolAllocator textAllocator;
    HtmlFormatterArgs *args = CreateFormatterDefaultArgs(820, 920, &textAllocator);
    if (!args) {
        return;
    }
    args->htmlStr = doc->GetHtmlData();
    HtmlPage *pages[3];
    HtmlFormatter *formatter = new EpubFormatter(args, doc);
    int page = 0;
    for (HtmlPage *pd = formatter->Next(); pd; pd = formatter->Next()) {
        pages[page++] = pd;
        if (page == dimof(pages))
            break;
    }
    delete formatter;
    delete args;
    CrashAlwaysIf(page != 3);

    args = CreateFormatterDefaultArgs(820, 920, &textAllocator);
    args->htmlStr = doc->GetHtmlData();
    args->reparseIdx = pages[2]->reparseIdx;
    formatter = new EpubFormatter(args, doc);
    // if bug is present, this will crash in formatter->Next()
    for (HtmlPage *pd = formatter->Next(); pd; pd = formatter->Next()) {
        delete pd;
    }
    delete formatter;
    delete args;
    delete doc;
}
