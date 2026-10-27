// SPDX-License-Identifier: GPL-2.0
/*
 * env.c — 环境（作用域链）
 * 变量绑定保存在一个简单的动态数组里，沿 parent 链向上查找。
 */
#include "nuomi.h"

Env* newEnv(Env* parent) {
    Env* e = (Env*)allocObj(sizeof(Env), OBJ_ENV);
    e->parent = parent;
    if (parent) retain(OBJ_VAL(parent));
    e->count = 0;
    e->capacity = 0;
    e->keys = NULL;
    e->values = NULL;
    return e;
}

void envDefine(Env* e, ObjString* key, Value val) {
    for (int i = 0; i < e->count; i++) {
        if (e->keys[i] == key) {        /* 同名覆盖 */
            release(e->values[i]);
            e->values[i] = val;
            retain(val);
            return;
        }
    }
    if (e->count == e->capacity) {
        int nc = e->capacity < 8 ? 8 : e->capacity * 2;
        e->keys = (ObjString**)realloc(e->keys, nc * sizeof(ObjString*));
        e->values = (Value*)realloc(e->values, nc * sizeof(Value));
        e->capacity = nc;
    }
    e->keys[e->count] = key;   retain(OBJ_VAL(key));
    e->values[e->count] = val; retain(val);
    e->count++;
}

/* 沿作用域链查找，返回值的槽位指针；找不到返回 NULL */
Value* envFind(Env* e, ObjString* key) {
    for (Env* cur = e; cur; cur = cur->parent) {
        for (int i = 0; i < cur->count; i++)
            if (cur->keys[i] == key) return &cur->values[i];
    }
    return NULL;
}
