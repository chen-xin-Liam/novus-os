// SPDX-License-Identifier: GPL-2.0
/*
 * native.c — 内置原生函数
 */
#include "nuomi.h"

/* ---------- 算术 ---------- */
static Value natAdd(int c, Value* a, Env* e) {
    (void)e; double s = 0;
    for (int i = 0; i < c; i++) { if (!IS_NUM(a[i])) { runtimeError("+ expects numbers"); return NIL_VAL; } s += AS_NUM(a[i]); }
    return NUM_VAL(s);
}
static Value natSub(int c, Value* a, Env* e) {
    (void)e;
    if (c == 1) { if (!IS_NUM(a[0])) { runtimeError("- expects numbers"); return NIL_VAL; } return NUM_VAL(-AS_NUM(a[0])); }
    double s = AS_NUM(a[0]);
    for (int i = 1; i < c; i++) { if (!IS_NUM(a[i])) { runtimeError("- expects numbers"); return NIL_VAL; } s -= AS_NUM(a[i]); }
    return NUM_VAL(s);
}
static Value natMul(int c, Value* a, Env* e) {
    (void)e; double s = 1;
    for (int i = 0; i < c; i++) { if (!IS_NUM(a[i])) { runtimeError("* expects numbers"); return NIL_VAL; } s *= AS_NUM(a[i]); }
    return NUM_VAL(s);
}
static Value natDiv(int c, Value* a, Env* e) {
    (void)e; double s = (c == 1) ? 1.0 : AS_NUM(a[0]);
    for (int i = (c == 1 ? 0 : 1); i < c; i++) {
        if (!IS_NUM(a[i])) { runtimeError("/ expects numbers"); return NIL_VAL; }
        if (AS_NUM(a[i]) == 0) { runtimeError("division by zero"); return NIL_VAL; }
        s /= AS_NUM(a[i]);
    }
    return NUM_VAL(s);
}
static Value natMod(int c, Value* a, Env* e) {
    (void)e;
    if (c != 2 || !IS_NUM(a[0]) || !IS_NUM(a[1])) { runtimeError("% expects two numbers"); return NIL_VAL; }
    if (AS_NUM(a[1]) == 0) { runtimeError("modulo by zero"); return NIL_VAL; }
    return NUM_VAL(fmod(AS_NUM(a[0]), AS_NUM(a[1])));
}

/* ---------- 比较 ---------- */
static Value natNumEq(int c, Value* a, Env* e) {
    (void)e;
    for (int i = 1; i < c; i++)
        if (!IS_NUM(a[0]) || !IS_NUM(a[i]) || AS_NUM(a[0]) != AS_NUM(a[i]))
            return BOOL_VAL(false);
    return BOOL_VAL(true);
}
static Value natLt(int c, Value* a, Env* e) {
    (void)e;
    for (int i = 1; i < c; i++)
        if (!IS_NUM(a[i-1]) || !IS_NUM(a[i]) || !(AS_NUM(a[i-1]) < AS_NUM(a[i])))
            return BOOL_VAL(false);
    return BOOL_VAL(true);
}
static Value natGt(int c, Value* a, Env* e) {
    (void)e;
    for (int i = 1; i < c; i++)
        if (!IS_NUM(a[i-1]) || !IS_NUM(a[i]) || !(AS_NUM(a[i-1]) > AS_NUM(a[i])))
            return BOOL_VAL(false);
    return BOOL_VAL(true);
}
static Value natLe(int c, Value* a, Env* e) {
    (void)e;
    for (int i = 1; i < c; i++)
        if (!IS_NUM(a[i-1]) || !IS_NUM(a[i]) || !(AS_NUM(a[i-1]) <= AS_NUM(a[i])))
            return BOOL_VAL(false);
    return BOOL_VAL(true);
}
static Value natGe(int c, Value* a, Env* e) {
    (void)e;
    for (int i = 1; i < c; i++)
        if (!IS_NUM(a[i-1]) || !IS_NUM(a[i]) || !(AS_NUM(a[i-1]) >= AS_NUM(a[i])))
            return BOOL_VAL(false);
    return BOOL_VAL(true);
}
static Value natNot(int c, Value* a, Env* e) {
    (void)e; (void)c; return BOOL_VAL(isFalsey(a[0]));
}

/* ---------- 相等性 ---------- */
/* 结构相等：列表/向量递归比较，其余按值 */
static bool valueEqual(Value x, Value y) {
    if (x.type != y.type) return false;
    if (IS_NIL(x) || IS_BOOL(x) || IS_NUM(x)) {
        if (IS_NIL(x)) return IS_NIL(y);
        if (IS_BOOL(x)) return AS_BOOL(x) == AS_BOOL(y);
        return AS_NUM(x) == AS_NUM(y);
    }
    if (IS_OBJ(x)) {
        Obj* ox = AS_OBJ(x); Obj* oy = AS_OBJ(y);
        if (ox->type != oy->type) return false;
        if (ox->type == OBJ_STRING) {
            ObjString* sx = (ObjString*)ox, *sy = (ObjString*)oy;
            return sx->length == sy->length && memcmp(sx->chars, sy->chars, sx->length) == 0;
        }
        if (ox->type == OBJ_LIST) {
            Value cx = x, cy = y;
            while (isList(cx) && isList(cy)) {
                if (!valueEqual(listCar(cx), listCar(cy))) return false;
                cx = listCdr(cx); cy = listCdr(cy);
            }
            return IS_NIL(cx) && IS_NIL(cy);
        }
        if (ox->type == OBJ_VECTOR) {
            ObjVector* vx = (ObjVector*)ox, *vy = (ObjVector*)oy;
            if (vx->count != vy->count) return false;
            for (int i = 0; i < vx->count; i++)
                if (!valueEqual(vx->items[i], vy->items[i])) return false;
            return true;
        }
        return ox == oy;
    }
    return false;
}
static Value natEqual(int c, Value* a, Env* e) {
    (void)e;
    if (c < 2) return BOOL_VAL(true);
    for (int i = 1; i < c; i++) if (!valueEqual(a[0], a[i])) return BOOL_VAL(false);
    return BOOL_VAL(true);
}
/* 同一性（指针相等） */
static Value natEq(int c, Value* a, Env* e) {
    (void)e;
    for (int i = 1; i < c; i++)
        if (!(IS_OBJ(a[0]) && IS_OBJ(a[i]) && AS_OBJ(a[0]) == AS_OBJ(a[i])) &&
            !(IS_NIL(a[0]) && IS_NIL(a[i])) &&
            !(IS_BOOL(a[0]) && IS_BOOL(a[i]) && AS_BOOL(a[0]) == AS_BOOL(a[i])) &&
            !(IS_NUM(a[0]) && IS_NUM(a[i]) && AS_NUM(a[0]) == AS_NUM(a[i])))
            return BOOL_VAL(false);
    return BOOL_VAL(true);
}

/* ---------- 输出 ---------- */
static Value natPrint(int c, Value* a, Env* e) {
    (void)e;
    for (int i = 0; i < c; i++) { if (i) putchar(' '); printValue(a[i], stdout); }
    return NIL_VAL;
}
static Value natPrintln(int c, Value* a, Env* e) {
    (void)e;
    natPrint(c, a, e);
    putchar('\n');
    return NIL_VAL;
}

/* ---------- 列表 / 向量 ---------- */
static Value natCons(int c, Value* a, Env* e) {
    (void)e;
    if (c != 2) { runtimeError("cons expects 2 args"); return NIL_VAL; }
    ObjList* cell = (ObjList*)allocObj(sizeof(ObjList), OBJ_LIST);
    /* a[0]/a[1] 由调用方(argv)持有，apply 返回后会被 release；
       本 cell 要长期持有它们，必须各自 retain 取得所有权。 */
    cell->car = a[0]; retain(a[0]);
    cell->cdr = a[1]; retain(a[1]);
    return OBJ_VAL(cell);
}
static Value natCar(int c, Value* a, Env* e) {
    (void)e;
    if (c != 1 || !isList(a[0]) || IS_NIL(a[0])) { runtimeError("car: not a pair"); return NIL_VAL; }
    Value v = listCar(a[0]); retain(v); return v;
}
static Value natCdr(int c, Value* a, Env* e) {
    (void)e;
    if (c != 1 || !isList(a[0]) || IS_NIL(a[0])) { runtimeError("cdr: not a pair"); return NIL_VAL; }
    Value v = listCdr(a[0]); retain(v); return v;
}
static Value natList(int c, Value* a, Env* e) {
    (void)e;
    Value head = NIL_VAL;
    for (int i = c - 1; i >= 0; i--) {
        ObjList* cell = (ObjList*)allocObj(sizeof(ObjList), OBJ_LIST);
        /* a[i] 由 argv 持有，apply 返回后会被 release；cell 要长期持有 car，
           必须 retain。cdr=head 是链表的后继链节，用「转移所有权」不额外 retain
           （否则尾节点会泄漏）。 */
        cell->car = a[i]; retain(a[i]);
        cell->cdr = head;
        head = OBJ_VAL(cell);
    }
    return head;
}

/* substring：按字节切字符串。
 *   (substring s)            → 整串
 *   (substring s start)      → 从 start 到末尾
 *   (substring s start end)  → [start, end)
 * start / end 支持负数：-1 表示末尾，倒数第 2 个即 -2（同 json-get 的负索引风格）。
 * 越界一律夹到合法范围，不报错。 */
static Value natSubstring(int c, Value* a, Env* e) {
    (void)e;
    if (c < 1 || c > 3) { runtimeError("substring expects (s [start [end]])"); return NIL_VAL; }
    Value v = a[0];
    if (!IS_OBJ(v) || AS_OBJ(v)->type != OBJ_STRING) { runtimeError("substring: not a string"); return NIL_VAL; }
    ObjString* str = (ObjString*)AS_OBJ(v);
    int len = str->length;
    long long st = 0, en = len;
    if (c >= 2) {
        if (!IS_NUM(a[1])) { runtimeError("substring: start must be a number"); return NIL_VAL; }
        st = (long long)AS_NUM(a[1]);
    }
    if (c >= 3) {
        if (!IS_NUM(a[2])) { runtimeError("substring: end must be a number"); return NIL_VAL; }
        en = (long long)AS_NUM(a[2]);
    }
    if (st < 0) st += len;          /* 负数 → 从末尾数 */
    if (en < 0) en += len;
    if (st < 0) st = 0;
    if (en > len) en = len;
    if (st > len) st = len;
    if (en < st) en = st;
    int n = (int)(en - st);
    ObjString* out = internString(str->chars + st, n, false);
    return OBJ_VAL(out);
}

/* string-search：在 haystack 里找 needle，返回首次出现的下标；找不到返回 -1。
 *   (string-search haystack needle)        → 从头开始找
 *   (string-search haystack needle start)  → 从 start 位置开始找（找第 2、第 3 个分隔符时用）
 * 负的 start 表示从末尾数（-1 = 末尾）。 */
static Value natStringSearch(int c, Value* a, Env* e) {
    (void)e;
    if (c < 2 || c > 3) { runtimeError("string-search expects (haystack needle [start])"); return NIL_VAL; }
    if (!IS_OBJ(a[0]) || AS_OBJ(a[0])->type != OBJ_STRING ||
        !IS_OBJ(a[1]) || AS_OBJ(a[1])->type != OBJ_STRING) {
        runtimeError("string-search: expects two strings");
        return NIL_VAL;
    }
    ObjString* h = (ObjString*)AS_OBJ(a[0]);
    ObjString* n = (ObjString*)AS_OBJ(a[1]);
    long long from = 0;
    if (c == 3) {
        if (!IS_NUM(a[2])) { runtimeError("string-search: start must be a number"); return NIL_VAL; }
        from = (long long)AS_NUM(a[2]);
    }
    if (from < 0) from += h->length;
    if (from < 0) from = 0;
    if (from > h->length) return NUM_VAL(-1);
    if (n->length == 0) return NUM_VAL((double)from);
    for (int i = (int)from; i + n->length <= h->length; i++) {
        if (memcmp(h->chars + i, n->chars, (size_t)n->length) == 0) return NUM_VAL(i);
    }
    return NUM_VAL(-1);
}

static Value natLength(int c, Value* a, Env* e) {
    (void)e;
    if (c != 1) { runtimeError("length expects 1 arg"); return NIL_VAL; }
    Value v = a[0];
    if (isList(v)) return NUM_VAL(listLen(v));
    if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_VECTOR) return NUM_VAL(((ObjVector*)AS_OBJ(v))->count);
    if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING) return NUM_VAL(((ObjString*)AS_OBJ(v))->length);
    runtimeError("length: bad type"); return NIL_VAL;
}
static Value natNth(int c, Value* a, Env* e) {
    (void)e;
    if (c != 2 || !IS_NUM(a[1])) { runtimeError("nth expects (coll index)"); return NIL_VAL; }
    int idx = (int)AS_NUM(a[1]);
    Value v = a[0];
    if (isList(v)) {
        Value cur = listRef(v, idx);
        if (!isList(cur)) { runtimeError("nth: index out of range"); return NIL_VAL; }
        Value r = listCar(cur); retain(r); return r;
    }
    if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_VECTOR) {
        ObjVector* vec = (ObjVector*)AS_OBJ(v);
        if (idx < 0 || idx >= vec->count) { runtimeError("nth: index out of range"); return NIL_VAL; }
        Value r = vec->items[idx]; retain(r); return r;
    }
    runtimeError("nth: not indexable"); return NIL_VAL;
}
static Value natGet(int c, Value* a, Env* e) {
    (void)e;
    if (c != 2 || !IS_NUM(a[1])) { runtimeError("get expects (coll index)"); return NIL_VAL; }
    int idx = (int)AS_NUM(a[1]);
    Value v = a[0];
    if (isList(v)) {
        Value cur = listRef(v, idx);
        if (!isList(cur)) { runtimeError("get: index out of range"); return NIL_VAL; }
        Value r = listCar(cur); retain(r); return r;
    }
    if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_VECTOR) {
        ObjVector* vec = (ObjVector*)AS_OBJ(v);
        if (idx < 0 || idx >= vec->count) { runtimeError("get: index out of range"); return NIL_VAL; }
        Value r = vec->items[idx]; retain(r); return r;
    }
    if (IS_OBJ(v) && AS_OBJ(v)->type == OBJ_STRING) {
        ObjString* s = (ObjString*)AS_OBJ(v);
        if (idx < 0 || idx >= s->length) { runtimeError("get: index out of range"); return NIL_VAL; }
        ObjString* ch = internString(s->chars + idx, 1, false);
        retain(OBJ_VAL(ch));
        return OBJ_VAL(ch);
    }
    runtimeError("get: not indexable"); return NIL_VAL;
}
static Value natSet(int c, Value* a, Env* e) {
    (void)e;
    if (c != 3 || !IS_NUM(a[1])) { runtimeError("set expects (vec index val)"); return NIL_VAL; }
    if (!(IS_OBJ(a[0]) && AS_OBJ(a[0])->type == OBJ_VECTOR)) { runtimeError("set: not a vector"); return NIL_VAL; }
    ObjVector* vec = (ObjVector*)AS_OBJ(a[0]);
    int idx = (int)AS_NUM(a[1]);
    if (idx < 0 || idx >= vec->count) { runtimeError("set: index out of range"); return NIL_VAL; }
    release(vec->items[idx]);
    vec->items[idx] = a[2]; retain(a[2]);   /* 向量持有 */
    retain(a[2]);                           /* 返回值持有 */
    return a[2];
}
static Value natVec(int c, Value* a, Env* e) {
    (void)e;
    ObjVector* v = (ObjVector*)allocObj(sizeof(ObjVector), OBJ_VECTOR);
    v->count = c; v->capacity = c;
    v->items = (Value*)malloc(c * sizeof(Value));
    /* a[i] 由 argv 持有，apply 返回后会被 release；向量要长期持有每个元素，
       必须各自 retain 取得所有权（不能只「转移」，否则 vector 内的指针会悬空）。 */
    for (int i = 0; i < c; i++) { v->items[i] = a[i]; retain(a[i]); }
    return OBJ_VAL(v);
}
static Value natStr(int c, Value* a, Env* e) {
    (void)e;
    char** parts = (char**)malloc((c > 0 ? c : 1) * sizeof(char*));
    size_t total = 0;
    for (int i = 0; i < c; i++) {
        parts[i] = valueToCString(a[i]);
        total += strlen(parts[i]);
    }
    char* s = (char*)malloc(total + 1);
    size_t off = 0;
    for (int i = 0; i < c; i++) {
        size_t l = strlen(parts[i]);
        memcpy(s + off, parts[i], l);
        off += l;
        free(parts[i]);
    }
    s[off] = '\0';
    ObjString* str = internString(s, (int)off, false);
    free(s);
    free(parts);
    retain(OBJ_VAL(str));
    return OBJ_VAL(str);
}

/* ---------- 输入 ---------- */
/* input [提示]：从 stdin 读一行（去掉换行），返回字符串。
   可选的第一个参数作为提示（原样输出、不带引号）。 */
static Value natInput(int c, Value* a, Env* e) {
    (void)e;
    if (c >= 1 && IS_OBJ(a[0]) && AS_OBJ(a[0])->type == OBJ_STRING) {
        fputs(((ObjString*)AS_OBJ(a[0]))->chars, stdout);
        fflush(stdout);
    }
    char buf[512];
    if (!fgets(buf, sizeof(buf), stdin)) {
        ObjString* s = internString("", 0, false);
        retain(OBJ_VAL(s));
        return OBJ_VAL(s);
    }
    int len = (int)strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
    ObjString* s = internString(buf, len, false);
    retain(OBJ_VAL(s));
    return OBJ_VAL(s);
}
/* input-int [提示]：从 stdin 读一行并解析为整数，返回数字。
   非数字或空行返回 0。可选的第一个参数作为提示。 */
static Value natInputInt(int c, Value* a, Env* e) {
    (void)e;
    if (c >= 1 && IS_OBJ(a[0]) && AS_OBJ(a[0])->type == OBJ_STRING) {
        fputs(((ObjString*)AS_OBJ(a[0]))->chars, stdout);
        fflush(stdout);
    }
    char buf[512];
    if (!fgets(buf, sizeof(buf), stdin)) return NUM_VAL(0);
    int len = (int)strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
    char* p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return NUM_VAL(0);
    double v = strtol(p, NULL, 10);
    return NUM_VAL(v);
}

/* ============================================================
 * ---------- 终端 / 游戏原语 ----------
 * 贪吃蛇这类实时游戏需要四样东西：清屏、延时、不阻塞的键盘、随机数。
 * 糯米原先只有行式输入（input），所以补上这一组。
 * Windows 走 Win32 控制台 API + conio；其他平台用 ANSI 序列兜底。
 * ============================================================ */
#if defined(_WIN32)
#include <windows.h>
#include <conio.h>
#include <time.h>

static HANDLE g_conOut = NULL;
static void conEnsure(void) {
    if (!g_conOut) g_conOut = GetStdHandle(STD_OUTPUT_HANDLE);
}

/* cls：清屏 + 光标归位 + 隐藏光标（隐藏是为防游戏画面闪烁） */
static Value natCls(int c, Value* a, Env* e) {
    (void)c; (void)a; (void)e;
    conEnsure();
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(g_conOut, &csbi)) {
        DWORD cells = (DWORD)csbi.dwSize.X * (DWORD)csbi.dwSize.Y;
        COORD home = { 0, 0 };
        DWORD written;
        FillConsoleOutputCharacterA(g_conOut, ' ', cells, home, &written);
        FillConsoleOutputAttribute(g_conOut, csbi.wAttributes, cells, home, &written);
        SetConsoleCursorPosition(g_conOut, home);
    }
    CONSOLE_CURSOR_INFO ci;
    if (GetConsoleCursorInfo(g_conOut, &ci)) { ci.bVisible = FALSE; SetConsoleCursorInfo(g_conOut, &ci); }
    return NIL_VAL;
}

/* show-cursor：恢复光标（游戏退出时必须调，不然光标一直藏着） */
static Value natShowCursor(int c, Value* a, Env* e) {
    (void)c; (void)a; (void)e;
    conEnsure();
    CONSOLE_CURSOR_INFO ci;
    if (GetConsoleCursorInfo(g_conOut, &ci)) { ci.bVisible = TRUE; SetConsoleCursorInfo(g_conOut, &ci); }
    return NIL_VAL;
}

/* home：光标归位 + 隐藏光标，**不清屏**。
 * 清屏是游戏闪烁的根因——每帧把整屏抹白再重画，眼睛直接看瞎。
 * 正确做法是每帧只把光标移回左上角重绘（画面每行等宽，旧内容自然被覆盖），
 * 配合 draw 整帧一次写出，控制台整帧呈现，不再有半成品画面。 */
static Value natHome(int c, Value* a, Env* e) {
    (void)c; (void)a; (void)e;
    conEnsure();
    COORD home = { 0, 0 };
    SetConsoleCursorPosition(g_conOut, home);
    CONSOLE_CURSOR_INFO ci;
    if (GetConsoleCursorInfo(g_conOut, &ci)) { ci.bVisible = FALSE; SetConsoleCursorInfo(g_conOut, &ci); }
    return NIL_VAL;
}

/* draw 文本：原样输出字符串——不加引号（print 会加）、不加换行、**一次 fwrite 写完**。
 * 游戏每帧把整个画面拼成一个大字符串交给它：一次系统调用 → 控制台整帧更新，
 * 不会像分 22 次写那样看到画到一半的画面（撕裂）。 */
static Value natDraw(int c, Value* a, Env* e) {
    (void)e;
    if (c < 1 || !IS_OBJ(a[0]) || AS_OBJ(a[0])->type != OBJ_STRING) {
        runtimeError("draw expects a string");
        return NIL_VAL;
    }
    ObjString* s = (ObjString*)AS_OBJ(a[0]);
    fwrite(s->chars, 1, (size_t)s->length, stdout);
    fflush(stdout);
    return NIL_VAL;
}

/* sleep 毫秒：游戏帧延时 */
static Value natSleep(int c, Value* a, Env* e) {
    (void)e;
    if (c < 1 || !IS_NUM(a[0])) { runtimeError("sleep expects milliseconds"); return NIL_VAL; }
    if (AS_NUM(a[0]) < 0) return NIL_VAL;
    Sleep((DWORD)AS_NUM(a[0]));
    return NIL_VAL;
}

/* key：非阻塞读键盘。无按键 → nil。
 * 方向键 → 符号 'up / 'down / 'left / 'right；Esc → 'esc；
 * 普通键 → 该字符的字符串（如 "w"）。方向键在 Windows 控制台是
 * 0xE0/0x00 前缀 + 扫描码两段式，这里合成成符号返回。 */
static Value natKey(int c, Value* a, Env* e) {
    (void)c; (void)a; (void)e;
    if (!_kbhit()) return NIL_VAL;
    int ch = _getch();
    if (ch == 0 || ch == 0xE0) {
        int code = _getch();
        const char* nm = NULL;
        switch (code) {
            case 72: nm = "up";    break;
            case 80: nm = "down";  break;
            case 75: nm = "left";  break;
            case 77: nm = "right"; break;
            default: return NIL_VAL;
        }
        ObjString* s = internString(nm, (int)strlen(nm), true);
        retain(OBJ_VAL(s));
        return OBJ_VAL(s);
    }
    if (ch == 27) {
        ObjString* s = internString("esc", 3, true);
        retain(OBJ_VAL(s));
        return OBJ_VAL(s);
    }
    if (ch == '\r' || ch == '\n') ch = ' ';
    char buf[2] = { (char)ch, '\0' };
    ObjString* s = internString(buf, 1, false);
    retain(OBJ_VAL(s));
    return OBJ_VAL(s);
}

/* random n：[0, n) 均匀随机整数。LCG + 时间种子，n<=0 时返回 0。 */
static unsigned long long g_rng = 0;
static Value natRandom(int c, Value* a, Env* e) {
    (void)e;
    if (c < 1 || !IS_NUM(a[0])) { runtimeError("random expects a positive integer"); return NIL_VAL; }
    if (!g_rng) {
        g_rng = (unsigned long long)time(NULL);
        g_rng ^= (unsigned long long)(size_t)&g_rng;   /* 地址扰动，避免同秒同种子 */
        if (!g_rng) g_rng = 0x9E3779B97F4A7C15ull;
    }
    g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull;
    double n = AS_NUM(a[0]);
    if (n <= 0) return NUM_VAL(0);
    return NUM_VAL((double)((g_rng >> 33) % (unsigned long long)n));
}
#else  /* ---- 非 Windows：ANSI 清屏 + nanosleep；无键盘支持（key 恒返回 nil） ---- */
#include <time.h>
static Value natCls(int c, Value* a, Env* e) {
    (void)c; (void)a; (void)e;
    fputs("\x1B[2J\x1B[H\x1B[?25l", stdout);
    fflush(stdout);
    return NIL_VAL;
}
static Value natShowCursor(int c, Value* a, Env* e) {
    (void)c; (void)a; (void)e;
    fputs("\x1B[?25h", stdout);
    fflush(stdout);
    return NIL_VAL;
}
static Value natHome(int c, Value* a, Env* e) {
    (void)c; (void)a; (void)e;
    fputs("\x1B[H\x1B[?25l", stdout);
    fflush(stdout);
    return NIL_VAL;
}
static Value natDraw(int c, Value* a, Env* e) {
    (void)e;
    if (c < 1 || !IS_OBJ(a[0]) || AS_OBJ(a[0])->type != OBJ_STRING) {
        runtimeError("draw expects a string");
        return NIL_VAL;
    }
    ObjString* s = (ObjString*)AS_OBJ(a[0]);
    fwrite(s->chars, 1, (size_t)s->length, stdout);
    fflush(stdout);
    return NIL_VAL;
}
static Value natSleep(int c, Value* a, Env* e) {
    (void)e;
    if (c < 1 || !IS_NUM(a[0])) { runtimeError("sleep expects milliseconds"); return NIL_VAL; }
    struct timespec ts;
    ts.tv_sec = (time_t)(AS_NUM(a[0]) / 1000);
    ts.tv_nsec = (long)((AS_NUM(a[0]) - ts.tv_sec * 1000) * 1000000);
    nanosleep(&ts, NULL);
    return NIL_VAL;
}
static Value natKey(int c, Value* a, Env* e) {
    (void)c; (void)a; (void)e;
    return NIL_VAL;   /* 非 Windows 平台暂无非阻塞键盘 */
}
static Value natRandom(int c, Value* a, Env* e) {
    (void)e;
    if (c < 1 || !IS_NUM(a[0])) { runtimeError("random expects a positive integer"); return NIL_VAL; }
    if (AS_NUM(a[0]) <= 0) return NUM_VAL(0);
    return NUM_VAL((double)(rand() % (int)AS_NUM(a[0])));
}
#endif

/* ============================================================
 * ---------- 文件系统原语 ----------
 * 糯米在 novus 里的定位是**系统壳语言**（≈ Windows 的 PowerShell），
 * 文件操作是壳的基本功，所以这一组是语言的核心能力而非外挂。
 *
 * 实现分层：
 *   · 宿主（Windows）：Win32 API
 *   · 其他平台：POSIX（remove/mkdir/rename/opendir/stat/fopen）
 *   · 将来跑在 novus 上时：这一层换成 novus 的文件系统 syscall，
 *     **脚本接口不变**——号表集中在 include/novus_syscall.h 的思路一样。
 *
 * 错误语义与 Nuomi 运行时一致：**软错误**——失败返回 nil/false/-1，
 * 打印 Runtime error 但不中止程序（壳语言要能容忍单个命令失败）。
 *
 * ⚠️ 这是真实破坏力：delete/rmdir 不可恢复。没有递归删除原语
 *    （rm -rf 请在脚本里用 ls + delete + rmdir 自己递归，见 samples/files.nuomi）。
 * ============================================================ */
#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#endif
static bool fsArg(Value v, const char** out) {
    if (!IS_OBJ(v) || AS_OBJ(v)->type != OBJ_STRING) return false;
    *out = ((ObjString*)AS_OBJ(v))->chars;
    return true;
}

/* exists? path -> bool */
static Value natExists(int c, Value* a, Env* e) {
    (void)e;
    const char* p;
    if (c < 1 || !fsArg(a[0], &p)) { runtimeError("exists? expects a path string"); return NIL_VAL; }
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(p);
    return BOOL_VAL(attr != INVALID_FILE_ATTRIBUTES);
#else
    struct stat st;
    return BOOL_VAL(stat(p, &st) == 0);
#endif
}

/* is-dir? path -> bool */
static Value natIsDir(int c, Value* a, Env* e) {
    (void)e;
    const char* p;
    if (c < 1 || !fsArg(a[0], &p)) { runtimeError("is-dir? expects a path string"); return NIL_VAL; }
#if defined(_WIN32)
    DWORD attr = GetFileAttributesA(p);
    return BOOL_VAL(attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
#else
    struct stat st;
    return BOOL_VAL(stat(p, &st) == 0 && S_ISDIR(st.st_mode));
#endif
}

/* delete path -> bool：删**文件**。目录/不存在/被占用 → false */
static Value natDelete(int c, Value* a, Env* e) {
    (void)e;
    const char* p;
    if (c < 1 || !fsArg(a[0], &p)) { runtimeError("delete expects a path string"); return NIL_VAL; }
#if defined(_WIN32)
    return BOOL_VAL(DeleteFileA(p) != 0);
#else
    return BOOL_VAL(remove(p) == 0);
#endif
}

/* rmdir path -> bool：删**空**目录。非空/不存在 → false */
static Value natRmdir(int c, Value* a, Env* e) {
    (void)e;
    const char* p;
    if (c < 1 || !fsArg(a[0], &p)) { runtimeError("rmdir expects a path string"); return NIL_VAL; }
#if defined(_WIN32)
    return BOOL_VAL(RemoveDirectoryA(p) != 0);
#else
    return BOOL_VAL(rmdir(p) == 0);
#endif
}

/* mkdir path -> bool：已存在 → false */
static Value natMkdir(int c, Value* a, Env* e) {
    (void)e;
    const char* p;
    if (c < 1 || !fsArg(a[0], &p)) { runtimeError("mkdir expects a path string"); return NIL_VAL; }
#if defined(_WIN32)
    return BOOL_VAL(CreateDirectoryA(p, NULL) != 0);
#else
    return BOOL_VAL(mkdir(p, 0755) == 0);
#endif
}

/* file-size path -> 字节数；失败 -1 */
static Value natFileSize(int c, Value* a, Env* e) {
    (void)e;
    const char* p;
    if (c < 1 || !fsArg(a[0], &p)) { runtimeError("file-size expects a path string"); return NIL_VAL; }
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fad)) return NUM_VAL(-1);
    unsigned long long sz = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    return NUM_VAL((double)sz);
#else
    struct stat st;
    if (stat(p, &st) != 0) return NUM_VAL(-1);
    return NUM_VAL((double)st.st_size);
#endif
}

/* read-file path -> 字符串（保留字节原样，可含 \0）；失败 nil */
static Value natReadFile(int c, Value* a, Env* e) {
    (void)e;
    const char* p;
    if (c < 1 || !fsArg(a[0], &p)) { runtimeError("read-file expects a path string"); return NIL_VAL; }
#if defined(_WIN32)
    HANDLE h = CreateFileA(p, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NIL_VAL;
    DWORD sz = GetFileSize(h, NULL);
    if (sz == INVALID_FILE_SIZE) { CloseHandle(h); return NIL_VAL; }
    char* buf = (char*)malloc((size_t)sz + 1);
    DWORD rd = 0;
    ReadFile(h, buf, sz, &rd, NULL);
    CloseHandle(h);
    ObjString* s = internString(buf, (int)rd, false);
    free(buf);
    retain(OBJ_VAL(s));
    return OBJ_VAL(s);
#else
    FILE* f = fopen(p, "rb");
    if (!f) return NIL_VAL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NIL_VAL; }
    char* buf = (char*)malloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    ObjString* s = internString(buf, (int)rd, false);
    free(buf);
    retain(OBJ_VAL(s));
    return OBJ_VAL(s);
#endif
}

/* write-file path content -> bool：整串覆盖写，自动创建 */
static Value natWriteFile(int c, Value* a, Env* e) {
    (void)e;
    const char* p;
    if (c < 2 || !fsArg(a[0], &p) || !IS_OBJ(a[1]) || AS_OBJ(a[1])->type != OBJ_STRING) {
        runtimeError("write-file expects (path, content)");
        return NIL_VAL;
    }
    ObjString* s = (ObjString*)AS_OBJ(a[1]);
#if defined(_WIN32)
    HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return BOOL_VAL(false);
    DWORD wr = 0;
    BOOL ok = WriteFile(h, s->chars, (DWORD)s->length, &wr, NULL);
    CloseHandle(h);
    return BOOL_VAL(ok && wr == (DWORD)s->length);
#else
    FILE* f = fopen(p, "wb");
    if (!f) return BOOL_VAL(false);
    size_t wr = fwrite(s->chars, 1, (size_t)s->length, f);
    fclose(f);
    return BOOL_VAL(wr == (size_t)s->length);
#endif
}

/* append-file path content -> bool：追加到末尾，自动创建 */
static Value natAppendFile(int c, Value* a, Env* e) {
    (void)e;
    const char* p;
    if (c < 2 || !fsArg(a[0], &p) || !IS_OBJ(a[1]) || AS_OBJ(a[1])->type != OBJ_STRING) {
        runtimeError("append-file expects (path, content)");
        return NIL_VAL;
    }
    ObjString* s = (ObjString*)AS_OBJ(a[1]);
#if defined(_WIN32)
    HANDLE h = CreateFileA(p, GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return BOOL_VAL(false);
    SetFilePointer(h, 0, NULL, FILE_END);
    DWORD wr = 0;
    BOOL ok = WriteFile(h, s->chars, (DWORD)s->length, &wr, NULL);
    CloseHandle(h);
    return BOOL_VAL(ok && wr == (DWORD)s->length);
#else
    FILE* f = fopen(p, "ab");
    if (!f) return BOOL_VAL(false);
    size_t wr = fwrite(s->chars, 1, (size_t)s->length, f);
    fclose(f);
    return BOOL_VAL(wr == (size_t)s->length);
#endif
}

/* mv src dst -> bool：移动/重命名 */
static Value natMv(int c, Value* a, Env* e) {
    (void)e;
    const char *src, *dst;
    if (c < 2 || !fsArg(a[0], &src) || !fsArg(a[1], &dst)) {
        runtimeError("mv expects (src, dst)");
        return NIL_VAL;
    }
#if defined(_WIN32)
    return BOOL_VAL(MoveFileA(src, dst) != 0);
#else
    return BOOL_VAL(rename(src, dst) == 0);
#endif
}

/* cp src dst -> bool：复制（覆盖已存在目标） */
static Value natCp(int c, Value* a, Env* e) {
    (void)e;
    const char *src, *dst;
    if (c < 2 || !fsArg(a[0], &src) || !fsArg(a[1], &dst)) {
        runtimeError("cp expects (src, dst)");
        return NIL_VAL;
    }
#if defined(_WIN32)
    return BOOL_VAL(CopyFileA(src, dst, FALSE) != 0);
#else
    FILE* in = fopen(src, "rb");
    if (!in) return BOOL_VAL(false);
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return BOOL_VAL(false); }
    char buf[8192];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    fclose(in);
    fclose(out);
    return BOOL_VAL(ok);
#endif
}

/* ls dir -> 文件名列表（不含 . 与 ..）；失败/不是目录 → nil */
static Value natLs(int c, Value* a, Env* e) {
    (void)e;
    const char* p;
    if (c < 1 || !fsArg(a[0], &p)) { runtimeError("ls expects a directory path"); return NIL_VAL; }
    /* 先收集到 C 数组，再逆序 cons 成 Nuomi 链表 */
    char** names = NULL;
    int n = 0, cap = 0;
#if defined(_WIN32)
    size_t plen = strlen(p);
    char* pat = (char*)malloc(plen + 3);
    memcpy(pat, p, plen);
    /* 末尾补 \ 与 *（已带分隔符则只补 *） */
    if (plen > 0 && p[plen-1] != '\\' && p[plen-1] != '/') pat[plen++] = '\\';
    pat[plen++] = '*';
    pat[plen] = '\0';
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) return NIL_VAL;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (n == cap) { cap = cap < 16 ? 16 : cap * 2; names = (char**)realloc(names, (size_t)cap * sizeof(char*)); }
        names[n++] = _strdup(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(p);
    if (!d) return NIL_VAL;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (n == cap) { cap = cap < 16 ? 16 : cap * 2; names = (char**)realloc(names, (size_t)cap * sizeof(char*)); }
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
#endif
    Value lst = NIL_VAL;
    for (int i = n - 1; i >= 0; i--) {
        ObjString* s = internString(names[i], (int)strlen(names[i]), false);
        retain(OBJ_VAL(s));
        ObjList* cell = (ObjList*)allocObj(sizeof(ObjList), OBJ_LIST);
        cell->car = OBJ_VAL(s);      /* 转移 s 的所有权给 cell */
        cell->cdr = lst;             /* 后继链：转移所有权，不 retain */
        lst = OBJ_VAL(cell);
        free(names[i]);
    }
    free(names);
    return lst;
}

/* ---------- 高阶函数与列表工具 ---------- */
/* 遍历集合（列表或向量），对每个元素调用 fn。
 * listSeqNext 返回第 idx 个元素并把它拷出；集合耗尽时返回 false。 */
static bool collAt(Value coll, int idx, Value* out) {
    if (isList(coll)) {
        Value cur = listRef(coll, idx);
        if (!isList(cur)) return false;
        *out = listCar(cur);
        retain(*out);
        return true;
    }
    if (IS_OBJ(coll) && AS_OBJ(coll)->type == OBJ_VECTOR) {
        ObjVector* v = (ObjVector*)AS_OBJ(coll);
        if (idx < 0 || idx >= v->count) return false;
        *out = v->items[idx];
        retain(*out);
        return true;
    }
    return false;
}
static int collCount(Value coll) {
    /* nil 就是空列表（规范 §4.1：nil = 空值/空列表尾），
       所以 map/filter/reduce 等序列函数应把 nil 当空集合（0 个元素），
       而不是报「second arg must be a list or vector」。
       否则 rm-rf 这类脚本组合（对空目录 map）会刷一屏软错误。 */
    if (coll.type == VAL_NIL) return 0;
    if (isList(coll)) return listLen(coll);
    if (IS_OBJ(coll) && AS_OBJ(coll)->type == OBJ_VECTOR) return ((ObjVector*)AS_OBJ(coll))->count;
    return -1;
}
/* 在链表头压入一个元素，返回新的头。
 * 所有权约定：新 cell 自身持有一份（refcount=1，由返回值转交给调用方）；
 * 对 item 和 head 各 retain 一次，使它们同时被新 cell 与调用方各自持有。
 * 调用方随后应release 自己那份 item/head。
 */
static Value prependCell(Value head, Value item) {
    ObjList* cell = (ObjList*)allocObj(sizeof(ObjList), OBJ_LIST);
    cell->car = item; retain(item);
    cell->cdr = head; retain(head);      /* 新 cell 也要持有后继，缺这行会 use-after-free */
    return OBJ_VAL(cell);
}

/* (map fn coll) —— 逐元素映射，返回同形状的新列表 */
static Value natMap(int c, Value* a, Env* e) {
    if (c != 2) { runtimeError("map expects (fn coll)"); return NIL_VAL; }
    int n = collCount(a[1]);
    if (n < 0) { runtimeError("map: second arg must be a list or vector"); return NIL_VAL; }
    Value out = NIL_VAL;
    for (int i = n - 1; i >= 0; i--) {
        Value item;
        if (!collAt(a[1], i, &item)) continue;
        Value arg = item;
        Value r = apply(a[0], &arg, 1, e);
        /* arg 是借用别名：不能 release(arg)（会减掉 item 的引用计数，
           item 由本函数负责释放，见循环末尾）。 */
        Value nh = prependCell(out, r);
        release(out);
        out = nh;
        release(r);
        release(item);      /* 释放 collAt 给本文的那一份 */
    }
    return out;
}

/* (filter pred coll) —— 保留 pred 为真的元素 */
static Value natFilter(int c, Value* a, Env* e) {
    if (c != 2) { runtimeError("filter expects (pred coll)"); return NIL_VAL; }
    int n = collCount(a[1]);
    if (n < 0) { runtimeError("filter: second arg must be a list or vector"); return NIL_VAL; }
    /* 先正序收集命中的元素，再逆序建链 */
    Value* keep = (Value*)malloc((n > 0 ? n : 1) * sizeof(Value));
    int nk = 0;
    for (int i = 0; i < n; i++) {
        Value item;
        if (!collAt(a[1], i, &item)) continue;   /* collAt 已 retain(item)：本文 owns 一份 */
        Value arg = item;
        Value p = apply(a[0], &arg, 1, e);
        /* 注意：arg 只是借用别名，不能 release(arg) —— 那会把 item 的引用计数减掉，
           而下面还要把 item 存进 keep[]（keep 里的对象必须是活的）。
           正确做法：命中的元素把「本文 owns 的那一份所有权」转移给 keep[]，
           未命中的才 release 掉。 */
        bool yes = !isFalsey(p);
        release(p);
        if (yes) keep[nk++] = item;   /* 所有权转移给 keep[]，不 release */
        else release(item);           /* 未命中 → 释放本文这份 */
    }
    Value out = NIL_VAL;
    for (int i = nk - 1; i >= 0; i--) {
        Value nh = prependCell(out, keep[i]);
        release(out);
        out = nh;
        release(keep[i]);
    }
    free(keep);
    return out;
}

/* (reduce fn coll) / (reduce fn init coll) / (reduce fn coll init)
 * 三参时按「哪个参数是集合」来区分 (fn init coll) 与 (fn coll init)。 */
static Value natReduce(int c, Value* a, Env* e) {
    if (c < 2) { runtimeError("reduce expects (fn coll) or (fn init coll)"); return NIL_VAL; }
    if (c > 3) { runtimeError("reduce: too many args"); return NIL_VAL; }
    Value fn = a[0];
    int n;
    Value coll, acc;
    bool haveAcc = false;

    if (c == 2) {
        coll = a[1];
    } else {
        int n1 = collCount(a[1]);
        int n2 = collCount(a[2]);
        /* 恰有一个是集合时，按位置决定：第二个是集合 → (fn coll init)；
         * 否则默认 (fn init coll)。 */
        bool a1isColl = (n1 >= 0);
        bool a2isColl = (n2 >= 0);
        if (a1isColl && !a2isColl)      { coll = a[1]; acc = a[2]; haveAcc = true; }
        else if (!a1isColl && a2isColl) { coll = a[2]; acc = a[1]; haveAcc = true; }
        else {
            runtimeError("reduce: one of the two args must be a list or vector");
            return NIL_VAL;
        }
    }
    n = collCount(coll);
    if (n < 0) { runtimeError("reduce: not a list or vector"); return NIL_VAL; }

    Value cur = acc; retain(cur);
    int start = 0;
    if (!haveAcc) {
        if (n == 0) { release(cur); return NIL_VAL; }
        Value first;
        if (!collAt(coll, 0, &first)) { release(cur); return NIL_VAL; }
        release(cur);
        cur = first;              /* first 已 retain，作为新初值 */
        start = 1;
    }
    for (int i = start; i < n; i++) {
        Value item;
        if (!collAt(coll, i, &item)) break;
        Value args[2];
        args[0] = cur; args[1] = item;
        Value r = apply(fn, args, 2, e);
        release(cur);
        release(item);
        cur = r;                  /* 转移所有权 */
    }
    return cur;
}

/* (range start [end [step]]) —— 返回数字列表
 * (range 5)      => (0 1 2 3 4)
 * (range 2 5)    => (2 3 4)
 * (range 0 10 2) => (0 2 4 6 8) */
static Value natRange(int c, Value* a, Env* e) {
    (void)e;
    double start = 0, end = 0, step = 1;
    if (c == 1) { if (!IS_NUM(a[0])) { runtimeError("range expects numbers"); return NIL_VAL; } end = AS_NUM(a[0]); }
    else if (c == 2) {
        if (!IS_NUM(a[0]) || !IS_NUM(a[1])) { runtimeError("range expects numbers"); return NIL_VAL; }
        start = AS_NUM(a[0]); end = AS_NUM(a[1]);
    } else if (c == 3) {
        if (!IS_NUM(a[0]) || !IS_NUM(a[1]) || !IS_NUM(a[2])) { runtimeError("range expects numbers"); return NIL_VAL; }
        start = AS_NUM(a[0]); end = AS_NUM(a[1]); step = AS_NUM(a[2]);
    } else { runtimeError("range expects 1-3 args"); return NIL_VAL; }
    if (step == 0) { runtimeError("range: step must not be zero"); return NIL_VAL; }
    /* 先正向算出元素个数，再逆序建链 */
    int n = 0;
    if (step > 0) { for (double v = start; v < end; v += step) n++; }
    else          { for (double v = start; v > end; v += step) n++; }
    if (n > 10000000) { runtimeError("range: too many elements"); return NIL_VAL; }
    Value out = NIL_VAL;
    for (int i = n - 1; i >= 0; i--) {
        Value num = NUM_VAL(start + step * (double)i);
        Value nh = prependCell(out, num);
        release(out);
        out = nh;
        release(num);
    }
    return out;
}

/* (null? x) —— 列表/向量是否为空 */
static Value natNull(int c, Value* a, Env* e) {
    (void)e;
    if (c != 1) { runtimeError("null? expects 1 arg"); return NIL_VAL; }
    return BOOL_VAL(IS_NIL(a[0]) || collCount(a[0]) == 0);
}

/* (assoc lst key val) —— 在关联列表中替换/新增
 * 关联表元素形如 (key value)。 */
static Value natAssoc(int c, Value* a, Env* e) {
    if (c != 2 && c != 3) { runtimeError("assoc expects (lst key val)"); return NIL_VAL; }
    if (c == 2) {
        /* 两参形式assoc(lst key)：只查不建，返回找到的那条pair 或 nil */
        for (Value cur = a[0]; isList(cur); cur = listCdr(cur)) {
            Value pair = listCar(cur);
            if (!isList(pair)) continue;              /* 跳过非法条目，不解引用 */
            Value k = listCar(pair);
            if (valueEqual(k, a[1])) { retain(pair); return pair; }
        }
        return NIL_VAL;
    }
    Value head = NIL_VAL;
    bool replaced = false;
    for (Value cur = a[0]; isList(cur); cur = listCdr(cur)) {
        Value pair = listCar(cur);
        if (!isList(pair)) continue;                  /* 非法条目直接跳过 */
        Value k = listCar(pair);
        bool same = valueEqual(k, a[1]);
        if (same && !replaced) {
            Value kv[2]; kv[0] = k; kv[1] = a[2];
            Value np = natList(2, kv, e);
            Value nh = prependCell(head, np);
            release(head);
            head = nh;
            release(np);
            replaced = true;
        } else {
            retain(pair);
            Value nh = prependCell(head, pair);
            release(head);
            head = nh;
            release(pair);
        }
    }
    if (!replaced) {
        Value kv[2]; kv[0] = a[1]; kv[1] = a[2];
        Value np = natList(2, kv, e);
        Value nh = prependCell(head, np);
        release(head);
        head = nh;
        release(np);
    }
    return head;
}

/* (member? x coll) —— x 是否在 coll 中（结构相等） */
static Value natMember(int c, Value* a, Env* e) {
    (void)e;
    if (c != 2) { runtimeError("member? expects (x coll)"); return NIL_VAL; }
    int n = collCount(a[1]);
    if (n < 0) return BOOL_VAL(false);
    for (int i = 0; i < n; i++) {
        Value item;
        if (!collAt(a[1], i, &item)) break;
        bool eq = valueEqual(a[0], item);
        release(item);
        if (eq) return BOOL_VAL(true);
    }
    return BOOL_VAL(false);
}

/* (number? x) / (string? x) / (list? x) / (vector? x) / (function? x) / (bool? x) */
static Value natNumberP(int c, Value* a, Env* e) { (void)e; (void)c; return BOOL_VAL(IS_NUM(a[0])); }
static Value natStringP(int c, Value* a, Env* e) {
    (void)e; (void)c;
    return BOOL_VAL(IS_OBJ(a[0]) && AS_OBJ(a[0])->type == OBJ_STRING && !((ObjString*)AS_OBJ(a[0]))->isSymbol);
}
static Value natListP(int c, Value* a, Env* e) { (void)e; (void)c; return BOOL_VAL(isList(a[0]) || IS_NIL(a[0])); }
static Value natVectorP(int c, Value* a, Env* e) {
    (void)e; (void)c;
    return BOOL_VAL(IS_OBJ(a[0]) && AS_OBJ(a[0])->type == OBJ_VECTOR);
}
static Value natFunctionP(int c, Value* a, Env* e) {
    (void)e; (void)c;
    if (!IS_OBJ(a[0])) return BOOL_VAL(false);
    ObjType t = AS_OBJ(a[0])->type;
    return BOOL_VAL(t == OBJ_FUNCTION || t == OBJ_NATIVE);
}
static Value natBoolP(int c, Value* a, Env* e) { (void)e; (void)c; return BOOL_VAL(IS_BOOL(a[0])); }

/* (abs x) / (floor x) / (ceil x) / (round x) / (sqrt x) / (min ...) / (max ...)
 * (pow a b) / (mod-floor a b) —— 补齐常用数学函数 */
static Value natAbs(int c, Value* a, Env* e) {
    (void)e;
    if (c != 1 || !IS_NUM(a[0])) { runtimeError("abs expects a number"); return NIL_VAL; }
    return NUM_VAL(fabs(AS_NUM(a[0])));
}
static Value natFloor(int c, Value* a, Env* e) {
    (void)e;
    if (c != 1 || !IS_NUM(a[0])) { runtimeError("floor expects a number"); return NIL_VAL; }
    return NUM_VAL(floor(AS_NUM(a[0])));
}
static Value natCeil(int c, Value* a, Env* e) {
    (void)e;
    if (c != 1 || !IS_NUM(a[0])) { runtimeError("ceil expects a number"); return NIL_VAL; }
    return NUM_VAL(ceil(AS_NUM(a[0])));
}
static Value natRound(int c, Value* a, Env* e) {
    (void)e;
    if (c != 1 || !IS_NUM(a[0])) { runtimeError("round expects a number"); return NIL_VAL; }
    return NUM_VAL(round(AS_NUM(a[0])));
}
static Value natSqrt(int c, Value* a, Env* e) {
    (void)e;
    if (c != 1 || !IS_NUM(a[0])) { runtimeError("sqrt expects a number"); return NIL_VAL; }
    if (AS_NUM(a[0]) < 0) { runtimeError("sqrt: negative argument"); return NIL_VAL; }
    return NUM_VAL(sqrt(AS_NUM(a[0])));
}
static Value natPow(int c, Value* a, Env* e) {
    (void)e;
    if (c != 2 || !IS_NUM(a[0]) || !IS_NUM(a[1])) { runtimeError("pow expects two numbers"); return NIL_VAL; }
    return NUM_VAL(pow(AS_NUM(a[0]), AS_NUM(a[1])));
}
static Value natMin(int c, Value* a, Env* e) {
    (void)e;
    if (c < 1) { runtimeError("min expects at least 1 arg"); return NIL_VAL; }
    double m = AS_NUM(a[0]);
    for (int i = 1; i < c; i++) { if (!IS_NUM(a[i])) { runtimeError("min expects numbers"); return NIL_VAL; } if (AS_NUM(a[i]) < m) m = AS_NUM(a[i]); }
    return NUM_VAL(m);
}
static Value natMax(int c, Value* a, Env* e) {
    (void)e;
    if (c < 1) { runtimeError("max expects at least 1 arg"); return NIL_VAL; }
    double m = AS_NUM(a[0]);
    for (int i = 1; i < c; i++) { if (!IS_NUM(a[i])) { runtimeError("max expects numbers"); return NIL_VAL; } if (AS_NUM(a[i]) > m) m = AS_NUM(a[i]); }
    return NUM_VAL(m);
}

/* (last coll) / (butlast coll) / (append a b) / (reverse coll) */
static Value natLast(int c, Value* a, Env* e) {
    (void)e;
    if (c != 1) { runtimeError("last expects 1 arg"); return NIL_VAL; }
    int n = collCount(a[0]);
    if (n <= 0) { runtimeError("last expects a non-empty list"); return NIL_VAL; }
    Value item;
    if (!collAt(a[0], n - 1, &item)) {
        runtimeError("last: bad list");
        return NIL_VAL;
    }
    return item;      /* collAt 已 retain，转移所有权给调用方 */
}
static Value natReverse(int c, Value* a, Env* e) {
    (void)e;
    int n = collCount(a[0]);
    if (n < 0) { runtimeError("reverse expects a list or vector"); return NIL_VAL; }
    Value out = NIL_VAL;
    for (int i = n - 1; i >= 0; i--) {
        Value item;
        if (!collAt(a[0], i, &item)) continue;
        Value nh = prependCell(out, item);
        release(out);
        out = nh;
        release(item);
    }
    return out;
}
static Value natAppend(int c, Value* a, Env* e) {
    (void)e;
    if (c != 2) { runtimeError("append expects (a b)"); return NIL_VAL; }
    int na = collCount(a[0]), nb = collCount(a[1]);
    if (na < 0 || nb < 0) { runtimeError("append expects lists or vectors"); return NIL_VAL; }
    Value out = NIL_VAL;
    for (int i = nb - 1; i >= 0; i--) {
        Value item;
        if (collAt(a[1], i, &item)) {
            Value nh = prependCell(out, item);
            release(out); out = nh; release(item);
        }
    }
    for (int i = na - 1; i >= 0; i--) {
        Value item;
        if (collAt(a[0], i, &item)) {
            Value nh = prependCell(out, item);
            release(out); out = nh; release(item);
        }
    }
    return out;
}

/* ---------- 注册 ---------- */
void defNative(Env* g, const char* name, NativeFn fn) {
    ObjNative* nat = (ObjNative*)allocObj(sizeof(ObjNative), OBJ_NATIVE);
    nat->fn = fn;
    nat->name = (char*)malloc(strlen(name) + 1);
    strcpy(nat->name, name);
    ObjString* s = internString(name, (int)strlen(name), true);
    envDefine(g, s, OBJ_VAL(nat));
}

void registerNatives(Env* g) {
    defNative(g, "+", natAdd);
    defNative(g, "-", natSub);
    defNative(g, "*", natMul);
    defNative(g, "/", natDiv);
    defNative(g, "%", natMod);
    defNative(g, "=", natNumEq);
    defNative(g, "<", natLt);
    defNative(g, ">", natGt);
    defNative(g, "<=", natLe);
    defNative(g, ">=", natGe);
    defNative(g, "not", natNot);
    defNative(g, "equal?", natEqual);
    defNative(g, "eq?", natEq);
    defNative(g, "print", natPrint);
    defNative(g, "println", natPrintln);
    defNative(g, "cons", natCons);
    defNative(g, "car", natCar);
    defNative(g, "cdr", natCdr);
    defNative(g, "list", natList);
    defNative(g, "length", natLength);
    defNative(g, "substring", natSubstring);
    defNative(g, "string-search", natStringSearch);
    defNative(g, "nth", natNth);
    defNative(g, "get", natGet);
    defNative(g, "set", natSet);
    defNative(g, "vec", natVec);
    defNative(g, "str", natStr);
    defNative(g, "input", natInput);
    defNative(g, "input-int", natInputInt);

    /* 终端 / 游戏原语（cls / home / show-cursor / sleep / key / random / draw） */
    defNative(g, "cls", natCls);
    defNative(g, "home", natHome);
    defNative(g, "show-cursor", natShowCursor);
    defNative(g, "sleep", natSleep);
    defNative(g, "key", natKey);
    defNative(g, "random", natRandom);
    defNative(g, "draw", natDraw);

    /* 文件系统原语（壳语言基本功；软错误：失败返回 nil/false/-1） */
    defNative(g, "exists?", natExists);
    defNative(g, "is-dir?", natIsDir);
    defNative(g, "delete", natDelete);
    defNative(g, "rmdir", natRmdir);
    defNative(g, "mkdir", natMkdir);
    defNative(g, "file-size", natFileSize);
    defNative(g, "read-file", natReadFile);
    defNative(g, "write-file", natWriteFile);
    defNative(g, "append-file", natAppendFile);
    defNative(g, "mv", natMv);
    defNative(g, "cp", natCp);
    defNative(g, "ls", natLs);

    /* JSON 组单独成文件（src/json.c）：解析器代码量大，混在这里会难维护 */
    registerJsonNatives(g);
    /* HTTP 组单独成文件（src/http.c）：WinHTTP 动态加载，代码量大 */
    registerHttpNatives(g);
    registerProcNatives(g);   /* 进程组单独成文件 */
    registerWmNatives(g);     /* 窗口组单独成文件（src/wm.c） */

    /* 高阶函数与列表工具 */
    defNative(g, "map", natMap);
    defNative(g, "filter", natFilter);
    defNative(g, "reduce", natReduce);
    defNative(g, "range", natRange);
    defNative(g, "null?", natNull);
    defNative(g, "assoc", natAssoc);
    defNative(g, "member?", natMember);

    /* 类型判断 */
    defNative(g, "number?", natNumberP);
    defNative(g, "string?", natStringP);
    defNative(g, "list?", natListP);
    defNative(g, "vector?", natVectorP);
    defNative(g, "function?", natFunctionP);
    defNative(g, "bool?", natBoolP);

    /* 数学函数 */
    defNative(g, "abs", natAbs);
    defNative(g, "floor", natFloor);
    defNative(g, "ceil", natCeil);
    defNative(g, "round", natRound);
    defNative(g, "sqrt", natSqrt);
    defNative(g, "pow", natPow);
    defNative(g, "min", natMin);
    defNative(g, "max", natMax);

    /* 序列工具 */
    defNative(g, "last", natLast);
    defNative(g, "reverse", natReverse);
    defNative(g, "append", natAppend);
}
