// SPDX-License-Identifier: GPL-2.0
/*
 * nuomi.h — Nuomi 语言解释器公共头文件
 * 一门用 C 从零手撸的极简 Lisp 风格编程语言。
 */
#ifndef NUOMI_H
#define NUOMI_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <math.h>
#include <setjmp.h>

/* ====================== 版本号 ======================
 * 与最新 git tag 对齐（见 `git tag`）。改版本时记得同步 README.md 顶部
 * 的「当前版本」与下载链接 tag，以及本文件。Nuomi 0.x 阶段迭代快，
 * 统一来源能避免 REPL 标语 / User-Agent / 更新检查各说各话。
 * 更新模块（src/update.c）拿它和 GitHub latest release 的 tag 比对。 */
#ifndef NUOMI_VERSION
#define NUOMI_VERSION "0.3.0"
#endif

/* HTTP 辅助接口（落地于 src/http.c，供更新模块复用 WinINet 实现） */
bool nuomi_http_get_memory(const char* url, unsigned char** out, size_t* len,
                           char* err, size_t errcap);
bool nuomi_http_download_to_file(const char* url, const char* dest,
                                 char* err, size_t errcap);

/* ====================== 更新模块（落地于 src/update.c） ====================== */
void nuomi_print_version(void);
int  nuomi_update(int argc, char** argv);

/* ====================== 值 (Value) ====================== */

typedef enum { VAL_NIL, VAL_BOOL, VAL_NUMBER, VAL_OBJ } ValueType;
typedef struct Obj Obj;

typedef struct {
    ValueType type;
    union {
        bool boolean;
        double number;
        Obj* obj;
    } as;
} Value;

#define IS_NIL(v)   ((v).type == VAL_NIL)
#define IS_BOOL(v)  ((v).type == VAL_BOOL)
#define IS_NUM(v)   ((v).type == VAL_NUMBER)
#define IS_OBJ(v)   ((v).type == VAL_OBJ)
#define AS_BOOL(v)  ((v).as.boolean)
#define AS_NUM(v)   ((v).as.number)
#define AS_OBJ(v)   ((v).as.obj)
#define NIL_VAL     ((Value){VAL_NIL, {0}})
#define BOOL_VAL(b) ((Value){VAL_BOOL, {.boolean = (b)}})
#define NUM_VAL(n)  ((Value){VAL_NUMBER, {.number = (n)}})
/* The cast is not decoration. Callers pass typed pointers - ObjString *,
 * Env *, ObjList * - and every one of those types starts with an Obj, but
 * a compiler that has been told to treat pointer mismatches as errors
 * refuses the assignment without it. The union member is Obj *; the
 * objects are all reachable through it by the layout above. */
#define OBJ_VAL(o)  ((Value){VAL_OBJ, {.obj = (Obj *)(o)}})

/* ====================== 对象 (Obj) ====================== */

typedef enum {
    OBJ_STRING, OBJ_LIST, OBJ_VECTOR, OBJ_FUNCTION, OBJ_ENV, OBJ_NATIVE
} ObjType;

struct Obj {
    ObjType type;
    int refcount;
    struct Obj* next;       /* 全局注册表，仅用于退出时彻底释放 */
#ifdef NUOMI_DEBUG
    unsigned long dbgSeq;   /* 分配序号，调试用 */
#endif
};

typedef struct {
    Obj obj;
    int length;
    bool isSymbol;          /* true = 标识符/符号，false = 字符串字面量 */
    char chars[];           /* 柔性数组，与结构体本身同一次 malloc */
} ObjString;

typedef struct {
    Obj obj;
    Value car;
    Value cdr;
} ObjList;

typedef struct {
    Obj obj;
    int count;
    int capacity;
    Value* items;
} ObjVector;

typedef struct Env Env;
typedef struct {
    Obj obj;
    ObjString* name;        /* 函数名（调试用） */
    int arity;
    ObjString** params;     /* 形参符号数组 */
    Value body;             /* 函数体（一个 Value 列表） */
    Env* env;               /* 闭包捕获的环境 */
} ObjFunction;

typedef Value (*NativeFn)(int argc, Value* argv, Env* env);
typedef struct {
    Obj obj;
    NativeFn fn;
    char* name;
} ObjNative;

struct Env {
    Obj obj;
    Env* parent;
    int count;
    int capacity;
    ObjString** keys;
    Value* values;
};

/* ====================== 列表辅助（内联） ====================== */
static inline bool isList(Value v) { return IS_OBJ(v) && AS_OBJ(v)->type == OBJ_LIST; }
static inline Value listCar(Value v) { return ((ObjList*)AS_OBJ(v))->car; }
static inline Value listCdr(Value v) { return ((ObjList*)AS_OBJ(v))->cdr; }
static inline int listLen(Value v) {
    int n = 0; while (isList(v)) { n++; v = listCdr(v); } return n;
}
static inline Value listRef(Value v, int idx) {
    for (int i = 0; i < idx && isList(v); i++) v = listCdr(v);
    return v;
}

/* ====================== 内存管理 ====================== */
Obj* allocObj(size_t size, ObjType type);   /* 内部：分配并初始化对象（refcount=1） */
void retain(Value v);
void release(Value v);
void freeAllObjects(void);

/* ====================== 字符串驻留 ====================== */
ObjString* internString(const char* chars, int len, bool isSymbol);
/* 当前执行阶段标签：调试版用于定位崩溃点；发布版仅被赋值、不被读取（无害）。 */
extern const char* g_dbg_phase;
void printValue(Value v, FILE* out);
char* valueToCString(Value v);   /* 返回 malloc 的字符串（raw：字符串不带引号） */

/* ====================== 读取器 ====================== */
typedef struct {
    const char* src;
    size_t len;
    size_t pos;
    bool error;
    bool done;             /* 已到末尾、没有更多表单 */
    const char* errmsg;
} Reader;
void skipWs(Reader* r);
Value readForm(Reader* r);

/* ====================== 环境 ====================== */
Env* newEnv(Env* parent);
void envDefine(Env* e, ObjString* key, Value val);
Value* envFind(Env* e, ObjString* key);

/* ====================== 求值器 ====================== */
bool isFalsey(Value v);
void runtimeError(const char* msg);
Value evalValue(Value node, Env* env);
ObjFunction* makeClosure(ObjString* name, Value paramList, Value body, Env* env);
Value apply(Value callee, Value* argv, int argc, Env* env);

/* 递归深度耗尽时跳出本次求值。
 * 逐层 unwind 会带着半构造的参数/环境继续跑，产生雪崩式的次生报错
 * （例如超深递归之后冒出 "- expects numbers"），对使用者毫无意义。
 * 一旦深度超限就直接 longjmp 到驱动层，报告一句清晰错误后放弃本次求值。 */
extern jmp_buf g_depth_jmp;
extern volatile int g_depth_overflow;   /* 1 = 已因超深递归跳出 */

/* ====================== 内置原生函数 ====================== */
void registerNatives(Env* g);
void registerProcNatives(Env* g);   /* 进程组（src/proc.c），由 registerNatives 转发 */
void registerJsonNatives(Env* g);   /* JSON 组（src/json.c），由 registerNatives 转发 */
void registerHttpNatives(Env* g);   /* HTTP 组（src/http.c），由 registerNatives 转发 */
void registerWmNatives(Env* g);    /* 窗口组（src/wm.c），由 registerNatives 转发 */
void defNative(Env* g, const char* name, NativeFn fn);   /* 原语注册器（native.c 实现，各原语文件共用） */

/* ====================== 驱动 ====================== */
void repl(Env* global);
void runFile(const char* path, Env* global);

#endif /* NUOMI_H */
