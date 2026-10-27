// SPDX-License-Identifier: GPL-2.0
/*
 * codegen.h — Nuomi → NUC 原生可执行文件 的 x86_64 代码生成（后端路线 A）
 *
 * 里程碑 1（当前）：仅支持「整数算术子集」——
 *   - 整数字面量
 *   - (+ - *) 运算（可嵌套、可多目）
 *   - (define name expr) 把整数常量登记进编译期符号表，供后续表达式引用
 * 编译产物是一段 x86_64 机器码（塞进 .nuc 的 .text 段），入口把表达式结果
 * 留在 RAX 并返回。后续里程碑再扩展：函数调用 / 闭包 / 运行时(GC、内置函数) /
 * 字符串 / 系统调用输出。
 *
 * 调用方负责 free(*out)。
 */
#ifndef NUOMI_CODEGEN_H
#define NUOMI_CODEGEN_H
#include <stddef.h>
#include <stdint.h>

int nuomi_compile_to_nuc(const char* src, uint8_t** out, size_t* out_size,
                         char* err, size_t errlen);

#endif /* NUOMI_CODEGEN_H */
