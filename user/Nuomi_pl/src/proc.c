/* ============================================================
 * proc.c —— 进程启动与控制（糯米壳语言的基本功）
 *
 * 为什么需要：启动器最终要把 java.exe 拉起来跑游戏 —— 传 classpath、
 * 主类、JVM 参数，还要实时抓它的 stdout/stderr 日志（"Loading 45%"），
 * 并且能让用户随时杀掉游戏进程。
 *
 * 平台：Windows 用 CreateProcessA + 匿名管道重定向子进程输出；
 *       其他平台暂未实现（将来接 novus 进程 syscall，脚本接口不变）。
 *
 * 脚本侧用法：
 *   (define h (proc-spawn "java" '("-Xmx2G" "-cp" "x.jar" "Main")))
 *   (while (proc-alive? h)
 *      (define line (proc-read h))
 *      (if (null? line) 0 (println line)))
 *   (println "退出码:" (proc-wait h))
 *   (proc-close h)
 * ============================================================ */

#include "nuomi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(NO_WIN_RUN)
#include <windows.h>

#define PROC_MAX 16          /* 同时可管理的子进程上限 */

typedef struct {
    int inUse;
    PROCESS_INFORMATION pi;
    HANDLE hRead;            /* 子进程 stdout/stderr 的读端 */
    char buf[4096];          /* 已读到但还没被取走的字节 */
    int bufLen;
    int eof;                 /* 管道已关闭（子进程结束且数据读完） */
} ProcSlot;

static ProcSlot g_procs[PROC_MAX];

static ProcSlot* slotAlloc(void) {
    for (int i = 0; i < PROC_MAX; i++)
        if (!g_procs[i].inUse) { memset(&g_procs[i], 0, sizeof(ProcSlot)); g_procs[i].inUse = 1; return &g_procs[i]; }
    return NULL;
}
static ProcSlot* slotOf(Value v) {
    if (!IS_NUM(v)) return NULL;
    long long id = (long long)AS_NUM(v);
    if (id < 0 || id >= PROC_MAX) return NULL;
    if (!g_procs[id].inUse) return NULL;
    return &g_procs[id];
}
/* 句柄用「槽位下标」表示，脚本侧就是个普通数字 */
static Value slotHandle(ProcSlot* s) { return NUM_VAL((double)(s - g_procs)); }

/* ---- 参数拼接：cmd + args[] → 一行命令行 ----
 * 含空格或制表符的参数加双引号；内部的双引号用 \" 转义。 */
static char* buildCmdLine(const char* cmd, Value args) {
    size_t cap = 256, len = 0;
    char* line = (char*)malloc(cap);
    if (!line) return NULL;
    line[0] = '\0';

    /* 先放 cmd 本身 */
    size_t cl = strlen(cmd);
    if (cl + 4 > cap) { cap = cl + 64; line = (char*)realloc(line, cap); }
    memcpy(line, cmd, cl); len = cl; line[len] = '\0';

    /* 逐个拼参数 */
    for (Value p = args; isList(p); p = listCdr(p)) {
        Value a = listCar(p);
        if (!IS_OBJ(a) || AS_OBJ(a)->type != OBJ_STRING) continue;
        ObjString* s = (ObjString*)AS_OBJ(a);
        bool needQuote = false;
        for (int i = 0; i < s->length; i++)
            if (s->chars[i] == ' ' || s->chars[i] == '\t') { needQuote = true; break; }
        /* 预留：空格 + 两个引号 + 转义余量 */
        size_t need = len + (size_t)s->length * 2 + 8;
        if (need > cap) { cap = need * 2; line = (char*)realloc(line, cap); }
        line[len++] = ' ';
        if (needQuote) line[len++] = '"';
        for (int i = 0; i < s->length; i++) {
            char c = s->chars[i];
            if (c == '"') line[len++] = '\\';   /* \" */
            line[len++] = c;
        }
        if (needQuote) line[len++] = '"';
        line[len] = '\0';
    }
    return line;
}

/* ---- 从管道往缓冲区补数据（非阻塞探测） ---- */
static void pump(ProcSlot* s) {
    if (s->eof || !s->hRead) return;
    DWORD avail = 0;
    if (!PeekNamedPipe(s->hRead, NULL, 0, NULL, &avail, NULL)) { s->eof = 1; return; }
    if (avail == 0) return;                    /* 暂无数据 */
    char tmp[1024];
    DWORD got = 0;
    if (!ReadFile(s->hRead, tmp, sizeof tmp, &got, NULL) || got == 0) { s->eof = 1; return; }
    int space = (int)sizeof(s->buf) - s->bufLen;
    if (space <= 0) return;                    /* 缓冲区满，等脚本取走 */
    int n = (int)got < space ? (int)got : space;
    memcpy(s->buf + s->bufLen, tmp, (size_t)n);
    s->bufLen += n;
}

/* ---- 阻塞等到至少有 1 字节或 EOF ---- */
static void pumpBlock(ProcSlot* s) {
    while (s->bufLen == 0 && !s->eof) {
        DWORD avail = 0;
        if (!PeekNamedPipe(s->hRead, NULL, 0, NULL, &avail, NULL)) { s->eof = 1; return; }
        if (avail == 0) {
            /* 子进程可能还没退出但暂时没输出：稍等再探 */
            Sleep(5);
            continue;
        }
        pump(s);
    }
}

/* (proc-spawn cmd args) → 句柄；失败 -1 */
static Value natProcSpawn(int c, Value* a, Env* e) {
    (void)e;
    if (c != 2) { runtimeError("proc-spawn expects (cmd args-list)"); return NUM_VAL(-1); }
    if (!IS_OBJ(a[0]) || AS_OBJ(a[0])->type != OBJ_STRING) {
        runtimeError("proc-spawn: cmd must be a string"); return NUM_VAL(-1);
    }
    ObjString* cmdS = (ObjString*)AS_OBJ(a[0]);
    char* cmdLine = buildCmdLine(cmdS->chars, a[1]);
    if (!cmdLine) { runtimeError("proc-spawn: out of memory"); return NUM_VAL(-1); }

    ProcSlot* s = slotAlloc();
    if (!s) { free(cmdLine); runtimeError("proc-spawn: too many processes"); return NUM_VAL(-1); }

    /* 匿名管道：子进程的 stdout/stderr 都写进来 */
    HANDLE rp = NULL, wp = NULL;
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rp, &wp, &sa, 0)) {
        free(cmdLine); s->inUse = 0;
        runtimeError("proc-spawn: CreatePipe failed"); return NUM_VAL(-1);
    }
    /* 读端必须不可继承，否则子进程攥着读端导致父进程永远等不到 EOF */
    if (!SetHandleInformation(rp, HANDLE_FLAG_INHERIT, 0)) { /* 失败不致命 */ }

    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wp;
    si.hStdError  = wp;

    memset(&s->pi, 0, sizeof s->pi);
    BOOL ok = CreateProcessA(
        NULL,           /* 用命令行指定程序 */
        cmdLine,        /* 可写的命令行缓冲 */
        NULL, NULL,     /* 默认安全属性 */
        TRUE,           /* 继承句柄（管道需要） */
        0,              /* 无特殊标志 */
        NULL,           /* 用父进程环境 */
        NULL,           /* 用父进程工作目录 */
        &si, &s->pi);
    free(cmdLine);

    /* 父进程不需要写端，立刻关掉（否则读不到 EOF） */
    CloseHandle(wp);

    if (!ok) {
        CloseHandle(rp);
        s->inUse = 0;
        runtimeError("proc-spawn: CreateProcess failed"); return NUM_VAL(-1);
    }
    s->hRead = rp;
    return slotHandle(s);
}

/* (proc-read h) → 一行字符串（不含换行）；EOF 返回 nil */
static Value natProcRead(int c, Value* a, Env* e) {
    (void)e;
    if (c != 1) { runtimeError("proc-read expects (handle)"); return NIL_VAL; }
    ProcSlot* s = slotOf(a[0]);
    if (!s) { runtimeError("proc-read: bad handle"); return NIL_VAL; }
    pumpBlock(s);
    /* 在缓冲里找换行 */
    for (int i = 0; i < s->bufLen; i++) {
        if (s->buf[i] == '\n') {
            int n = i;
            if (n > 0 && s->buf[n-1] == '\r') n--;      /* 去掉 \r */
            ObjString* out = internString(s->buf, n, false);
            /* 剩余字节前移 */
            int rest = s->bufLen - i - 1;
            memmove(s->buf, s->buf + i + 1, (size_t)rest);
            s->bufLen = rest;
            return OBJ_VAL(out);
        }
    }
    if (s->bufLen > 0) {
        /* EOF 且没有换行：把剩余一次给完 */
        ObjString* out = internString(s->buf, s->bufLen, false);
        s->bufLen = 0;
        return OBJ_VAL(out);
    }
    return NIL_VAL;                                    /* EOF 且无数据 */
}

/* (proc-read-all h) → 当前可读的全部文本；没有则空串 */
static Value natProcReadAll(int c, Value* a, Env* e) {
    (void)e;
    if (c != 1) { runtimeError("proc-read-all expects (handle)"); return NIL_VAL; }
    ProcSlot* s = slotOf(a[0]);
    if (!s) { runtimeError("proc-read-all: bad handle"); return NIL_VAL; }
    pump(s);
    if (s->bufLen == 0) return OBJ_VAL(internString("", 0, false));
    ObjString* out = internString(s->buf, s->bufLen, false);
    s->bufLen = 0;
    return OBJ_VAL(out);
}

/* (proc-alive? h) → 布尔 */
static Value natProcAlive(int c, Value* a, Env* e) {
    (void)e;
    if (c != 1) { runtimeError("proc-alive? expects (handle)"); return NIL_VAL; }
    ProcSlot* s = slotOf(a[0]);
    if (!s) { runtimeError("proc-alive?: bad handle"); return NIL_VAL; }
    DWORD code = 0;
    if (!GetExitCodeProcess(s->pi.hProcess, &code)) return BOOL_VAL(false);
    return BOOL_VAL(code == STILL_ACTIVE);
}

/* (proc-wait h) → 阻塞到结束，返回退出码 */
static Value natProcWait(int c, Value* a, Env* e) {
    (void)e;
    if (c != 1) { runtimeError("proc-wait expects (handle)"); return NIL_VAL; }
    ProcSlot* s = slotOf(a[0]);
    if (!s) { runtimeError("proc-wait: bad handle"); return NIL_VAL; }
    WaitForSingleObject(s->pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(s->pi.hProcess, &code);
    return NUM_VAL((double)code);
}

/* (proc-kill h) → 布尔 */
static Value natProcKill(int c, Value* a, Env* e) {
    (void)e;
    if (c != 1) { runtimeError("proc-kill expects (handle)"); return NIL_VAL; }
    ProcSlot* s = slotOf(a[0]);
    if (!s) { runtimeError("proc-kill: bad handle"); return NIL_VAL; }
    BOOL ok = TerminateProcess(s->pi.hProcess, 1);
    return BOOL_VAL(ok != 0);
}

/* (proc-close h) → 释放槽位与句柄 */
static Value natProcClose(int c, Value* a, Env* e) {
    (void)e;
    if (c != 1) { runtimeError("proc-close expects (handle)"); return NIL_VAL; }
    ProcSlot* s = slotOf(a[0]);
    if (!s) return NIL_VAL;
    if (s->hRead) { CloseHandle(s->hRead); s->hRead = NULL; }
    if (s->pi.hThread) { CloseHandle(s->pi.hThread); s->pi.hThread = NULL; }
    if (s->pi.hProcess) { CloseHandle(s->pi.hProcess); s->pi.hProcess = NULL; }
    s->inUse = 0;
    return NIL_VAL;
}

/* (proc-run cmd args) → 一把跑完：透传输出、返回退出码
 * 启动器「前台跑命令」的便捷形式（编译、校验之类）。 */
static Value natProcRun(int c, Value* a, Env* e) {
    (void)e;
    if (c != 2) { runtimeError("proc-run expects (cmd args-list)"); return NUM_VAL(-1); }
    Value hv = natProcSpawn(c, a, e);
    if (!IS_NUM(hv) || (long long)AS_NUM(hv) < 0) return NUM_VAL(-1);
    ProcSlot* s = slotOf(hv);
    /* 边跑边把子进程输出透传到本进程 stdout */
    for (;;) {
        Value line = natProcRead(1, &hv, e);
        if (IS_NIL(line)) break;
        if (!IS_OBJ(line) || AS_OBJ(line)->type != OBJ_STRING) break;
        ObjString* s2 = (ObjString*)AS_OBJ(line);
        fwrite(s2->chars, 1, (size_t)s2->length, stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    Value rc = natProcWait(1, &hv, e);
    natProcClose(1, &hv, e);
    return rc;
}

#else  /* ---- 非 Windows：暂未实现 ---- */
static Value natProcSpawn(int c, Value* a, Env* e) { (void)c;(void)a;(void)e; runtimeError("proc-spawn: not implemented on this platform yet"); return NUM_VAL(-1); }
static Value natProcRead(int c, Value* a, Env* e) { (void)c;(void)a;(void)e; runtimeError("proc-read: not implemented on this platform yet"); return NIL_VAL; }
static Value natProcReadAll(int c, Value* a, Env* e) { (void)c;(void)a;(void)e; runtimeError("proc-read-all: not implemented on this platform yet"); return NIL_VAL; }
static Value natProcAlive(int c, Value* a, Env* e) { (void)c;(void)a;(void)e; runtimeError("proc-alive?: not implemented on this platform yet"); return NIL_VAL; }
static Value natProcWait(int c, Value* a, Env* e) { (void)c;(void)a;(void)e; runtimeError("proc-wait: not implemented on this platform yet"); return NIL_VAL; }
static Value natProcKill(int c, Value* a, Env* e) { (void)c;(void)a;(void)e; runtimeError("proc-kill: not implemented on this platform yet"); return NIL_VAL; }
static Value natProcClose(int c, Value* a, Env* e) { (void)c;(void)a;(void)e; runtimeError("proc-close: not implemented on this platform yet"); return NIL_VAL; }
static Value natProcRun(int c, Value* a, Env* e) { (void)c;(void)a;(void)e; runtimeError("proc-run: not implemented on this platform yet"); return NIL_VAL; }
#endif

/* 注册（由 native.c 的 registerNatives 转发） */
void registerProcNatives(Env* g) {
    defNative(g, "proc-spawn",   natProcSpawn);
    defNative(g, "proc-read",    natProcRead);
    defNative(g, "proc-read-all",natProcReadAll);
    defNative(g, "proc-alive?",  natProcAlive);
    defNative(g, "proc-wait",    natProcWait);
    defNative(g, "proc-kill",    natProcKill);
    defNative(g, "proc-close",   natProcClose);
    defNative(g, "proc-run",     natProcRun);
}
