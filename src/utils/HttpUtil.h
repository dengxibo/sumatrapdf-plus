/* Copyright 2022 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

struct HttpRsp {
    AutoFreeStr url;
    StrBuilder data;
    DWORD error = (DWORD)-1;
    DWORD httpStatusCode = (DWORD)-1;

    HttpRsp() = default;
};

struct HttpProgress {
    i64 nDownloaded = 0;
    i64 nTotal = -1; // -1 if unknown (e.g. chunked transfer)
};

bool IsHttpRspOk(const HttpRsp*);

bool HttpPost(const char* server, int port, const char* url, StrBuilder* headers, StrBuilder* data);
bool HttpGet(const char* url, HttpRsp* rspOut);
bool HttpGetToFile(const char* url, const char* destFilePath, const Func1<HttpProgress*>& cbProgress);
