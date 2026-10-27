// SPDX-License-Identifier: GPL-2.0
/*
 * json.c — JSON 解析/序列化原语
 *
 * 为什么糯米需要它：MC 启动器全靠 JSON 吃饭——
 *   · Mojang Piston API（版本清单 version_manifest_v2.json、版本元数据、资产索引）
 *   · Modrinth API（搜索 Mod、拉版本文件列表）
 *   · 实例元数据、Modrinth .mrpack 打包清单
 * 没有 JSON，糯米连「读懂一个版本清单」都做不到。
 *
 * JSON → 糯米值的映射：
 *   object → alist：若干 (key . value) 链节组成的列表，用 json-get 取值
 *   array  → 向量：用 get 按下标取（支持负索引）
 *   string → 字符串（解转义：\" \\ \/ \b \f \n \r \t \uXXXX→UTF-8，含代理对）
 *   number → 数字（double）
 *   true/false/null → 布尔 / nil
 *
 * 为什么 object 用 alist 而不是字典：字典要新增哈希表类型，那是语言层面的
 * 大改动（垃圾回收、拷贝语义都要跟着动）。alist 是现成的 cons 链，零成本。
 * 代价是取值 O(n) —— 版本清单这种小 JSON 无所谓；资源索引（几千个对象）
 * 确实会慢，将来要上字典时这里不用改调用方。
 */
#include "nuomi.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* 输入常来自网络，必须防恶意深层嵌套把 C 栈爆掉 */
#define JSON_MAX_DEPTH 128

typedef struct {
    const char* p;
    bool err;
} JP;

/* ---------- 工具 ---------- */

/* 比较两个值是否为同一字符串（符号/字符串一视同仁，写起来方便） */
static bool jstr_eq(Value a, Value b) {
    if (!IS_OBJ(a) || !IS_OBJ(b)) return false;
    if (AS_OBJ(a)->type != OBJ_STRING || AS_OBJ(b)->type != OBJ_STRING) return false;
    ObjString* x = (ObjString*)AS_OBJ(a);
    ObjString* y = (ObjString*)AS_OBJ(b);
    if (x->length != y->length) return false;
    return memcmp(x->chars, y->chars, (size_t)x->length) == 0;
}

/* 码点 → UTF-8 字节，返回写入长度（buf 至少 4 字节） */
static int utf8_encode(unsigned cp, char* buf) {
    if (cp < 0x80)    { buf[0] = (char)cp; return 1; }
    if (cp < 0x800)   { buf[0] = (char)(0xC0 | (cp >> 6));
                        buf[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { buf[0] = (char)(0xE0 | (cp >> 12));
                        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        buf[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    buf[0] = (char)(0xF0 | (cp >> 18));
    buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static unsigned hex4(const char* s, bool* ok) {
    unsigned v = 0;
    *ok = true;
    for (int i = 0; i < 4; i++) {
        char h = s[i];
        unsigned d;
        if (h >= '0' && h <= '9') d = (unsigned)(h - '0');
        else if (h >= 'a' && h <= 'f') d = (unsigned)(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') d = (unsigned)(h - 'A' + 10);
        else { *ok = false; return 0; }
        v = (v << 4) | d;
    }
    return v;
}

/* ---------- 解析 ---------- */

static void jskip_ws(JP* j) {
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') j->p++;
}

/* 前向声明：array/object 都要递归调用它，而它的定义在两者之后 */
static Value jparse_value(JP* j, int depth);

/* 解析字符串字面量（含转义展开），返回新字符串；失败 NULL */
static ObjString* jparse_string(JP* j) {
    if (*j->p != '"') { j->err = true; return NULL; }
    j->p++;
    size_t cap = 32, len = 0;
    char* buf = (char*)malloc(cap);
    while (*j->p && *j->p != '"') {
        if (len + 8 > cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
        char ch = *j->p;
        if (ch != '\\') { buf[len++] = ch; j->p++; continue; }

        j->p++;
        char esc = *j->p;
        switch (esc) {
            case '"':  buf[len++] = '"';  j->p++; break;
            case '\\': buf[len++] = '\\'; j->p++; break;
            case '/':  buf[len++] = '/';  j->p++; break;
            case 'b':  buf[len++] = '\b'; j->p++; break;
            case 'f':  buf[len++] = '\f'; j->p++; break;
            case 'n':  buf[len++] = '\n'; j->p++; break;
            case 'r':  buf[len++] = '\r'; j->p++; break;
            case 't':  buf[len++] = '\t'; j->p++; break;
            case 'u': {
                j->p++;
                bool ok = false;
                unsigned cp = hex4(j->p, &ok);
                if (!ok) { free(buf); j->err = true; return NULL; }
                j->p += 4;
                /* 代理对：\uD83D\uDE00 这种要合并成一个码点才是一个字符 */
                if (cp >= 0xD800 && cp <= 0xDBFF && j->p[0] == '\\' && j->p[1] == 'u') {
                    bool ok2 = false;
                    unsigned lo = hex4(j->p + 2, &ok2);
                    if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        j->p += 6;
                    }
                }
                len += (size_t)utf8_encode(cp, buf + len);
                break;
            }
            default: free(buf); j->err = true; return NULL;   /* 非法转义 */
        }
    }
    if (*j->p != '"') { free(buf); j->err = true; return NULL; }   /* 未闭合 */
    j->p++;
    ObjString* s = internString(buf, (int)len, false);
    free(buf);
    return s;
}

/* 解析数组 → 向量 */
static Value jparse_array(JP* j, int depth) {
    j->p++;                       /* [ */
    jskip_ws(j);
    Value* items = NULL;
    int n = 0, cap = 0;
    if (*j->p == ']') {           /* 空数组 */
        j->p++;
        ObjVector* ev = (ObjVector*)allocObj(sizeof(ObjVector), OBJ_VECTOR);
        ev->count = 0; ev->capacity = 0; ev->items = NULL;
        return OBJ_VAL(ev);
    }
    while (1) {
        Value v = jparse_value(j, depth + 1);
        if (j->err) { free(items); return NIL_VAL; }
        if (n == cap) { cap = cap ? cap * 2 : 8; items = (Value*)realloc(items, (size_t)cap * sizeof(Value)); }
        items[n++] = v;            /* v 自带一个引用，存进向量即转移所有权 */
        jskip_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == ']') { j->p++; break; }
        j->err = true; free(items); return NIL_VAL;
    }
    ObjVector* v = (ObjVector*)allocObj(sizeof(ObjVector), OBJ_VECTOR);
    v->count = n; v->capacity = n; v->items = items;
    return OBJ_VAL(v);
}

/* 解析对象 → alist（(key . value) 链节列表） */
static Value jparse_object(JP* j, int depth) {
    j->p++;                       /* { */
    jskip_ws(j);
    Value head = NIL_VAL;
    Value tail = NIL_VAL;
    if (*j->p == '}') { j->p++; return NIL_VAL; }   /* 空对象 = 空 alist = nil */
    while (1) {
        jskip_ws(j);
        ObjString* key = jparse_string(j);
        if (j->err || !key) return NIL_VAL;
        jskip_ws(j);
        if (*j->p != ':') { j->err = true; return NIL_VAL; }
        j->p++;
        Value v = jparse_value(j, depth + 1);
        if (j->err) return NIL_VAL;

        /* 每项做成 (键 . 值) 配对链节，再用外层链节把它们串起来。
         * ⚠️ 不能图省事把值直接放外层链节的 cdr —— 外层 cdr 要留给「下一个项」，
         *    两者抢同一个字段，追加第二项时会把第一项的值顶掉（键全串位）。 */
        ObjList* pair = (ObjList*)allocObj(sizeof(ObjList), OBJ_LIST);
        pair->car = OBJ_VAL(key);  retain(pair->car);
        pair->cdr = v;             /* 值转移所有权 */
        ObjList* cell = (ObjList*)allocObj(sizeof(ObjList), OBJ_LIST);
        cell->car = OBJ_VAL(pair);
        cell->cdr = NIL_VAL;

        if (head.type == VAL_NIL) head = OBJ_VAL(cell);
        else ((ObjList*)AS_OBJ(tail))->cdr = OBJ_VAL(cell);
        tail = OBJ_VAL(cell);

        jskip_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == '}') { j->p++; break; }
        j->err = true; return NIL_VAL;
    }
    return head;
}

static Value jparse_number(JP* j) {
    char* end = NULL;
    double d = strtod(j->p, &end);
    if (end == j->p) { j->err = true; return NIL_VAL; }
    j->p = end;
    return NUM_VAL(d);
}

static Value jparse_value(JP* j, int depth) {
    if (depth > JSON_MAX_DEPTH) { j->err = true; return NIL_VAL; }
    jskip_ws(j);
    char c = *j->p;
    switch (c) {
        case '{': return jparse_object(j, depth);
        case '[': return jparse_array(j, depth);
        case '"': {
            ObjString* s = jparse_string(j);
            if (!s) return NIL_VAL;
            Value v = OBJ_VAL(s);
            retain(v);
            return v;
        }
        case 't': if (strncmp(j->p, "true", 4) == 0)  { j->p += 4; return BOOL_VAL(true); }  break;
        case 'f': if (strncmp(j->p, "false", 5) == 0) { j->p += 5; return BOOL_VAL(false); } break;
        case 'n': if (strncmp(j->p, "null", 4) == 0)  { j->p += 4; return NIL_VAL; }         break;
        case '\0': break;
        default:
            if (c == '-' || (c >= '0' && c <= '9')) return jparse_number(j);
            break;
    }
    j->err = true;
    return NIL_VAL;
}

/* ---------- 序列化 ---------- */

typedef struct { char* buf; size_t len, cap; } SB;

static void sb_put(SB* sb, const char* s, size_t n) {
    if (sb->len + n + 1 > sb->cap) {
        while (sb->len + n + 1 > sb->cap) sb->cap = sb->cap ? sb->cap * 2 : 128;
        sb->buf = (char*)realloc(sb->buf, sb->cap);
    }
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = '\0';
}
static void sb_str(SB* sb, const char* s) { sb_put(sb, s, strlen(s)); }
static void sb_ch(SB* sb, char c) { sb_put(sb, &c, 1); }

static void sb_json_string(SB* sb, const char* s, int len) {
    sb_ch(sb, '"');
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  sb_str(sb, "\\\""); break;
            case '\\': sb_str(sb, "\\\\"); break;
            case '\n': sb_str(sb, "\\n");  break;
            case '\r': sb_str(sb, "\\r");  break;
            case '\t': sb_str(sb, "\\t");  break;
            case '\b': sb_str(sb, "\\b");  break;
            case '\f': sb_str(sb, "\\f");  break;
            default:
                if (c < 0x20) { char tmp[8]; snprintf(tmp, sizeof tmp, "\\u%04x", c); sb_str(sb, tmp); }
                else sb_ch(sb, (char)c);
        }
    }
    sb_ch(sb, '"');
}

static void sb_value(SB* sb, Value v);

/* 取出 alist 一「项」的键与值。
 *
 * 项**只能是点对** ("key" . value)。这是刻意收紧的：
 * 早先也想认手写常见的二元表 ("key" value)，但两种写法在「值恰好是
 * 只有一个元素的列表」时结构上无法区分 —— 嵌套单键对象
 * `{"a":{"b":1}}` 会把内层对象误当成值，取值直接串位。
 * 猜不出来就别猜：要手写对象用 json-obj 构造器。
 */
static bool alist_entry(Value entry, Value* key, Value* val) {
    if (!isList(entry)) return false;
    Value k = listCar(entry);
    if (!IS_OBJ(k) || AS_OBJ(k)->type != OBJ_STRING) return false;
    *key = k;
    *val = listCdr(entry);
    return true;
}

/* alist 的一「项」= 外层链节里装着一个 (键 . 值) 配对链节 */
static bool is_alist_entry(Value v) {
    Value k, val;
    return alist_entry(listCar(v), &k, &val);
}

/* 列表：每个元素都是 "键字符串 . 值" 的链节 → 判为 JSON 对象，否则数组 */
static void sb_value(SB* sb, Value v) {
    if (v.type == VAL_NIL)      { sb_str(sb, "null"); return; }
    if (v.type == VAL_BOOL)     { sb_str(sb, AS_BOOL(v) ? "true" : "false"); return; }
    if (v.type == VAL_NUMBER) {
        double d = AS_NUM(v);
        char tmp[40];
        /* 整数值不要写成 1.0，贴近 JSON 惯例 */
        if (d == (double)(long long)d && d < 9e15 && d > -9e15)
            snprintf(tmp, sizeof tmp, "%lld", (long long)d);
        else
            snprintf(tmp, sizeof tmp, "%.17g", d);
        sb_str(sb, tmp);
        return;
    }
    if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING) {
        ObjString* s = (ObjString*)AS_OBJ(v);
        sb_json_string(sb, s->chars, s->length);
        return;
    }
    if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_VECTOR) {
        ObjVector* vec = (ObjVector*)AS_OBJ(v);
        sb_ch(sb, '[');
        for (int i = 0; i < vec->count; i++) {
            if (i) sb_ch(sb, ',');
            sb_value(sb, vec->items[i]);
        }
        sb_ch(sb, ']');
        return;
    }
    if (isList(v)) {
        /* 全是 (键 . 值) 链节 → 对象 */
        bool asObject = true;
        for (Value cur = v; isList(cur); cur = listCdr(cur))
            if (!is_alist_entry(cur)) { asObject = false; break; }
        if (asObject) {
            sb_ch(sb, '{');
            bool first = true;
            for (Value cur = v; isList(cur); cur = listCdr(cur)) {
                Value k, val;
                if (!alist_entry(listCar(cur), &k, &val)) continue;
                if (!first) sb_ch(sb, ',');
                first = false;
                ObjString* ks = (ObjString*)AS_OBJ(k);
                sb_json_string(sb, ks->chars, ks->length);
                sb_ch(sb, ':');
                sb_value(sb, val);
            }
            sb_ch(sb, '}');
        } else {
            sb_ch(sb, '[');
            bool first = true;
            for (Value cur = v; isList(cur); cur = listCdr(cur)) {
                if (!first) sb_ch(sb, ',');
                first = false;
                sb_value(sb, listCar(cur));
            }
            sb_ch(sb, ']');
        }
        return;
    }
    sb_str(sb, "null");
}

/* ---------- 原语 ---------- */

/* json-parse 文本 -> 值（失败 nil + Runtime error） */
static Value natJsonParse(int c, Value* a, Env* e) {
    (void)e;
    if (c < 1 || !IS_OBJ(a[0]) || AS_OBJ(a[0])->type != OBJ_STRING) {
        runtimeError("json-parse expects a string");
        return NIL_VAL;
    }
    ObjString* s = (ObjString*)AS_OBJ(a[0]);
    /* 复制一份并补 NUL：源串可能内嵌 \0（read-file 来的），不能直接当 C 串用 */
    char* text = (char*)malloc((size_t)s->length + 1);
    memcpy(text, s->chars, (size_t)s->length);
    text[s->length] = '\0';

    JP j = { text, false };
    Value v = jparse_value(&j, 0);
    if (!j.err) {
        jskip_ws(&j);
        if (*j.p != '\0') j.err = true;     /* 尾部有残留 → 不是单个 JSON 值 */
    }
    free(text);
    if (j.err) { runtimeError("json-parse: invalid JSON"); return NIL_VAL; }
    return v;
}

/* json-get 对象 键... —— 支持多层路径；对象查键、向量按下标 */
static Value jget_path(Value cur, int argc, Value* keys, int ki) {
    if (ki >= argc) { retain(cur); return cur; }
    Value k = keys[ki];
    if (isList(cur)) {                      /* alist：找键 */
        for (Value node = cur; isList(node); node = listCdr(node)) {
            Value ek, ev;                 /* 这一项的键/值，别和查找键 k 撞名 */
            if (alist_entry(listCar(node), &ek, &ev) && jstr_eq(ek, k))
                return jget_path(ev, argc, keys, ki + 1);
        }
        return NIL_VAL;
    }
    if (IS_OBJ(cur) && AS_OBJ(cur)->type == OBJ_VECTOR) {
        if (!IS_NUM(k)) return NIL_VAL;
        ObjVector* v = (ObjVector*)AS_OBJ(cur);
        int idx = (int)AS_NUM(k);
        if (idx < 0) idx += v->count;        /* 负索引：-1 = 最后一项 */
        if (idx < 0 || idx >= v->count) return NIL_VAL;
        return jget_path(v->items[idx], argc, keys, ki + 1);
    }
    return NIL_VAL;
}

static Value natJsonGet(int c, Value* a, Env* e) {
    (void)e;
    if (c < 2) { runtimeError("json-get expects (value, key ...)"); return NIL_VAL; }
    return jget_path(a[0], c - 1, a + 1, 0);
}

/* json-stringify 值 -> JSON 文本 */
static Value natJsonStringify(int c, Value* a, Env* e) {
    (void)e;
    if (c < 1) { runtimeError("json-stringify expects a value"); return NIL_VAL; }
    SB sb = { NULL, 0, 0 };
    sb_put(&sb, "", 0);
    sb_value(&sb, a[0]);
    ObjString* out = internString(sb.buf ? sb.buf : "", (int)sb.len, false);
    free(sb.buf);
    retain(OBJ_VAL(out));
    return OBJ_VAL(out);
}

/* json-obj 键 值 键 值 ... -> JSON 对象的 alist
 *
 * 存在的理由：alist 项严格用点对 ("k" . v)，手写点对太啰嗦
 * （`cons` 套 `list` 套 `list`）。要构造对象就用这个。
 *   (json-obj "name" "Ryan" "port" 25565)
 *   → ((name . Ryan) (port . 25565))
 * 参数个数须为偶数；键必须是字符串。 */
static Value natJsonObj(int c, Value* a, Env* e) {
    (void)e;
    if (c % 2 != 0) { runtimeError("json-obj expects (key value ...)"); return NIL_VAL; }
    Value head = NIL_VAL, tail = NIL_VAL;
    for (int i = 0; i + 1 < c; i += 2) {
        if (!IS_OBJ(a[i]) || AS_OBJ(a[i])->type != OBJ_STRING) {
            runtimeError("json-obj: key must be a string");
            return NIL_VAL;
        }
        ObjList* pair = (ObjList*)allocObj(sizeof(ObjList), OBJ_LIST);
        pair->car = a[i]; retain(pair->car);      /* a[i] 由 argv 持有 */
        pair->cdr = a[i + 1]; retain(pair->cdr);  /* 值也要 retain（可能被复用/长期持有） */
        ObjList* cell = (ObjList*)allocObj(sizeof(ObjList), OBJ_LIST);
        cell->car = OBJ_VAL(pair);
        cell->cdr = NIL_VAL;
        if (head.type == VAL_NIL) head = OBJ_VAL(cell);
        else ((ObjList*)AS_OBJ(tail))->cdr = OBJ_VAL(cell);
        tail = OBJ_VAL(cell);
    }
    return head;
}

/* 注册（由 native.c 的 registerNatives 调用） */
void registerJsonNatives(Env* g) {
    defNative(g, "json-parse", natJsonParse);
    defNative(g, "json-get", natJsonGet);
    defNative(g, "json-stringify", natJsonStringify);
    defNative(g, "json-obj", natJsonObj);
}