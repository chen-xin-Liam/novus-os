# Nuomi 语言 + NUC 工具链 Makefile（跨平台：Linux / Windows / macOS）
# 用 .RECIPEPREFIX 把配方前缀改成 ">"，避免 Makefile 必须的 Tab 缩进问题。
# 默认 gcc；novus-os 构建机可用 CROSS_COMPILE= 交叉编译（如 x86_64-elf-）。
.RECIPEPREFIX = >
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Iinclude

# nucfmt.c 是共享的 NUC 格式库：编译器用它写、宿主工具用它读，链接进 nuomi。
NUC_LIB  := src/nucfmt.c

# 单一二进制 nuomi：解释器 + -c 编译后端 (codegen.c) + NUC 工具链 (nucutil.c)。
# nucutil.c 的 NUC 子命令（dump/validate/emit-demo/run）已并入 nuomi，通过
# `nuomi nuc <cmd>` 调用，不再单独产出 nucutil 可执行文件。
LANG_SRC := src/value.c src/reader.c src/env.c src/eval.c src/native.c \
            src/codegen.c src/update.c src/nucutil.c src/main.c
SAMPLES  := $(wildcard samples/*.nuomi)

# fmod 来自 C 数学库，多数主机(Linux)上是独立的，需要 -lm。
LDLIBS ?= -lm

.PHONY: all nuomi test clean

all: nuomi

nuomi: $(LANG_SRC) $(NUC_LIB) include/nuomi.h include/nucfmt.h include/codegen.h
> $(CC) $(CFLAGS) $(LANG_SRC) $(NUC_LIB) -o nuomi $(LDLIBS)

# 冒烟测试：跑全部样例(解释器) + NUC 流水线(nuomi nuc: emit-demo -> validate -> run)
test: all
> @for f in $(SAMPLES); do echo "[run] $$f"; ./nuomi "$$f" >/dev/null || { echo "FAIL: $$f"; exit 1; }; done
> ./nuomi nuc emit-demo /tmp/nuomi-demo.nuc >/dev/null && \
    ./nuomi nuc validate /tmp/nuomi-demo.nuc && \
    ./nuomi nuc run /tmp/nuomi-demo.nuc
> @rm -f /tmp/nuomi-demo.nuc
> @echo "test: OK"

clean:
> rm -f nuomi nuomi.exe nucutil nucutil.exe
