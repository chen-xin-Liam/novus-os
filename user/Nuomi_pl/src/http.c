// SPDX-License-Identifier: GPL-2.0
/*
 * http.c — HTTP(S) 请求原语
 *
 * 为什么糯米需要它：写启动器的第一件事就是联网——
 *   · 拉 Mojang 版本清单（piston-meta.mojang.com）
 *   · 拉版本元数据、资产索引
 *   · 查 Modrinth API（搜索 Mod、拉版本文件列表）
 *   · 下载 client.jar / 依赖库 / 资源
 * 配上 json.c，就能「读懂清单 → 下载文件 → 落盘」，启动器的主干就通了。
 *
 * 实现：WinINet（wininet.dll），**运行时动态加载**——TCC 既不带 wininet.h
 * 也不带导入库，LoadLibraryA + GetProcAddress 只依赖已链的 kernel32，
 * 不引第三方库、不改链接参数，TLS 走系统证书库。
 *
 * ★ 双通道设计（重要）
 * WinINet 有两套用法，能力与稳定性各有所长：
 *   简单 API：InternetOpenUrlA（一次到位，https 自动处理）
 *   高级 API：InternetConnect + HttpOpenRequest + HttpSendRequest
 *             （能自定义方法 HEAD、能加 Range 头、能查状态码/长度）
 * 本机实测：**高级 API 在 https 上会卡死**（TLS 初始化在 HttpOpenRequest 里挂住），
 * 而简单 API 一切正常；高级 API 在明文 http 上也正常。
 * 所以：
 *   · 默认按环境自动选：检测到 http_proxy/https_proxy 环境变量 → 简单 API
 *     （这类环境通常有代理，简单 API 更皮实）；否则 → 高级 API（功能全）
 *   · 可用环境变量 NUOMI_HTTP_MODE=simple|advanced 强制指定
 * 简单模式下的能力差异：没有 HEAD/Range（续传退化为整文件重下），
 * 状态码与 Content-Length 可能查不到（分块传输时本就查不到）。
 *
 * 将来跑在 novus 上时，这一层换成 novus 的网络 syscall，**脚本接口不变**。
 */
#include "nuomi.h"
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>

/* --- WinINet 常量（抄自 wininet.h，省得引头文件） --- */
#define INET_OPEN_DIRECT     1        /* INTERNET_OPEN_TYPE_DIRECT：直连 */
#define INET_SERVICE_HTTP    3        /* INTERNET_SERVICE_HTTP */
#define INET_FLAG_HTTPS      0x00800000u
#define INET_FLAG_RELOAD     0x80000000u
#define INET_FLAG_NO_CACHE   0x04000000u
#define INET_FLAG_NO_UI      0x00000300u
#define INET_OPT_RESP_CODE   38       /* INTERNET_OPTION_HTTP_RESPONSE_CODE */
#define INET_OPT_CONTENT_LEN 93       /* INTERNET_OPTION_CONTENT_LENGTH */
#define INET_OPT_CONN_TIMEOUT 2
#define INET_OPT_SEND_TIMEOUT 6
#define INET_OPT_RECV_TIMEOUT 5

/* --- 函数签名（x64 下 WINAPI 可忽略，写上更保险） --- */
typedef void* (WINAPI *fnIOpenA)(const char*, DWORD, const char*, const char*, DWORD);
typedef void* (WINAPI *fnIConnectA)(void*, const char*, WORD, const char*, const char*, DWORD, DWORD, DWORD);
typedef void* (WINAPI *fnHttpOpenA)(void*, const char*, const char*, const char*, const char*, DWORD, DWORD);
typedef BOOL  (WINAPI *fnHttpSendA)(void*, const char*, DWORD, void*, DWORD, size_t);
typedef BOOL  (WINAPI *fnIQueryOptA)(void*, DWORD, void*, DWORD*, DWORD*);
typedef BOOL  (WINAPI *fnIReadFile)(void*, void*, DWORD, DWORD*);
typedef BOOL  (WINAPI *fnICloseHandle)(void*);
typedef BOOL  (WINAPI *fnISetOptA)(void*, DWORD, void*, DWORD);
/* InternetOpenUrlA 真实签名就是 6 个参数，且第 3 个 lpszHeaders 就是自定义请求头
   —— 所以简单通道也能带 Range 头（断点续传），不必非用高级 API。 */
typedef void* (WINAPI *fnIOpenUrlA)(void*, const char*, const char*, DWORD, DWORD, void**);

typedef struct {
    int   loaded;
    fnIOpenA       Open;
    fnIConnectA    Connect;
    fnHttpOpenA    HttpOpen;
    fnHttpSendA    HttpSend;
    fnIQueryOptA   QueryOpt;
    fnIReadFile    ReadFile;
    fnICloseHandle CloseHandle;
    fnISetOptA     SetOpt;
    fnIOpenUrlA    OpenUrl;
} HttpApi;

static HttpApi g_api;

static const HttpApi* api_get(void) {
    if (g_api.loaded) return &g_api;
    HMODULE dll = LoadLibraryA("wininet.dll");
    if (!dll) return NULL;
    g_api.Open       = (fnIOpenA)       GetProcAddress(dll, "InternetOpenA");
    g_api.Connect    = (fnIConnectA)    GetProcAddress(dll, "InternetConnectA");
    g_api.HttpOpen   = (fnHttpOpenA)    GetProcAddress(dll, "HttpOpenRequestA");
    g_api.HttpSend   = (fnHttpSendA)    GetProcAddress(dll, "HttpSendRequestA");
    g_api.QueryOpt   = (fnIQueryOptA)   GetProcAddress(dll, "InternetQueryOptionA");
    g_api.ReadFile   = (fnIReadFile)    GetProcAddress(dll, "InternetReadFile");
    g_api.CloseHandle= (fnICloseHandle) GetProcAddress(dll, "InternetCloseHandle");
    g_api.SetOpt     = (fnISetOptA)     GetProcAddress(dll, "InternetSetOptionA");
    g_api.OpenUrl    = (fnIOpenUrlA)    GetProcAddress(dll, "InternetOpenUrlA");
    if (!g_api.Open || !g_api.ReadFile || !g_api.CloseHandle) { FreeLibrary(dll); return NULL; }
    g_api.loaded = 1;
    return &g_api;
}

/* 解析 URL：scheme://host[:port]/path?query */
static bool parse_url(const char* url, bool* https, char* host, size_t hostCap,
                      int* port, const char** path) {
    const char* p = url;
    if (strncmp(p, "https://", 8) == 0)     { *https = true;  p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { *https = false; p += 7; }
    else return false;
    const char* slash = strpbrk(p, "/?");
    size_t hostLen = slash ? (size_t)(slash - p) : strlen(p);
    const char* colon = (const char*)memchr(p, ':', hostLen);
    if (colon) { hostLen = (size_t)(colon - p); *port = atoi(colon + 1); }
    else *port = *https ? 443 : 80;
    if (hostLen == 0 || hostLen >= hostCap) return false;
    memcpy(host, p, hostLen);
    host[hostLen] = '\0';
    *path = slash ? slash : "/";
    return true;
}

typedef enum { MODE_AUTO = 0, MODE_SIMPLE, MODE_ADVANCED } HttpMode;

/* 选通道：环境变量优先，其次按有无代理变量自动判断 */
static HttpMode http_mode(void) {
    const char* m = getenv("NUOMI_HTTP_MODE");
    if (m) {
        if (strcmp(m, "simple") == 0)   return MODE_SIMPLE;
        if (strcmp(m, "advanced") == 0) return MODE_ADVANCED;
    }
    /* 有代理环境变量 → 简单 API（本机实测高级 API 的 https 会卡死） */
    if (getenv("https_proxy") || getenv("HTTPS_PROXY") ||
        getenv("http_proxy")  || getenv("HTTP_PROXY"))
        return MODE_SIMPLE;
    return MODE_ADVANCED;
}

typedef struct {
    int             status;      /* HTTP 状态码；未知/失败 = 0 */
    long long       length;      /* Content-Length；未知 = -1 */
    unsigned char*  body;
    size_t          bodyLen;
    bool            ranged;      /* 服务端真的做了部分传输（206） */
} HttpResp;

static void resp_free(HttpResp* r) {
    free(r->body);
    r->body = NULL;
    r->bodyLen = 0;
}

/* 把响应体读进内存（两个通道共用） */
static bool read_all(const HttpApi* A, void* h, HttpResp* out) {
    size_t cap = 65536;
    out->body = (unsigned char*)malloc(cap);
    if (!out->body) return false;
    out->bodyLen = 0;
    while (1) {
        DWORD got = 0;
        if (!A->ReadFile(h, out->body + out->bodyLen,
                         (DWORD)(cap - out->bodyLen), &got)) break;
        if (got == 0) break;                    /* 读完了 */
        out->bodyLen += got;
        if (out->bodyLen + 1 >= cap) {
            cap *= 2;
            unsigned char* nb = (unsigned char*)realloc(out->body, cap);
            if (!nb) { free(out->body); out->body = NULL; return false; }
            out->body = nb;                     /* realloc 可能搬家 */
        }
    }
    return true;
}

/* ---------- 高级 API 通道（功能全：HEAD / Range / 状态码 / 长度） ---------- */
static bool http_advanced(const char* url, const char* ua, const char* verb,
                          long long rangeStart, bool wantBody, HttpResp* out) {
    const HttpApi* A = api_get();
    if (!A || !A->Connect || !A->HttpOpen || !A->HttpSend || !A->QueryOpt) return false;

    bool https;
    char host[256];
    int port = 0;
    const char* path = NULL;
    if (!parse_url(url, &https, host, sizeof host, &port, &path)) return false;

    const char* agent = (ua && *ua) ? ua : "Nuomi/" NUOMI_VERSION " (nuomi-lang)";
    /* ⚠️ 绝不能加 INET_FLAG_RELOAD：本机实测带它 WinINet 会在这一步挂死
       （无 RELOAD 时同一调用完全正常）。它本意是"绕过缓存重取"，
       对启动器场景没必要，去掉更稳。 */
    DWORD flags = (https ? INET_FLAG_HTTPS : 0) | INET_FLAG_NO_UI;

    void* hNet = A->Open(agent, INET_OPEN_DIRECT, NULL, NULL, 0);
    if (!hNet) return false;
    DWORD tmo = 30000;
    if (A->SetOpt) {
        A->SetOpt(hNet, INET_OPT_CONN_TIMEOUT, &tmo, sizeof tmo);
        A->SetOpt(hNet, INET_OPT_SEND_TIMEOUT, &tmo, sizeof tmo);
        A->SetOpt(hNet, INET_OPT_RECV_TIMEOUT, &tmo, sizeof tmo);
    }
    void* hConn = A->Connect(hNet, host, (WORD)port, NULL, NULL, INET_SERVICE_HTTP, 0, 0);
    if (!hConn) { A->CloseHandle(hNet); return false; }
    void* hReq = A->HttpOpen(hConn, verb, path, NULL, NULL, flags, 0);
    if (!hReq) { A->CloseHandle(hConn); A->CloseHandle(hNet); return false; }

    /* Range 头：断点续传/分段下载 */
    char headers[128];
    const char* hdr = NULL;
    if (rangeStart >= 0) {
        const char* pre = "Range: bytes=";
        size_t pl = strlen(pre);
        memcpy(headers, pre, pl);
        unsigned long long uv = (unsigned long long)rangeStart;
        char digits[24];
        int di = 24;
        digits[--di] = '\0';
        if (uv == 0) digits[--di] = '0';
        while (uv > 0) { digits[--di] = (char)('0' + (int)(uv % 10)); uv /= 10; }
        size_t dl = strlen(digits);
        memcpy(headers + pl, digits, dl);
        size_t total = pl + dl;
        headers[total++] = '\r';
        headers[total++] = '\n';
        headers[total] = '\0';
        hdr = headers;
    }

    bool ok = A->HttpSend(hReq, hdr, hdr ? (DWORD)-1 : 0, NULL, 0, 0);
    if (ok) {
        /* 缓冲要给足：只用 4 字节时 WinINet 可能报 INSUFFICIENT_BUFFER 而查不到值 */
        DWORD buf[16];
        DWORD sz = sizeof buf;
        if (A->QueryOpt(hReq, INET_OPT_RESP_CODE, buf, &sz, NULL)) out->status = (int)buf[0];
        sz = sizeof buf;
        if (A->QueryOpt(hReq, INET_OPT_CONTENT_LEN, buf, &sz, NULL)) out->length = (long long)buf[0];
        out->ranged = (out->status == 206);
    }
    if (ok && wantBody && out->status >= 200 && out->status < 300)
        ok = read_all(A, hReq, out);

    A->CloseHandle(hReq);
    A->CloseHandle(hConn);
    A->CloseHandle(hNet);
    if (!ok) resp_free(out);
    return ok;
}

/* ---------- 简单 API 通道（皮实：https 稳，但无 Range / 可能查不到状态码） ---------- */
static bool http_simple(const char* url, const char* ua, const char* headers,
                        bool wantBody, HttpResp* out) {
    const HttpApi* A = api_get();
    if (!A || !A->OpenUrl) return false;
    const char* agent = (ua && *ua) ? ua : "Nuomi/" NUOMI_VERSION " (nuomi-lang)";
    void* hNet = A->Open(agent, INET_OPEN_DIRECT, NULL, NULL, 0);
    if (!hNet) return false;
    DWORD tmo = 30000;
    if (A->SetOpt) {
        A->SetOpt(hNet, INET_OPT_CONN_TIMEOUT, &tmo, sizeof tmo);
        A->SetOpt(hNet, INET_OPT_RECV_TIMEOUT, &tmo, sizeof tmo);
    }
    void* h = A->OpenUrl(hNet, url, headers, headers ? (DWORD)-1 : 0,
                         INET_FLAG_NO_CACHE, NULL);
    if (!h) { A->CloseHandle(hNet); return false; }
    bool ok = true;
    if (A->QueryOpt) {
        DWORD buf[16];
        DWORD sz = sizeof buf;
        if (A->QueryOpt(h, INET_OPT_RESP_CODE, buf, &sz, NULL)) out->status = (int)buf[0];
        sz = sizeof buf;
        if (A->QueryOpt(h, INET_OPT_CONTENT_LEN, buf, &sz, NULL)) out->length = (long long)buf[0];
    }
    if (wantBody) ok = read_all(A, h, out);
    A->CloseHandle(h);
    A->CloseHandle(hNet);
    if (!ok) resp_free(out);
    return ok;
}

/* 统一入口 */
static bool http_request(const char* url, const char* ua, const char* verb,
                         long long rangeStart, bool wantBody, HttpResp* out) {
    memset(out, 0, sizeof *out);
    out->status = 0;
    out->length = -1;
    HttpMode mode = http_mode();
    if (mode == MODE_ADVANCED)
        return http_advanced(url, ua, verb, rangeStart, wantBody, out);
    /* 简单通道：只支持 GET；但能带 Range 头（续传照样能用） */
    if (strcmp(verb, "GET") != 0) return false;
    char rangeHdr[64];
    const char* hdr = NULL;
    if (rangeStart >= 0) {
        unsigned long long uv = (unsigned long long)rangeStart;
        char digits[24];
        int di = 24;
        digits[--di] = '\0';
        if (uv == 0) digits[--di] = '0';
        while (uv > 0) { digits[--di] = (char)('0' + (int)(uv % 10)); uv /= 10; }
        /* ⚠️ 必须同时声明 Accept-Encoding: identity —— 否则服务器会回 gzip 压缩流，
           而带 Range 的响应 WinINet 不自动解压，追加到未压缩文件上就成了乱码。
           （实测：清单 278138 字节 → 压缩态只有 1947 字节） */
        /* ⚠️ 头块必须以 CRLF CRLF 结束 —— 简单 API 是把 headers 原样塞进请求的，
           少了空行结束符，代理会认为头不完整并回一个 HTML 错误页（实测 1947 字节）。
           同样要带 Accept-Encoding: identity，否则服务器回 gzip 压缩流，
           而带 Range 的响应 WinINet 不自动解压，追加到未压缩文件上就成了乱码。 */
        size_t n = (size_t)snprintf(rangeHdr, sizeof rangeHdr,
                                    "Range: bytes=%s-\r\nAccept-Encoding: identity\r\n\r\n",
                                    digits + di);
        if (n < sizeof rangeHdr) hdr = rangeHdr;
    }
    return http_simple(url, ua, hdr, wantBody, out);
}

/* 取出字符串参数 */
static const char* strArg(Value v) {
    if (!IS_OBJ(v) || AS_OBJ(v)->type != OBJ_STRING) return NULL;
    return ((ObjString*)AS_OBJ(v))->chars;
}
static const char* uaArg(int c, Value* a, int idx) {
    return (c > idx) ? strArg(a[idx]) : NULL;
}

/* ---------- 原语实现 ---------- */

/* (http-get url [ua]) -> 响应体字符串；失败 nil */
static Value natHttpGet(int c, Value* a, Env* e) {
    (void)e;
    const char* url = (c >= 1) ? strArg(a[0]) : NULL;
    if (!url) { runtimeError("http-get expects (url [user-agent])"); return NIL_VAL; }
    HttpResp r;
    if (!http_request(url, uaArg(c, a, 1), "GET", -1, true, &r)) {
        runtimeError("http-get: request failed");
        return NIL_VAL;
    }
    if (r.status != 0 && (r.status < 200 || r.status >= 300)) {
        runtimeError("http-get: HTTP status");
        resp_free(&r);
        return NIL_VAL;
    }
    ObjString* s = internString((const char*)(r.body ? (char*)r.body : ""), (int)r.bodyLen, false);
    resp_free(&r);
    retain(OBJ_VAL(s));
    return OBJ_VAL(s);
}

/* (http-head url [ua]) -> 状态码（200/404/...）；失败 -1 */
static Value natHttpHead(int c, Value* a, Env* e) {
    (void)e;
    const char* url = (c >= 1) ? strArg(a[0]) : NULL;
    if (!url) { runtimeError("http-head expects (url [user-agent])"); return NIL_VAL; }
    HttpResp r;
    if (!http_request(url, uaArg(c, a, 1), "HEAD", -1, false, &r)) {
        resp_free(&r);
        return NUM_VAL(-1);
    }
    int st = r.status;
    resp_free(&r);
    return NUM_VAL(st);
}

/* (http-length url [ua]) -> Content-Length；未知 -1 */
static Value natHttpLength(int c, Value* a, Env* e) {
    (void)e;
    const char* url = (c >= 1) ? strArg(a[0]) : NULL;
    if (!url) { runtimeError("http-length expects (url [user-agent])"); return NIL_VAL; }
    HttpResp r;
    if (!http_request(url, uaArg(c, a, 1), "HEAD", -1, false, &r)) {
        resp_free(&r);
        return NUM_VAL(-1);
    }
    long long len = r.length;
    resp_free(&r);
    return NUM_VAL((double)len);
}

/* (http-download url dest [ua]) -> 下载字节数；失败 -1 */
static Value natHttpDownload(int c, Value* a, Env* e) {
    (void)e;
    const char* url = (c >= 1) ? strArg(a[0]) : NULL;
    const char* dest = (c >= 2) ? strArg(a[1]) : NULL;
    if (!url || !dest) { runtimeError("http-download expects (url dest [user-agent])"); return NIL_VAL; }
    HttpResp r;
    if (!http_request(url, uaArg(c, a, 2), "GET", -1, true, &r)) {
        runtimeError("http-download: request failed");
        return NUM_VAL(-1);
    }
    if (r.status != 0 && (r.status < 200 || r.status >= 300)) {
        runtimeError("http-download: HTTP status");
        resp_free(&r);
        return NUM_VAL(-1);
    }
    FILE* f = fopen(dest, "wb");
    if (!f) {
        runtimeError("http-download: cannot open destination");
        resp_free(&r);
        return NUM_VAL(-1);
    }
    size_t wrote = r.bodyLen ? fwrite(r.body, 1, r.bodyLen, f) : 0;
    fclose(f);
    size_t total = r.bodyLen;
    resp_free(&r);
    return NUM_VAL((double)(wrote == total ? (long long)total : -1));
}

/* (http-download-from url dest offset [ua]) -> 续传写入字节数；失败 -1
   带 Range 请求并追加写，断点续传就靠它。
   ⚠️ 简单通道（NUOMI_HTTP_MODE=simple 或有代理环境变量时）不支持 Range，
      会退化成整文件重下（返回整文件字节数）。 */

/* 判断服务器的 Range 是否真的生效（决定追加还是覆盖）。
 *
 * 背景：WinINet 简单 API 拿不到 HTTP 状态码（InternetQueryOption 在 URL 句柄上
 * 一律 err=87），所以不能靠 206 判断「这是部分内容」。
 *
 * 改用内容自校验：请求头是 Range: bytes=<offset>-，服务器只有两种行为 ——
 *   · 遵守 Range → 返回从 offset 开始的**剩余部分**，其开头与「已下载文件的开头」不同；
 *   · 忽略 Range → 返回**整份**，其开头与「已下载文件的开头」相同。
 * 所以：开头相同 → 覆盖；开头不同 → 追加。
 * （offset=0 时两者必同 → 覆盖，符合预期。）
 *
 * 返回 true 表示「确认是部分内容，应追加」。 */
static bool range_is_partial(const char* dest,
                             const unsigned char* body, size_t bodyLen) {
    if (!body || bodyLen < 16) return false;
    FILE* f = fopen(dest, "rb");
    if (!f) return false;
    unsigned char head[16];
    size_t got = fread(head, 1, sizeof head, f);
    fclose(f);
    if (got < 16) return false;      /* 已下载部分太短，无法判别 → 保守按覆盖处理 */
    return memcmp(head, body, 16) != 0;
}

static Value natHttpDownloadFrom(int c, Value* a, Env* e) {
    (void)e;
    const char* url = (c >= 1) ? strArg(a[0]) : NULL;
    const char* dest = (c >= 2) ? strArg(a[1]) : NULL;
    long long offset = (c >= 3 && IS_NUM(a[2])) ? (long long)AS_NUM(a[2]) : -1;
    if (!url || !dest || offset < 0) {
        runtimeError("http-download-from expects (url dest offset [user-agent])");
        return NIL_VAL;
    }
    HttpResp r;
    if (!http_request(url, uaArg(c, a, 3), "GET", offset, true, &r)) {
        runtimeError("http-download-from: request failed");
        return NUM_VAL(-1);
    }
    if (r.status != 0 && (r.status < 200 || r.status >= 300)) {
        runtimeError("http-download-from: HTTP status");
        resp_free(&r);
        return NUM_VAL(-1);
    }
    /* 判定「服务端是否真的按 Range 做了部分传输」：
       · 高级通道有状态码，直接看 206；
       · 简单通道拿不到状态码（WinINet 在 URL 句柄上一律 err=87），
         改用内容自校验（见 range_honored 说明）。
       确认是部分内容 → 追加；否则视为整份 → 覆盖。 */
    bool partial = (r.status == 206) ||
                   (offset > 0 && range_is_partial(dest, (const unsigned char*)r.body, r.bodyLen));
    FILE* f = fopen(dest, partial ? "ab" : "wb");
    if (!f) {
        runtimeError("http-download-from: cannot open destination");
        resp_free(&r);
        return NUM_VAL(-1);
    }
    size_t wrote = r.bodyLen ? fwrite(r.body, 1, r.bodyLen, f) : 0;
    fclose(f);
    size_t total = r.bodyLen;
    resp_free(&r);
    return NUM_VAL((double)(wrote == total ? (long long)total : -1));
}

/* ---------- 给更新模块（src/update.c）用的纯 C 下载封装（复用 http_request） ---------- */

/* 把 HTTP 响应体取到内存。成功返回 true，*out 为 malloc 的 buffer（调用方 free）、
   *len 为长度；失败返回 false，err 写入错误信息。 */
bool nuomi_http_get_memory(const char* url, unsigned char** out, size_t* len,
                           char* err, size_t errcap) {
    HttpResp r;
    if (!http_request(url, "Nuomi/" NUOMI_VERSION " (nuomi-update)", "GET", -1, true, &r)) {
        if (err) snprintf(err, errcap, "request failed: %s", url);
        return false;
    }
    if (r.status != 0 && (r.status < 200 || r.status >= 300)) {
        if (err) snprintf(err, errcap, "HTTP %d: %s", r.status, url);
        resp_free(&r);
        return false;
    }
    *out = r.body;      /* 转移所有权，调用方负责 free（含失败时） */
    *len = r.bodyLen;
    return true;
}

/* 下载到文件（落盘）。成功返回 true，失败返回 false。 */
bool nuomi_http_download_to_file(const char* url, const char* dest,
                                 char* err, size_t errcap) {
    unsigned char* buf = NULL; size_t n = 0;
    if (!nuomi_http_get_memory(url, &buf, &n, err, errcap)) return false;
    FILE* f = fopen(dest, "wb");
    if (!f) {
        if (err) snprintf(err, errcap, "cannot open for write: %s", dest);
        free(buf);
        return false;
    }
    size_t wrote = n ? fwrite(buf, 1, n, f) : 0;
    fclose(f);
    free(buf);
    bool ok = (wrote == n);
    if (!ok && err) snprintf(err, errcap, "write incomplete: %s", dest);
    return ok;
}

#else  /* ---- 非 Windows：暂未实现（将来接 novus 网络 syscall） ---- */
static Value natHttpGet(int c, Value* a, Env* e) {
    (void)c; (void)a; (void)e;
    runtimeError("http-get: not implemented on this platform yet");
    return NIL_VAL;
}
static Value natHttpHead(int c, Value* a, Env* e) {
    (void)c; (void)a; (void)e;
    runtimeError("http-head: not implemented on this platform yet");
    return NUM_VAL(-1);
}
static Value natHttpLength(int c, Value* a, Env* e) {
    (void)c; (void)a; (void)e;
    runtimeError("http-length: not implemented on this platform yet");
    return NUM_VAL(-1);
}
static Value natHttpDownload(int c, Value* a, Env* e) {
    (void)c; (void)a; (void)e;
    runtimeError("http-download: not implemented on this platform yet");
    return NUM_VAL(-1);
}
static Value natHttpDownloadFrom(int c, Value* a, Env* e) {
    (void)c; (void)a; (void)e;
    runtimeError("http-download-from: not implemented on this platform yet");
    return NUM_VAL(-1);
}

/* 非 Windows：更新模块依赖的下载封装也不可用（将来接 novus 网络 syscall） */
bool nuomi_http_get_memory(const char* url, unsigned char** out, size_t* len, char* err, size_t errcap) {
    (void)url; (void)out; (void)len; (void)err; (void)errcap;
    if (err) snprintf(err, errcap, "HTTP not implemented on this platform yet");
    return false;
}
bool nuomi_http_download_to_file(const char* url, const char* dest, char* err, size_t errcap) {
    (void)url; (void)dest; (void)err; (void)errcap;
    if (err) snprintf(err, errcap, "HTTP not implemented on this platform yet");
    return false;
}
#endif

/* 注册（由 native.c 的 registerNatives 转发） */
void registerHttpNatives(Env* g) {
    defNative(g, "http-get", natHttpGet);
    defNative(g, "http-head", natHttpHead);
    defNative(g, "http-length", natHttpLength);
    defNative(g, "http-download", natHttpDownload);
    defNative(g, "http-download-from", natHttpDownloadFrom);
}