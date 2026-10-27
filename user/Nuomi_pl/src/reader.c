// SPDX-License-Identifier: GPL-2.0
/*
 * reader.c — 读取器：把源码解析成 Value 组成的 AST
 * 支持： () 列表、[] 向量、' 引号缩写、; 行注释、数字、字符串、符号
 */
#include "nuomi.h"

static void rErr(Reader* r, const char* m) { r->error = true; r->errmsg = m; }

void skipWs(Reader* r) {
    while (r->pos < r->len) {
        char c = r->src[r->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') r->pos++;
        else if (c == ';') { while (r->pos < r->len && r->src[r->pos] != '\n') r->pos++; }
        else break;
    }
}

static bool isDelimiter(char c) {
    return c == '(' || c == ')' || c == '[' || c == ']' ||
           c == '\'' || c == ';' || c == ' ' || c == '\t' ||
           c == '\n' || c == '\r';
}

/* Nuomi 的字符串定界符 —— 刻意区别于市面语言
 *
 * 常用语言都用 " 或 ' 作字符串界定（ Lisp 家族里 ' 还得挪给quote 用），
 * 于是我们用一对排版引号 U+275D / U+275E 作**首选**字符串定界符：
 *   ❝hello❞            字符串
 *   "legacy"             传统界定符（保留兼容，语义完全相同）
 *   'x                  引用（quote），与字符串彻底区分开
 *
 * 这样 ' 一心一意只做 quote，不会像某些 Lisp 那样 " 和 ' 语义打架；
 * 而 ❝❞ 在主流语言里几乎没人用 —— 看到就知道是 Nuomi，辨识度拉满，
 * 且写成对的括号一样对称，编辑器高亮也毫无压力。 */
static const char* const STR_OPEN   = "\xE2\x9D\x9D";  /* U+275D ❝ */
static const char* const STR_CLOSE  = "\xE2\x9D\x9E";  /* U+275E ❞ */

Value readForm(Reader* r);

/* 读取一个列表或向量；调用前已消费开头的 ( 或 [ */
static Value readList(Reader* r, char close) {
    bool isVec = (close == ']');
    Value* forms = NULL;
    int n = 0, cap = 0;
    for (;;) {
        skipWs(r);
        if (r->pos >= r->len) { rErr(r, "unexpected EOF"); break; }
        char c = r->src[r->pos];
        if (c == close) { r->pos++; break; }
        if (c == ')') { if (close != ']') { rErr(r, "unexpected )"); break; } }
        if (c == ']') { if (close != ']') { rErr(r, "unexpected ]"); break; } }
        Value f = readForm(r);
        if (r->error) break;
        if (n == cap) {
            cap = cap < 8 ? 8 : cap * 2;
            forms = (Value*)realloc(forms, cap * sizeof(Value));
        }
        forms[n++] = f;
    }
    if (r->error) {
        for (int i = 0; i < n; i++) release(forms[i]);
        free(forms);
        return NIL_VAL;
    }
    Value result;
    if (isVec) {
        ObjVector* v = (ObjVector*)allocObj(sizeof(ObjVector), OBJ_VECTOR);
        v->count = n; v->capacity = n;
        v->items = (Value*)malloc(n * sizeof(Value));
        for (int i = 0; i < n; i++) { v->items[i] = forms[i]; retain(forms[i]); } /* 元素共享对象，必须 retain */
        result = OBJ_VAL(v);
    } else {
        Value l = NIL_VAL;
        for (int i = n - 1; i >= 0; i--) {
            ObjList* cell = (ObjList*)allocObj(sizeof(ObjList), OBJ_LIST);
            cell->car = forms[i];
            retain(forms[i]);     /* car 是符号/子表达式等共享对象，必须 retain */
            cell->cdr = l;        /* cdr 是链表后继（链式转移所有权，不额外 retain） */
            l = OBJ_VAL(cell);
        }
        result = l;
    }
    free(forms);
    return result;
}

static Value readAtom(Reader* r) {
    size_t start = r->pos;
    while (r->pos < r->len && !isDelimiter(r->src[r->pos])) r->pos++;
    int len = (int)(r->pos - start);
    if (len == 0) { rErr(r, "empty atom"); return NIL_VAL; }

    char* buf = (char*)malloc(len + 1);
    memcpy(buf, r->src + start, len);
    buf[len] = '\0';

    Value v;
    if (strcmp(buf, "true") == 0)            v = BOOL_VAL(true);
    else if (strcmp(buf, "false") == 0)      v = BOOL_VAL(false);
    else if (strcmp(buf, "nil") == 0)        v = NIL_VAL;
    else {
        char* end = NULL;
        double d = strtod(buf, &end);
        if (*end == '\0' && len > 0)         v = NUM_VAL(d);
        else {
            ObjString* s = internString(buf, len, true); /* 符号 */
            v = OBJ_VAL(s);
        }
    }
    free(buf);
    return v;
}

/* 读取一个字符串字面量；调用前已消费开头的定界符（" 或 ❝）
 * closeTok 传入结尾定界符串（" 为单字节；❞ 为 3 字节 UTF-8），closeLen 是其长度。 */
static Value readString(Reader* r, const char* closeTok, size_t closeLen) {
    size_t start = r->pos;
    int len = 0;
    bool needEsc = (closeLen == 1);   /* 传统 " 界定符才处理转义；❝❞ 原样保留 */
    while (r->pos < r->len) {
        /* 命中结尾定界符？ */
        if (r->pos + closeLen <= r->len && memcmp(r->src + r->pos, closeTok, closeLen) == 0) break;
        if (needEsc && r->src[r->pos] == '\\' && r->pos + 1 < r->len) r->pos++; /* 跳过转义 */
        r->pos++; len++;
    }
    if (r->pos >= r->len) { rErr(r, "unterminated string"); return NIL_VAL; }
    char* buf = (char*)malloc(len + 1);
    int j = 0;
    for (size_t i = start; i < r->pos; i++) {
        if (needEsc && r->src[i] == '\\' && i + 1 < r->pos) {
            char nx = r->src[++i];
            if (nx == 'n') buf[j++] = '\n';
            else if (nx == 't') buf[j++] = '\t';
            else if (nx == '"') buf[j++] = '"';
            else if (nx == '\\') buf[j++] = '\\';
            else buf[j++] = nx;
        } else buf[j++] = r->src[i];
    }
    buf[j] = '\0';
    ObjString* s = internString(buf, j, false); /* 字符串字面量 */
    free(buf);
    r->pos += closeLen; /* 消费结尾定界符 */
    return OBJ_VAL(s);
}

Value readForm(Reader* r) {
    skipWs(r);
    if (r->pos >= r->len) { r->done = true; return NIL_VAL; }
    char c = r->src[r->pos];
    if (c == '(') { r->pos++; return readList(r, ')'); }
    if (c == '[') { r->pos++; return readList(r, ']'); }
    if (c == ')') { rErr(r, "unexpected )"); return NIL_VAL; }
    if (c == ']') { rErr(r, "unexpected ]"); return NIL_VAL; }
    if (c == '"') { r->pos++; return readString(r, "\"", 1); }
    /* 独特定界符 ❝ ... ❞（UTF-8 各 3 字节） */
    if (r->pos + 3 <= r->len && memcmp(r->src + r->pos, STR_OPEN, 3) == 0) {
        r->pos += 3;
        return readString(r, STR_CLOSE, 3);
    }
    if (c == '\'') {
        r->pos++;
        Value q = readForm(r);
        if (r->error) return NIL_VAL;
        /* 展开成 (quote q)：allocObj 已给每个 cell 1 个引用（所有权），
           直接用「转移所有权」挂到 cdr/car，不要再 retain（否则会泄漏）。 */
        ObjList* cell1 = (ObjList*)allocObj(sizeof(ObjList), OBJ_LIST);
        ObjList* cell0 = (ObjList*)allocObj(sizeof(ObjList), OBJ_LIST);
        ObjString* qs = internString("quote", 5, true);
        cell0->car = OBJ_VAL(qs); retain(OBJ_VAL(qs));   /* 符号是永久根，多 retain 无害 */
        cell0->cdr = OBJ_VAL(cell1);                     /* 转移：cell0 拥有 cell1 */
        cell1->car = q;                                  /* 转移：cell1 拥有 q */
        cell1->cdr = NIL_VAL;
        return OBJ_VAL(cell0);
    }
    return readAtom(r);
}
