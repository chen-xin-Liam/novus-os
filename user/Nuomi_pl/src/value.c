// SPDX-License-Identifier: GPL-2.0
/*
 * value.c — 值的内存管理（引用计数 + 全局注册表）、字符串驻留、值打印
 */
#include "nuomi.h"

/* ====================== 全局注册表 ====================== */
static Obj* g_objects = NULL;

/* 阶段标签：始终存在；调试版用于崩溃定位，发布版仅赋值不读取。 */
const char* g_dbg_phase = "start";

static void registerObj(Obj* o) { o->next = g_objects; g_objects = o; }

/* ====================== 调试：对象存活检查 ====================== */
/* 编译时加 -DNUOMI_DEBUG 开启：用「存活集合」捕获释放后再次访问
   (use-after-free) / 重复释放这类引用计数 bug，直接 abort 并报类型。 */
#ifdef NUOMI_DEBUG
#include <stdint.h>
#define LIVE_CAP 16384
static Obj* g_live[LIVE_CAP];
static unsigned liveHash(Obj* o) { return ((uintptr_t)o >> 4) % LIVE_CAP; }
static bool liveContains(Obj* o) {
    unsigned h = liveHash(o);
    for (int i = 0; i < LIVE_CAP; i++) {
        unsigned idx = (h + i) % LIVE_CAP;
        if (!g_live[idx]) return false;
        if (g_live[idx] == o) return true;
    }
    return false;
}
static void liveAdd(Obj* o) {
    unsigned h = liveHash(o);
    for (int i = 0; i < LIVE_CAP; i++) {
        unsigned idx = (h + i) % LIVE_CAP;
        if (!g_live[idx] || g_live[idx] == o) { g_live[idx] = o; return; }
    }
}
static void liveRemove(Obj* o) {
    unsigned h = liveHash(o);
    for (int i = 0; i < LIVE_CAP; i++) {
        unsigned idx = (h + i) % LIVE_CAP;
        if (g_live[idx] == o) { g_live[idx] = NULL; return; }
    }
}
static void dbgCheckLive(Obj* o, const char* where) {
    if (o && !liveContains(o)) {
        fprintf(stderr, "NUOMI_DEBUG: %s @ '%s' on object NOT LIVE type=%d seq=%lu @%p ",
                where, g_dbg_phase, (int)o->type, o->dbgSeq, (void*)o);
        if (o->type == OBJ_STRING) {
            ObjString* s = (ObjString*)o;
            fprintf(stderr, "chars=[%.20s] isSymbol=%d len=%d\n", s->chars, s->isSymbol, s->length);
        } else { fprintf(stderr, "\n"); }
        abort();
    }
}
#else
#define dbgCheckLive(o, w) ((void)0)
#endif

Obj* allocObj(size_t size, ObjType type) {
    Obj* o = (Obj*)malloc(size);
    if (!o) { fprintf(stderr, "out of memory\n"); exit(1); }
    memset(o, 0, size);
    o->type = type;
    o->refcount = 1;          /* 调用方立即获得 1 个引用 */
    registerObj(o);
#ifdef NUOMI_DEBUG
    static unsigned long g_seq = 0;
    o->dbgSeq = ++g_seq;
    liveAdd(o);
#endif
    return o;
}

static void freeObject(Obj* o);

void retain(Value v) {
    if (IS_OBJ(v)) { Obj* o = AS_OBJ(v); dbgCheckLive(o, "retain"); o->refcount++; }
}

void release(Value v) {
    if (!IS_OBJ(v)) return;
    Obj* o = AS_OBJ(v);
    dbgCheckLive(o, "release");
#ifdef NUOMI_DEBUG
    fprintf(stderr, "  [release] @%p rc=%d\n", (void*)o, o->refcount);
#endif
#ifdef NUOMI_DEBUG
    if (o->refcount <= 0) {
        /* 过度释放：引用计数已经为 0 还要再减 —— 一定是某处 release 了不属它的引用。
           立即中止并把对象信息打出来，便于定位（正式版保持宽容，不中止）。 */
        fprintf(stderr, "[FATAL] over-release: obj=%p type=%d refcount=%d\n",
                (void*)o, (int)o->type, o->refcount);
        abort();
    }
#else
    if (o->refcount <= 0) return;
#endif
    o->refcount--;
    if (o->refcount == 0) freeObject(o);
}

static void freeObject(Obj* o) {
    /* 调试：标记已进入释放，若再次被访问即 use-after-free */
#ifdef NUOMI_DEBUG
    #ifdef NUOMI_DEBUG
    fprintf(stderr, "[FREE] seq=%lu type=%d @%p phase='%s'\n",
            o->dbgSeq, (int)o->type, (void*)o, g_dbg_phase);
#endif
    liveRemove(o);
    if (o->type == OBJ_LIST) {
        ObjList* l = (ObjList*)o;
        const char* tag = "?";
        if (IS_NUM(l->car)) { static char b[32]; sprintf(b, "car=%.0f", AS_NUM(l->car)); tag = b; }
        else if (IS_OBJ(l->car) && AS_OBJ(l->car)->type == OBJ_STRING) tag = ((ObjString*)AS_OBJ(l->car))->chars;
        fprintf(stderr, "  [free #%lu] LIST (%s)\n", o->dbgSeq, tag);
    } else {
        fprintf(stderr, "  [free #%lu] type=%d\n", o->dbgSeq, (int)o->type);
    }
#endif
    /* 先从全局注册表摘除 */
    Obj** p = &g_objects;
    while (*p) {
        if (*p == o) { *p = o->next; break; }
        p = &(*p)->next;
    }
    switch (o->type) {
        case OBJ_STRING: break; /* 柔性数组，随结构体一起释放 */
        case OBJ_LIST: {
            ObjList* l = (ObjList*)o;
            release(l->car);
            release(l->cdr);
            break;
        }
        case OBJ_VECTOR: {
            ObjVector* v = (ObjVector*)o;
            for (int i = 0; i < v->count; i++) release(v->items[i]);
            free(v->items);
            break;
        }
        case OBJ_FUNCTION: {
            ObjFunction* f = (ObjFunction*)o;
            if (f->name) release(OBJ_VAL(f->name));
            for (int i = 0; i < f->arity; i++)
                if (f->params[i]) release(OBJ_VAL(f->params[i]));
            free(f->params);
            release(f->body);
            if (f->env) release(OBJ_VAL(f->env));
            break;
        }
        case OBJ_ENV: {
            Env* e = (Env*)o;
            for (int i = 0; i < e->count; i++) {
                release(OBJ_VAL(e->keys[i]));
                release(e->values[i]);
            }
            free(e->keys);
            free(e->values);
            if (e->parent) release(OBJ_VAL(e->parent));
            break;
        }
        case OBJ_NATIVE:
            free(((ObjNative*)o)->name);
            break;
    }
    free(o);
}

/* 程序退出时：无论引用计数如何，强制释放全部对象（含顶层闭包环） */
void freeAllObjects(void) {
    Obj* o = g_objects;
    while (o) {
        Obj* next = o->next;
        switch (o->type) {
            case OBJ_VECTOR: free(((ObjVector*)o)->items); break;
            case OBJ_FUNCTION: free(((ObjFunction*)o)->params); break;
            case OBJ_ENV: free(((Env*)o)->keys); free(((Env*)o)->values); break;
            case OBJ_NATIVE: free(((ObjNative*)o)->name); break;
            default: break;
        }
        free(o);
        o = next;
    }
    g_objects = NULL;
}

/* ====================== 字符串驻留 ====================== */
static ObjString** g_intern = NULL;
static int g_internCap = 0;
static int g_internCount = 0;

static unsigned hashStr(const char* s, int len) {
    unsigned h = 2166136261u;
    for (int i = 0; i < len; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

static ObjString* makeString(const char* chars, int len, bool isSymbol) {
    ObjString* s = (ObjString*)allocObj(sizeof(ObjString) + len + 1, OBJ_STRING);
    s->length = len;
    s->isSymbol = isSymbol;
    memcpy(s->chars, chars, len);
    s->chars[len] = '\0';
    return s;
}

ObjString* internString(const char* chars, int len, bool isSymbol) {
    if (g_internCap == 0) {
        g_internCap = 1024;
        g_intern = (ObjString**)calloc(g_internCap, sizeof(ObjString*));
    }
    unsigned mask = (unsigned)(g_internCap - 1);
    unsigned h = hashStr(chars, len) & mask;
    while (g_intern[h]) {
        ObjString* s = g_intern[h];
        if (s->length == len && s->isSymbol == isSymbol &&
            memcmp(s->chars, chars, len) == 0) {
            /* ⚠️ 去重命中也必须 retain —— 本函数的契约是「返回的串由调用方拥有一份引用」
               （与新分配路径一致：makeString 给 1 份 + 驻留表 1 份）。
               曾在此处直接 return 不 retain，导致同一内容第二次驻留时调用方拿到的是
               「幽灵引用」：后续 release 会把驻留中的串提前释放，
               而驻留表仍指着它 → 之后 internString 返回悬垂指针（use-after-free）。
               症状是非确定性段错误，且与「相同内容的串重复出现」强相关。 */
            s->obj.refcount++;
            return s;
        }
        h = (h + 1) & mask;
    }
    ObjString* s = makeString(chars, len, isSymbol);
    s->obj.refcount++;             /* 驻留表作为永久根持有一份引用 */
    g_intern[h] = s;
    g_internCount++;
    if (g_internCount * 3 > g_internCap * 2) {
        int newCap = g_internCap * 2;
        ObjString** nt = (ObjString**)calloc(newCap, sizeof(ObjString*));
        unsigned nmask = (unsigned)(newCap - 1);
        for (int i = 0; i < g_internCap; i++) {
            ObjString* s2 = g_intern[i];
            if (!s2) continue;
            unsigned hh = hashStr(s2->chars, s2->length) & nmask;
            while (nt[hh]) hh = (hh + 1) & nmask;
            nt[hh] = s2;
        }
        free(g_intern);
        g_intern = nt;
        g_internCap = newCap;
    }
    return s;
}

/* ====================== 打印值 ====================== */
/* 把值格式化为字符串（rawString=真时字符串不带引号，用于 str 拼接） */
static void valueToString(Value v, char** out, size_t* len, size_t* cap, bool raw) {
    char buf[256];
    int n = 0;
    if (IS_NIL(v)) { n = (int)strlen("nil"); memcpy(buf, "nil", n); }
    else if (IS_BOOL(v)) { const char* s = AS_BOOL(v) ? "true" : "false"; n = (int)strlen(s); memcpy(buf, s, n); }
    else if (IS_NUM(v)) {
        double d = AS_NUM(v);
        if (d == (long long)d) n = sprintf(buf, "%lld", (long long)d);
        else n = sprintf(buf, "%g", d);
    } else {
        Obj* o = AS_OBJ(v);
        if (o->type == OBJ_STRING) {
            ObjString* s = (ObjString*)o;
            int total = s->length + (raw ? 0 : 2);
            if (*len + total + 1 > *cap) { *cap = (*len + total + 16); *out = (char*)realloc(*out, *cap); }
            if (!raw) (*out)[(*len)++] = '"';
            memcpy(*out + *len, s->chars, s->length);
            *len += s->length;
            if (!raw) (*out)[(*len)++] = '"';
            return;
        }
        if (o->type == OBJ_LIST) {
            char tmp[32]; int tn = sprintf(tmp, "(");
            if (*len + tn > *cap) { *cap = (*len + tn) * 2 + 16; *out = (char*)realloc(*out, *cap); }
            memcpy(*out + *len, tmp, tn); *len += tn;
            Value cur = OBJ_VAL(o);
            bool first = true;
            while (isList(cur)) {
                if (!first) {
                    if (*len + 1 > *cap) { *cap = *len + 16; *out = (char*)realloc(*out, *cap); }
                    (*out)[(*len)++] = ' ';
                }
                valueToString(listCar(cur), out, len, cap, false);
                first = false;
                cur = listCdr(cur);
            }
            if (!IS_NIL(cur)) {
                if (*len + 3 > *cap) { *cap = *len + 16; *out = (char*)realloc(*out, *cap); }
                (*out)[(*len)++] = ' '; (*out)[(*len)++] = '.'; (*out)[(*len)++] = ' ';
                valueToString(cur, out, len, cap, false);
            }
            if (*len + 1 > *cap) { *cap = *len + 16; *out = (char*)realloc(*out, *cap); }
            (*out)[(*len)++] = ')';
            return;
        }
        if (o->type == OBJ_VECTOR) {
            char tmp[32]; int tn = sprintf(tmp, "[");
            if (*len + tn > *cap) { *cap = (*len + tn) * 2 + 16; *out = (char*)realloc(*out, *cap); }
            memcpy(*out + *len, tmp, tn); *len += tn;
            ObjVector* vec = (ObjVector*)o;
            for (int i = 0; i < vec->count; i++) {
                if (i) {
                    if (*len + 1 > *cap) { *cap = *len + 16; *out = (char*)realloc(*out, *cap); }
                    (*out)[(*len)++] = ' ';
                }
                valueToString(vec->items[i], out, len, cap, false);
            }
            if (*len + 1 > *cap) { *cap = *len + 16; *out = (char*)realloc(*out, *cap); }
            (*out)[(*len)++] = ']';
            return;
        }
        if (o->type == OBJ_FUNCTION) {
            ObjFunction* f = (ObjFunction*)o;
            n = sprintf(buf, "<fn %s>", f->name ? f->name->chars : "lambda");
        } else if (o->type == OBJ_NATIVE) {
            n = sprintf(buf, "<native %s>", ((ObjNative*)o)->name);
        } else {
            n = sprintf(buf, "<env>");
        }
    }
    if (*len + n + 1 > *cap) { *cap = (*len + n + 16); *out = (char*)realloc(*out, *cap); }
    memcpy(*out + *len, buf, n);
    *len += n;
}

void printValue(Value v, FILE* out) {
    char* s = NULL;
    size_t len = 0, cap = 64;
    s = (char*)malloc(cap);
    valueToString(v, &s, &len, &cap, false);
    s[len] = '\0';
    fputs(s, out);
    free(s);
}

/* 把值转成 malloc 的 C 字符串（raw=true 时字符串不带引号，用于 str 拼接） */
char* valueToCString(Value v) {
    char* s = NULL;
    size_t len = 0, cap = 64;
    s = (char*)malloc(cap);
    valueToString(v, &s, &len, &cap, true);
    s[len] = '\0';
    return s;
}
