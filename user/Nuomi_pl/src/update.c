// SPDX-License-Identifier: GPL-2.0
/*
 * update.c — Nuomi 自更新命令
 *
 *   nuomi version           显示当前版本
 *   nuomi update            检查 GitHub latest release，有新版本则下载并自替换
 *   nuomi update --check    只检查、不下载
 *   nuomi update --url U    用自定义 API 地址（自托管 / 测试用）
 *   nuomi update --exe P    更新指定路径的 exe（测试用，默认更新正在运行的自身）
 *
 * 复用 src/http.c 的 WinINet 封装（nuomi_http_get_memory /
 * nuomi_http_download_to_file），不引第三方库。
 *
 * 国内网络直连 api.github.com 常被 GFW 干扰（超时/重置/返回拦截页），导致
 * 抓不到 tag_name。故查询与下载都走「官方直连 + 国内代理前缀」回退：依次尝试，
 * 第一个拉通/解析到 tag 的即用。代理随时间可能变动，增删 NUOMI_UPDATE_PROXIES 即可。
 *
 * 自替换原理（Windows）：正在运行的 exe 可以被「重命名」（MoveFile 同卷重命名
 * 成功），但不能被删除。所以流程是 旧exe -> .old，新下载的 .new -> 旧exe 路径；
 * 任一步失败则把 .old 恢复回去。当前进程仍跑旧内存映像，重启后生效。
 */
#include "nuomi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#ifndef DEFAULT_API_URL
#define DEFAULT_API_URL "https://api.github.com/repos/Ryan-cpu-lang/Nuomi/releases/latest"
#endif

/* 国内代理前缀回退表：拼在官方 URL 前（ghproxy 系前缀代理）。
 * 第 1 项为空 = 官方直连；用户用 --url 自定义源时只直连、不套代理。 */
#ifndef NUOMI_UPDATE_PROXIES
#define NUOMI_UPDATE_PROXIES \
    "", \
    "https://ghproxy.net/", \
    "https://ghfast.top/", \
    "https://gh-proxy.com/"
#endif

/* ---------- 版本解析与比较 ---------- */

/* 解析 "v0.3.0" / "0.3.0-m2" -> major.minor.patch + 是否有 pre-release 后缀。
   返回 true 表示至少解析到一个数字。 */
static bool parse_version(const char* s, long* major, long* minor, long* patch, int* hasPre) {
    *major = *minor = *patch = 0; *hasPre = 0;
    if (!s || !*s) return false;
    while (*s && (*s < '0' || *s > '9')) s++;          /* 跳过 'v' 等非数字前缀 */
    if (!*s) return false;
    *major = strtol(s, (char**)&s, 10);
    if (*s == '.') { s++; *minor = strtol(s, (char**)&s, 10); }
    if (*s == '.') { s++; *patch = strtol(s, (char**)&s, 10); }
    if (*s == '-' || *s == '+') *hasPre = 1;            /* pre-release / build 元数据 */
    return true;
}

/* a 比 b 新返回 >0；相等 0；a 更旧 <0。版本号相同则正式版（无后缀）更大。 */
static int compare_version(const char* a, const char* b) {
    long am = 0, an = 0, ap = 0, bm = 0, bn = 0, bp = 0;
    int ah = 0, bh = 0;
    parse_version(a, &am, &an, &ap, &ah);
    parse_version(b, &bm, &bn, &bp, &bh);
    if (am != bm) return (am > bm) ? 1 : -1;
    if (an != bn) return (an > bn) ? 1 : -1;
    if (ap != bp) return (ap > bp) ? 1 : -1;
    if (ah != bh) return ah ? -1 : 1;                   /* 正式版 > 预发布版 */
    return 0;
}

/* ---------- 极简 JSON 提取（针对 GitHub releases API 固定结构，不写全量解析器） ---------- */

static const char* find_sub(const char* hay, size_t hlen, const char* needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen) return NULL;
    for (size_t i = 0; i + nlen <= hlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0) return hay + i;
    return NULL;
}

static const char* find_last_slash(const char* s, size_t len) {
    const char* r = NULL;
    for (size_t i = 0; i < len; i++) if (s[i] == '\\' || s[i] == '/') r = s + i;
    return r;
}

/* 提取 "tag_name":"..." 中的版本串，返回 malloc 的串（调用方 free）。 */
static char* extract_tag(const char* json, size_t len) {
    const char* key = "\"tag_name\":";
    const char* p = find_sub(json, len, key);
    if (!p) return NULL;
    p += strlen(key);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    const char* end = p;
    while (*end && *end != '"') end++;
    size_t n = (size_t)(end - p);
    char* out = (char*)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, p, n); out[n] = '\0';
    return out;
}

/* 收集所有 "browser_download_url":"..." 中文件名等于 name 的 URL，写入 outUrls。
   返回 false 表示数组已满。每个 URL 为 malloc 的串（调用方 free）。 */
static bool collect_asset(const char* json, size_t len, const char* name,
                          char** outUrls, int maxUrls, int* count) {
    const char* key = "\"browser_download_url\":";
    size_t klen = strlen(key);
    const char* p = json;
    size_t rem = len;
    while ((p = find_sub(p, rem, key)) != NULL) {
        p += klen; rem -= klen;
        while (*p == ' ' || *p == '\t') { p++; rem--; }
        if (*p != '"') break;
        p++; rem--;
        const char* end = p;
        while (*end && *end != '"') end++;
        size_t n = (size_t)(end - p);
        const char* slash = find_last_slash(p, n);
        const char* fname = slash ? slash + 1 : p;
        size_t fnlen = n - (size_t)(fname - p);
        if (fnlen == strlen(name) && memcmp(fname, name, fnlen) == 0) {
            if (*count >= maxUrls) return false;
            char* u = (char*)malloc(n + 1);
            if (!u) return false;
            memcpy(u, p, n); u[n] = '\0';
            outUrls[(*count)++] = u;
        }
        p = end + 1; rem = len - (size_t)(p - json);
    }
    return true;
}

/* ---------- 文件工具 ---------- */

static bool file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

/* 取目录名（保留末尾分隔符；无分隔符时返回 "./"）。 */
static void dir_of(const char* path, char* out, size_t outcap) {
    size_t n = strlen(path);
    size_t last = (size_t)-1;
    for (size_t i = 0; i < n; i++) if (path[i] == '\\' || path[i] == '/') last = i;
    if (last == (size_t)-1) { snprintf(out, outcap, "./"); return; }
    size_t c = last + 1 < outcap ? last + 1 : outcap - 1;
    memcpy(out, path, c); out[c] = '\0';
}

/* 自替换：把 dest 备份为 oldfile，再把 newfile 改名成 dest；失败则回滚。 */
static bool self_replace(const char* dest, const char* newfile, const char* oldfile,
                         char* err, size_t errcap) {
    if (file_exists(dest)) {
        if (rename(dest, oldfile) != 0) {
            if (err) snprintf(err, errcap, "无法备份当前文件: %s", dest);
            return false;
        }
    }
    if (rename(newfile, dest) != 0) {
        rename(oldfile, dest);                          /* 回滚 */
        if (err) snprintf(err, errcap, "替换失败，已回滚到原版本");
        return false;
    }
    remove(oldfile);
    return true;
}

/* ---------- 公共入口 ---------- */

void nuomi_print_version(void) {
#ifdef _WIN32
    printf("Nuomi %s (windows)\n", NUOMI_VERSION);
#else
    printf("Nuomi %s (posix)\n", NUOMI_VERSION);
#endif
}

int nuomi_update(int argc, char** argv) {
    bool check_only = false;
    bool custom_url = false;
    const char* api_url = DEFAULT_API_URL;
    const char* exe_override = NULL;
    /* 官方直连 + 国内代理前缀回退；--url 自定义源时只走直连（不套代理） */
    static const char* kPrefix[] = { NUOMI_UPDATE_PROXIES, NULL };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) check_only = true;
        else if (strcmp(argv[i], "--url") == 0 && i + 1 < argc) { api_url = argv[++i]; custom_url = true; }
        else if (strcmp(argv[i], "--exe") == 0 && i + 1 < argc) exe_override = argv[++i];
        else {
            fprintf(stderr, "nuomi update: 未知选项 %s\n", argv[i]);
            fprintf(stderr, "用法: nuomi update [--check] [--url <api>] [--exe <path>]\n");
            return 2;
        }
    }

    /* 自身 exe 路径 */
    char selfPath[1024];
#ifdef _WIN32
    if (exe_override) {
        strncpy(selfPath, exe_override, sizeof selfPath - 1); selfPath[sizeof selfPath - 1] = '\0';
    } else {
        DWORD n = GetModuleFileNameA(NULL, selfPath, sizeof selfPath);
        if (n == 0 || n >= sizeof selfPath) {
            fprintf(stderr, "更新失败：无法获取自身路径\n");
            return 1;
        }
    }
#else
    if (exe_override) {
        strncpy(selfPath, exe_override, sizeof selfPath - 1); selfPath[sizeof selfPath - 1] = '\0';
    } else {
        const char* p = getenv("_NUOMI_SELF");
        strncpy(selfPath, p ? p : "nuomi", sizeof selfPath - 1); selfPath[sizeof selfPath - 1] = '\0';
    }
#endif

    char dir[1024]; dir_of(selfPath, dir, sizeof dir);

    /* 1) 查 latest release：官方直连 + 国内代理前缀回退 */
    unsigned char* json = NULL; size_t jlen = 0; char err[256];
    char* tag = NULL;
    char urlBuf[1600];
    char* assets[8]; int nassets = 0;

    printf("正在检查更新（%s）...\n", api_url);
    for (int pi = 0; kPrefix[pi]; pi++) {
        if (pi > 0 && custom_url) break;                /* 自定义源只直连 */
        snprintf(urlBuf, sizeof urlBuf, "%s%s", kPrefix[pi], api_url);
        if (pi > 0) printf("直连失败，改用镜像：%s\n", kPrefix[pi]);
        if (nuomi_http_get_memory(urlBuf, &json, &jlen, err, sizeof err)) {
            tag = extract_tag((const char*)json, jlen);
            if (tag) {
                collect_asset((const char*)json, jlen, "nuomi.exe", assets, 8, &nassets);
                break;
            }
            free(json); json = NULL; jlen = 0;          /* 拿到内容但无 tag（如拦截页），换下一个源 */
        } else {
            fprintf(stderr, "  （%s 拉取失败：%s）\n", kPrefix[pi][0] ? kPrefix[pi] : "官方", err);
        }
    }
    if (!tag) {
        fprintf(stderr, "无法从更新源解析出版本号（官方直连与各镜像均失败）。\n");
        fprintf(stderr, "如在国内，可开 VPN 后重试；或到 Releases 页手动下载 nuomi.exe。\n");
        free(json);
        return 1;
    }

    int rc = 0;
    int cmp = compare_version(tag, NUOMI_VERSION);
    printf("当前版本：%s\n", NUOMI_VERSION);
    printf("最新版本：%s\n", tag);

    if (cmp <= 0) {
        printf("已是最新版本，无需更新。\n");
        goto cleanup;
    }

    if (check_only) {
        printf("有可用更新：%s -> %s（运行 `nuomi update` 进行更新）\n", NUOMI_VERSION, tag);
        goto cleanup;
    }

    if (nassets == 0) {
        fprintf(stderr, "最新版本 %s 未提供 nuomi.exe 下载，无法自动更新。\n", tag);
        rc = 1;
        goto cleanup;
    }

    /* 2) 下载 nuomi.exe（自身）：asset 是 github.com 直链，同样走代理前缀回退 */
    const char* nuomi_url = assets[0];
    char newPath[1100], oldPath[1100], dlBuf[1600];
    snprintf(newPath, sizeof newPath, "%s.new", selfPath);
    snprintf(oldPath, sizeof oldPath, "%s.old", selfPath);
    bool got = false;
    for (int pi = 0; kPrefix[pi]; pi++) {
        if (pi > 0 && custom_url) break;
        snprintf(dlBuf, sizeof dlBuf, "%s%s", kPrefix[pi], nuomi_url);
        printf("正在下载 nuomi.exe（%s）...\n", kPrefix[pi][0] ? kPrefix[pi] : "官方");
        if (nuomi_http_download_to_file(dlBuf, newPath, err, sizeof err)) { got = true; break; }
        fprintf(stderr, "  下载失败（%s）：%s\n", kPrefix[pi][0] ? kPrefix[pi] : "官方", err);
    }
    if (!got) {
        fprintf(stderr, "下载失败：所有源都无法下载 nuomi.exe。\n");
        rc = 1;
        goto cleanup;
    }
    printf("正在替换...\n");
    if (!self_replace(selfPath, newPath, oldPath, err, sizeof err)) {
        fprintf(stderr, "更新失败：%s\n", err);
        rc = 1;
        goto cleanup;
    }
    printf("nuomi 已更新到 %s。\n", tag);

    printf("更新完成。当前进程仍是旧版本，请重新启动 nuomi 使更改生效。\n");

cleanup:
    free(json);
    free(tag);
    for (int i = 0; i < nassets; i++) free(assets[i]);
    return rc;
}
