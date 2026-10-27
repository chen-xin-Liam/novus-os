# Nuomi 语言手册（格式 + 代码参考）

> 版本：0.1（树遍历解释器实现）
> 定位：一门用 C 从零手撸的**极简 Lisp 风格**编程语言，产物最终要能打包成 novus-os 的 `.nuc` 原生可执行格式。
> 本手册覆盖：**词法格式 / 求值规则 / 特殊形式 / 内置函数 / 示例程序 / 已知限制**。

---

## 1. 怎么跑起来

```bash
# 编译（本机用绿色版 TCC 即可，无需安装）
TCC=/c/Users/RAYN/AppData/Local/tcc/tcc/tcc.exe
$TCC -Iinclude -O2 src/value.c src/reader.c src/env.c src/eval.c src/native.c src/main.c -o nuomi.exe

# 跑一个脚本文件
./nuomi.exe samples/factorial.nuomi

# 进入 REPL（逐行输入表达式回车求值，Ctrl+Z 回车或 Ctrl+C 退出）
./nuomi.exe
```

> 注：`nucutil.exe` 是配套的 **NUC 格式工具链**（生成/解析/校验/转储 `.nuc`），与 Nuomi 解释器是两个独立程序。

---

## 2. 词法格式（Lexical Format）

Nuomi 的源码就是一堆 **S-表达式（S-expression）**，整体长得像 Lisp。

| 元素 | 写法 | 说明 |
|------|------|------|
| 注释 | `; 这是注释` | 从 `;` 到行尾都是注释 |
| 数字 | `42` `3.14` `-0.5` | 统一为双精度浮点 `double` |
| 布尔 | `true` `false` | 逻辑真假 |
| 空值 | `nil` | 类似 null / 空列表 |
| 符号 | `x` `+` `add` `list?` | 标识符 / 变量名 / 函数名（可含 `?` `!` 等） |
| 字符串 | `"hello"` | 双引号包裹，支持转义（见下） |
| 列表 | `(a b c)` | 用圆括号，逗号可写可不写 |
| 向量 | `[1 2 3]` | 用方括号，表示**可随机访问的数组** |
| 引号缩写 | `'x` | 等价于 `(quote x)`，原样返回不求值 |

**字符串转义**：支持 `\"` `\\` `\n`（换行）`\t`（制表）`\r` 等标准转义。

**几个要点**
- 表达式之间用空白（空格 / 换行 / 制表）分隔，不需要逗号，但写了也不影响（会被当作分隔符）。
- `'x` 是 `(quote x)` 的语法糖，常用于写“数据”而不是“代码”，例如 `'(1 2 3)` 是一个列表数据，`(list 1 2 3)` 是调用函数去构造。

---

## 3. 求值规则（Evaluation）

1. **数字 / 布尔 / nil**：自求值，原样返回。
2. **字符串字面量**：自求值，返回该字符串。
3. **符号（变量名）**：在环境里查找并取值；找不到报 `unbound variable`。
4. **列表 `(head arg ...)`**：
   - 若 `head` 是**特殊形式关键词**（见第 4 节），按特殊形式规则求值。
   - 否则：先求值 `head`（应得到一个函数或原生函数），再从左往右求值各参数，最后**调用**。
5. **向量 `[...]`**：作为“数据”时自成一个向量值；若出现在调用位置（很少见）按列表的调用规则处理。

---

## 4. 特殊形式（Special Forms）

特殊形式是“关键字”，它们的参数**不会**被默认先求值。

### `quote` — 原样返回
```nuomi
(quote (+ 1 2))     ; => 返回列表 (+ 1 2)，不会去算 3
'(1 2 3)             ; => 列表数据 (1 2 3)
```

### `define` — 定义变量或函数
```nuomi
(define x 10)                 ; 定义变量
(define (add a b) (+ a b))    ; 定义函数（第一个参数是 (函数名 形参...)）
(add 3 4)                     ; => 7
```

### `if` — 条件分支
```nuomi
(if (< x 0) "负" "非负")      ; 第三个（else）分支可省略，省略时返回 nil
```

### `while` — 循环
```nuomi
(define i 0)
(while (< i 3)
  (println i)
  (set! i (+ i 1)))           ; 返回最后一次循环体的值；一次都没跑返回 nil
```

### `begin` — 顺序求值，返回最后一个
```nuomi
(begin (println "a") (println "b") 99)   ; 依次执行，最终值 99
```

### `set!` — 修改已有绑定（变量必须已 define，否则报 `unbound variable`）
```nuomi
(define count 0)
(set! count (+ count 1))      ; count 变成 1
```

### `lambda` — 构造匿名函数（闭包）
```nuomi
(define make-adder
  (lambda (n)
    (lambda (x) (+ x n))))    ; 内层闭包捕获了外层变量 n
(define add5 (make-adder 5))
(add5 10)                     ; => 15
```

### `and` / `or` — 短路逻辑
```nuomi
(and true (< 1 2) "ok")       ; => "ok"（真到最后一个）
(or false nil 42)              ; => 42（遇到第一个真值即返回）
```

### `let` — 局部作用域（★ 最重要）
```nuomi
(let ((x 1) (y 2))
  (println (+ x y)))           ; => 3，不污染全局

;; 带初值的并行绑定：所有 init 先在外层求值
(define ox 10)
(let ((a 1) (b ox)) (list a b))  ; => (1 10)
```

### `let*` — 顺序绑定
```nuomi
(let* ((a 1)
       (b (+ a 10))             ; ← 这里已经能看到 a
       (c (* b 2)))
  (+ a b c))                   ; => 34
```

### `letrec` — 局部递归 / 互递归
```nuomi
(letrec ((even? (lambda (n) (if (= n 0) true  (odd?  (- n 1)))))
         (odd?  (lambda (n) (if (= n 0) false (even? (- n 1))))))
  (even? 10))                  ; => true

;; 局部递归：素数试除
(letrec ((check (lambda (d) (if (> (* d d) n) true
                                 (if (= (% n d) 0) false (check (+ d 1)))))))
  (check 2))
```

### 带状态的闭包 — `let` + `set!`
```nuomi
(define (make-counter)
  (let ((n 0))                 ; ← n 被闭包捕获，成为私有状态
    (lambda ()
      (set! n (+ n 1))
      n)))
(define c1 (make-counter))
(list (c1) (c1) (c1))          ; => (1 2 3)，每次调用独立
```

---

## 5. 内置函数（Built-in Functions）

> 参数里的“值”可以是任意 Nuomi 值。除非特别说明，函数都**支持不定个数参数**。

### 5.1 算术
| 函数 | 语义 | 示例 |
|------|------|------|
| `(+ a b …)` | 求和 | `(+ 1 2 3)` => `6` |
| `(- a b …)` | 相减；**只有一个参数时取负** | `(- 10 3)` => `7`；`(- 5)` => `-5` |
| `(* a b …)` | 求积 | `(* 2 3 4)` => `24` |
| `(/ a b …)` | 相除；**只有一个参数时等于 `1/x`** | `(/ 12 3)` => `4`；`(/ 2)` => `0.5` |
| `(% a b)` | 取模（要求恰好 2 个数，fmod） | `(% 10 3)` => `1` |
| `(abs x)` | 绝对值 | `(abs -7)` => `7` |
| `(floor x)` / `(ceil x)` / `(round x)` | 下取整 / 上取整 / 四舍五入 | `(floor 3.9)` => `3` |
| `(sqrt x)` | 平方根（负数报错） | `(sqrt 144)` => `12` |
| `(pow a b)` | 幂 | `(pow 3 4)` => `81` |
| `(min a …)` / `(max a …)` | 最小 / 最大 | `(min 5 2 8)` => `2` |

### 5.2 比较（链式，全部成立才为真）
| 函数 | 语义 |
|------|------|
| `(= a b …)` | 数值相等 |
| `(< a b …)` | 严格小于 |
| `(> a b …)` | 严格大于 |
| `(<= a b …)` | 小于等于 |
| `(>= a b …)` | 大于等于 |

### 5.3 逻辑与相等性
| 函数 | 语义 |
|------|------|
| `(not x)` | 逻辑取反（假值 → true） |
| `(equal? a b …)` | **结构相等**：列表/向量递归比较内容，字符串比字符，其他比值 |
| `(eq? a b …)` | **同一性**：对象看指针是否同一块；nil/bool/number 比值 |

### 5.4 输出
| 函数 | 语义 |
|------|------|
| `(print a b …)` | 以空格分隔打印各参数，不换行 |
| `(println a b …)` | 同上，末尾换行 |

打印列表会显示为 `(1 2 3)`，向量显示为 `[1 2 3]`，字符串不带引号（除非用 `str` 拼接）。

### 5.5 列表（单向链表）
| 函数 | 语义 | 示例 |
|------|------|------|
| `(cons x lst)` | 把 `x` 接到列表 `lst` 头部 | `(cons 1 '(2 3))` => `(1 2 3)` |
| `(car lst)` | 取第一个元素；不是 pair/nil 时报错 | `(car '(1 2))` => `1` |
| `(cdr lst)` | 取除第一个以外的剩余部分 | `(cdr '(1 2 3))` => `(2 3)` |
| `(list a b …)` | 用参数构造一个列表 | `(list 1 2 3)` => `(1 2 3)` |
| `(length coll)` | **列表/向量/字符串**的长度 | `(length '(1 2 3))` => `3` |
| `(nth coll i)` | 取第 `i` 个元素（0 基），越界报错；支持列表与向量 | `(nth '(a b c) 1)` => `b` |

### 5.6 向量（数组，可随机访问、可原地修改）
| 函数 | 语义 | 示例 |
|------|------|------|
| `[a b c]` | 字面量语法，直接构造向量 | `[10 20 30]` |
| `(vec a b …)` | 用参数构造向量 | `(vec 1 2 3)` => `[1 2 3]` |
| `(get v i)` | 取第 `i` 个元素（0 基）；**也支持字符串**（返回长度为 1 的字符串）和列表 | `(get [10 20 30] 1)` => `20` |
| `(set v i x)` | **原地修改**向量 `v` 第 `i` 位为 `x`（仅向量可用），返回 `x` | `(set v 1 99)` |
| `(length v)` | 向量长度（同 5.5 的 `length`） | |
| `(nth v i)` | 向量随机访问（同 5.5 的 `nth`） | |

### 5.7 字符串
| 函数 | 语义 | 示例 |
|------|------|------|
| `(str a b …)` | 把各参数的字符串表示**拼接**成一个字符串 | `(str "Hello, " "Nuomi" "!")` => `"Hello, Nuomi!"` |
| `(get s i)` | 取字符串第 `i` 个字符（返回长度 1 的字符串） | `(get "abc" 0)` => `"a"` |
| `(length s)` | 字符串字符数 | `(length "abc")` => `3` |

### 5.8 高阶函数（★ 写复杂逻辑的主力）
| 函数 | 语义 | 示例 |
|------|------|------|
| `(map fn coll)` | 逐元素映射，返回新列表 | `(map (lambda (v) (* v v)) [1 2 3])` => `(1 4 9)` |
| `(filter pred coll)` | 保留 pred 为真的元素 | `(filter (lambda (v) (> v 2)) [1 2 3 4])` => `(3 4)` |
| `(reduce fn coll)` | 归约（无初值，取首元素起算） | `(reduce + [1 2 3 4])` => `10` |
| `(reduce fn init coll)` | 归约（带初值） | `(reduce + 100 [1 2 3])` => `106` |

**流水线写法**（这是 Nuomi 里最像样的数据处理方式）：
```nuomi
(map (lambda (v) (* v v))
     (filter (lambda (v) (> v 2))
             (range 1 11)))
;=> (9 16 25 36 49 64 81 100)
```

### 5.9 序列工具
| 函数 | 语义 | 示例 |
|------|------|------|
| `(range n)` | `0 … n-1` | `(range 5)` => `(0 1 2 3 4)` |
| `(range start end)` | `start … end-1` | `(range 2 5)` => `(2 3 4)` |
| `(range start end step)` | 带步长（不含 end） | `(range 0 10 2)` => `(0 2 4 6 8)` |
| `(last coll)` | 最后一个元素 | `(last '(1 2 3))` => `3` |
| `(reverse coll)` | 反转 | `(reverse [1 2 3])` => `(3 2 1)` |
| `(append a b)` | 拼接 | `(append [1 2] [3 4])` => `(1 2 3 4)` |
| `(null? x)` | 是否为空 | `(null? [])` => `true` |
| `(member? x coll)` | 是否包含（结构相等） | `(member? 2 [1 2 3])` => `true` |

### 5.10 关联表
| 函数 | 语义 |
|------|------|
| `(assoc lst key val)` | 替换匹配 key 的条目，无匹配则在表头新增；返回新表 |
| `(assoc lst key)` | 只查询，找不到返回 `nil` |

```nuomi
(define alist (assoc (assoc '() 'apple 3) 'banana 5))
(println alist)              ;=> (("banana" 5) ("apple" 3))
(println (assoc alist 'apple 10))  ;=> (("apple" 10) ("banana" 5))
```

### 5.11 类型判断
`(number? x)` `(string? x)` `(list? x)` `(vector? x)` `(function? x)` `(bool? x)`

---

## 6. 函数与闭包

- **定义**：`(define (名字 形参...) 体...)`。形参是普通符号，调用时按位置绑定。
- **调用**：`(名字 实参...)`，实参个数必须等于形参个数（arity mismatch 会报错）。
- **闭包**：`lambda` 和定义的函数都会捕获“定义时所在的环境”，因此可以引用外层变量（见 4 节 `make-adder` 例子）。
- **递归**：直接递归即可（`fact` 示例，`(define (fact n) (if (= n 0) 1 (* n (fact (- n 1)))))`）。

### 6.1 递归深度限制 ⚠️

求值器会为每个嵌套表达式消耗一层栈，因此递归有上限：

- **约3700+ 层函数递归**是安全的（实测`(depth 3000)` 正常）
- 超过会输出可读报错 `maximum recursion depth exceeded (stack exhausted)` 并终止，**不会崩溃**
- 写深层递归时用 `while` 或 `map`/`filter` 等高阶函数替代

---

## 7. 示例程序（随仓库附带，在 `samples/` 目录）

| 文件 | 演示点 |
|------|--------|
| `factorial.nuomi` | 递归 + 算术（`fact(10)` => `3628800`） |
| `fib.nuomi` | 递归斐波那契（`fib(20)` => `6765`） |
| `closures.nuomi` | 闭包 / 高阶函数 |
| `vectors.nuomi` | 向量字面量、`get` / `set` / `length` |
| `while.nuomi` | `while` 循环累加（`sum 0..99` => `4950`） |
| `strings.nuomi` | `str` 拼接与字符串处理 |
| `logic.nuomi` | `and` / `or` / `not` / 比较 |
| `lists.nuomi` | `list` / `car` / `cdr` / `nth` 链表操作 |

---

## 8. 与 NUC 格式的关系（重要背景）

Nuomi 当前是**解释器**（在宿主机上读源码运行）。它的**最终目标产物**是 novus-os 的 `.nuc` 原生可执行文件，由 NUC 工具链（`nucutil`）生成。`nucutil` 支持的命令：

```bash
./nucutil.exe emit-demo demo.nuc   # 生成一个带 PC32 重定位的最小示例 .nuc
./nucutil.exe validate  demo.nuc   # 按规范校验格式
./nucutil.exe dump      demo.nuc   # 人类可读地转储
./nucutil.exe run       demo.nuc   # （仅 Windows）真机加载并执行，验证返回 42
```

> 让 Nuomi 真正编译成能在 novus-os 跑的 `.nuc`，有两条候选后端路线待定：
> **A** 直接生成 x86_64 机器码塞进 `.text` 段（对齐 novus-os 的 syscall/cppcc ABI）；
> **B** 内嵌一个字节码 VM，字节码放 `.data`，VM 本身由 cppcc 编成 `.nuc` 作为运行时。

---

## 9. 已知限制（Current Limitations）

1. **数字只有 `double`**：没有整数/浮点区分，大整数会丢失精度。
2. **引用计数是“带环会漏”的**：循环引用（如互指的环境/闭包）不会被回收，目前靠程序退出时 `freeAllObjects()` 强制清理。
3. **函数不支持可变参数**：实参个数必须严格匹配形参个数。
4. **宏 / 模块 / 异常处理** 等高级特性尚未实现。
5. **内存稳定性**：解释器在跑大量/复杂样例时偶发的不确定性内存错误（use-after-free 类）正在排查中——跑单个样例稳定，连续批量跑有概率触发，定位中。

---

## 10. 最小上手示例

```nuomi
; hello.nuomi
(define (fact n)
  (if (= n 0)
      1
      (* n (fact (- n 1)))))

(println "fact(10) =" (fact 10))     ; 输出 fact(10) = 3628800

(define v [1 2 3 4 5])
(println "sum =" (+ (get v 0) (get v 4)))   ; 输出 sum = 6

(define greet (lambda (name) (str "Hi, " name)))
(println (greet "Nuomi"))                    ; 输出 Hi, Nuomi
```
