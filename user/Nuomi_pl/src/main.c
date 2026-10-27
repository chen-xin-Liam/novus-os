// SPDX-License-Identifier: GPL-2.0
/*
 * main.c — 程序入口：REPL、脚本执行、以及编译模式（-c 输出 NUC 原生文件）
 */
#include "nuomi.h"
#include "codegen.h"

#ifdef _WIN32
#include <windows.h>
#ifndef CP_UTF8
#define CP_UTF8 65001u
#endif
/* Windows 控制台默认按 ANSI/GBK 代码页输出，源码中的 UTF-8 中文字面量会被
 * 误解码成乱码。把输出/输入代码页设为 UTF-8 即可正确渲染。
 * （Linux/macOS 终端本就按 UTF-8，无需处理。） */
static void setup_console_utf8(void) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}
#else
static void setup_console_utf8(void) { }
#endif

/* 由 src/nucutil.c 提供：NUC 工具链子命令（dump/validate/emit-demo/run）。
 * 调用约定见 nucutil.c 顶部注释。 */
int nuomi_nuc_main(int argc, char** argv);

void runFile(const char* path, Env* global) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char* buf = (char*)malloc(sz + 1);
    if (fread(buf, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read error\n"); exit(1); }
    buf[sz] = '\0';
    fclose(f);

    Reader r;
    r.src = buf; r.len = sz; r.pos = 0; r.error = false; r.done = false; r.errmsg = NULL;
    while (r.pos < r.len) {
        skipWs(&r);
        if (r.pos >= r.len) break;
        Value form = readForm(&r);
        if (r.error) { fprintf(stderr, "Parse error: %s\n", r.errmsg); break; }
        /* 每条顶层表达式各自设跳板：某条超深递归时不牵连后面的表达式。 */
        volatile int formDone = 0;
        Value res = NIL_VAL;
        g_depth_overflow = 0;                /* 允许本条表达式重新报一次深度错误 */
        if (setjmp(g_depth_jmp) == 0) {
            res = evalValue(form, global);
            formDone = 1;
        }
        if (formDone) {
            g_dbg_phase = "runFile-release-res";
            release(res);
        } else {
            /* 本条表达式因递归过深被放弃：脚本级执行就此终止，
               继续跑后面的表单没有意义（后面大概率同样依赖那层递归）。 */
            fprintf(stderr, "Execution stopped: recursion too deep.\n");
            break;
        }
#ifdef NUOMI_DEBUG
        fprintf(stderr, "  [runFile] release(form) seq=%lu\n", IS_OBJ(form) ? AS_OBJ(form)->dbgSeq : 0);
#endif
        g_dbg_phase = "runFile-release-form";
        release(form);
    }
    free(buf);
}

void repl(Env* global) {
    char* buf = NULL;
    size_t buflen = 0, bufcaps = 0;
    char line[8192];
    printf("Nuomi %s — 输入表达式回车求值，Ctrl+Z 回车 或 Ctrl+C 退出\n", NUOMI_VERSION);
    for (;;) {
        printf("nuomi> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        size_t ll = strlen(line);
        if (buflen + ll + 1 > bufcaps) {
            bufcaps = buflen + ll + 1 > 128 ? buflen + ll + 1 : 128;
            buf = (char*)realloc(buf, bufcaps);
        }
        memcpy(buf + buflen, line, ll);
        buflen += ll;
        buf[buflen] = '\0';

        Reader r;
        r.src = buf; r.len = buflen; r.pos = 0; r.error = false; r.done = false; r.errmsg = NULL;
        bool anyForm = false;
        while (r.pos < r.len) {
            skipWs(&r);
            if (r.pos >= r.len) break;
            Value form = readForm(&r);
            if (r.done) break;
            if (r.error) {
                if (strcmp(r.errmsg, "unexpected EOF") == 0) break; /* 不完整，继续读下一行 */
                fprintf(stderr, "Parse error: %s\n", r.errmsg);
                buflen = 0; buf[0] = '\0';
                anyForm = true;
                break;
            }
            /* 每个 REPL 输入各自设一次跳板：超深递归只放弃这一条表达式，
               REPL 本身继续可用（长跳转不会退回main，jmp_buf 是逐次覆盖的）。 */
            volatile int formDone = 0;
            Value res = NIL_VAL;
            g_depth_overflow = 0;            /* 允许本条表达式重新报一次深度错误 */
            if (setjmp(g_depth_jmp) == 0) {
                res = evalValue(form, global);
                formDone = 1;
            }
            if (formDone) {
                printf("=> ");
                printValue(res, stdout);
                printf("\n");
                fflush(stdout);
                release(res);
            }
            release(form);
            anyForm = 1;
        }
        if (anyForm && !r.error) {
            size_t consumed = r.pos;
            memmove(buf, buf + consumed, buflen - consumed);
            buflen -= consumed;
            buf[buflen] = '\0';
        }
    }
    free(buf);
}

int nuomiRealMain(int argc, char** argv);

#ifdef _WIN32
/* Windows：把解释器放到一个显式指定大小的栈上跑。
 * evalValue 对每个嵌套表达式消耗一层 C 栈，默认 1MB 主栈大约只撑得住
 * 500 层递归。这里开一个256MB 栈的线程，把可用的递归深度提到几万层，
 * 足以支撑树/图遍历这类真实算法。 */
#define NUOMI_STACK_BYTES (256u * 1024u * 1024u)

typedef struct { int argc; char** argv; int rc; } MainArgs;

static DWORD WINAPI nuomiThreadProc(LPVOID p) {
    MainArgs* ma = (MainArgs*)p;
    ma->rc = nuomiRealMain(ma->argc, ma->argv);
    return 0;
}

static int runOnBigStack(int argc, char** argv) {
    MainArgs ma; ma.argc = argc; ma.argv = argv; ma.rc = 0;
    HANDLE h = CreateThread(NULL, NUOMI_STACK_BYTES, nuomiThreadProc, &ma, 0, NULL);
    if (!h) return nuomiRealMain(argc, argv);   /* 建线程失败就退回主线程 */
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return ma.rc;                 /* 返回解释器实际返回码（线程过程自身返回值恒为 0，不可取） */
}
#else
#include <pthread.h>
/* POSIX 同理：pthread_attr_setstacksize 显式给一个大栈。 */
#define NUOMI_STACK_BYTES (256u * 1024u * 1024u)

typedef struct { int argc; char** argv; int rc; } MainArgs;

static void* nuomiThreadProc(void* p) {
    MainArgs* ma = (MainArgs*)p;
    ma->rc = nuomiRealMain(ma->argc, ma->argv);
    return NULL;
}

static int runOnBigStack(int argc, char** argv) {
    MainArgs ma; ma.argc = argc; ma.argv = argv; ma.rc = 0;
    pthread_attr_t attr;
    pthread_t tid;
    if (pthread_attr_init(&attr) != 0) return nuomiRealMain(argc, argv);
    pthread_attr_setstacksize(&attr, NUOMI_STACK_BYTES);
    if (pthread_create(&tid, &attr, nuomiThreadProc, &ma) != 0) {
        pthread_attr_destroy(&attr);
        return nuomiRealMain(argc, argv);
    }
    pthread_join(tid, NULL);
    pthread_attr_destroy(&attr);
    return ma.rc;
}
#endif

int main(int argc, char** argv) {
    return runOnBigStack(argc, argv);
}

static int nuomiRealMainInner(int argc, char** argv);

int nuomiRealMain(int argc, char** argv) {
    return nuomiRealMainInner(argc, argv);
}

static int nuomiRealMainInner(int argc, char** argv) {
    setup_console_utf8();   /* Windows: 控制台按 UTF-8 输出，修复 REPL 中文标语乱码 */

    /* 顶层子命令：version / update —— 不进 REPL、不读脚本，提前返回。
       （放在 newEnv 之前：这两条命令都不需要解释器环境。） */
    if (argc >= 2) {
        if (strcmp(argv[1], "version") == 0 ||
            strcmp(argv[1], "--version") == 0 ||
            strcmp(argv[1], "-v") == 0) {
            nuomi_print_version();
            return 0;
        }
        if (strcmp(argv[1], "update") == 0) {
            return nuomi_update(argc - 1, argv + 1);
        }
        /* NUC 工具链子命令：nuomi nuc <dump|validate|emit-demo|run> <file> */
        if (strcmp(argv[1], "nuc") == 0) {
            return nuomi_nuc_main(argc - 1, argv + 1);
        }
    }

    Env* global = newEnv(NULL);
    registerNatives(global);

    /* 解析参数：编译模式 nuomi -c <in> -o <out.nuc> */
    bool compileMode = false;
    const char* inPath = NULL;
    const char* outPath = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--compile") == 0) compileMode = true;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outPath = argv[++i];
        else if (!inPath) inPath = argv[i];
    }

    if (compileMode) {
        if (!inPath) { fprintf(stderr, "compile mode needs an input file: nuomi -c <in.nuomi> -o <out.nuc>\n"); return 1; }
        FILE* f = fopen(inPath, "rb");
        if (!f) { fprintf(stderr, "cannot open %s\n", inPath); return 1; }
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char* src = (char*)malloc((size_t)sz + 1);
        if (fread(src, 1, (size_t)sz, f) != (size_t)sz) { fprintf(stderr, "read error\n"); fclose(f); free(src); return 1; }
        src[sz] = '\0'; fclose(f);

        uint8_t* blob = NULL; size_t n = 0; char err[256];
        int rc = nuomi_compile_to_nuc(src, &blob, &n, err, sizeof err);
        free(src);
        if (rc != 0) { fprintf(stderr, "compile failed: %s\n", err); return 1; }

        const char* op = outPath ? outPath : "a.nuc";
        FILE* of = fopen(op, "wb");
        if (!of) { fprintf(stderr, "cannot write %s\n", op); free(blob); return 1; }
        fwrite(blob, 1, n, of); fclose(of); free(blob);
        printf("compiled %s -> %s (%zu bytes)\n", inPath, op, n);
        release(OBJ_VAL(global));
        freeAllObjects();
        return 0;
    }

    if (inPath) {
        runFile(inPath, global);
    } else {
        repl(global);
    }

#ifdef NUOMI_DEBUG
    fprintf(stderr, "=== main: release(global) ===\n");
#endif
    g_dbg_phase = "main-release-global";
    release(OBJ_VAL(global));
    freeAllObjects();
    return 0;
}
