# nccl-cc

一个用 C 语言从零编写的 **类 C 语言编译器**，目标平台为 **x86-64 Windows（Microsoft x64 调用约定）**。
编译器按“每课新增一个语言特性”的方式增量实现，从最简单的“返回常量”一路生长到支持完整的 C 子集。

生成的汇编为 AT&T 语法，使用随附的 MinGW-w64 `gcc` 进行汇编与链接。

## 路线图

| 课程 | 语言特性 | 状态 |
| ---- | -------- | ---- |
| step1  | 返回常量 (`return N;`) | ✅ |
| step2  | 一元运算 (`+ - ! ~`) | ✅ |
| step3  | 二元算术 (`+ - * / %`) | ✅ |
| step4  | 比较、逻辑与位运算 (`== != < <= > >= && \|\| & \| ^`) | ✅ |
| step5  | 局部变量与多条语句 | ✅ |
| step6  | 全局变量 | ✅ |
| step7  | 赋值 (`=`) | ✅ |
| step8  | 条件分支 (`if`/`else`/`?:`、代码块) | ✅ |
| step9  | 循环 (`for`/`while`/`break`/`continue`) | ✅ |
| step10 | 函数定义与调用 | ✅ |
| step11 | 递归 | ✅ |
| step12 | 字符与字符串 | ✅ |
| step13 | 数组 | ✅ |
| step14 | 指针 | ✅ |
| step15 | 结构体 | ✅ |
| step16 | 预处理器 | ✅ |
| step17 | 完整 C 子集 | ✅ |

## 支持的 C 子集

- **类型**：`char`、`int`、指针、数组（含多维）、结构体、`enum`；`long`/`short`/`unsigned`/`signed`/`void` 作为 `int` 的近义词处理。
- **运算符**：全部算术、比较、逻辑（短路）、位运算、移位；一元 `+ - ! ~ & *`、前置/后置 `++ --`、赋值与复合赋值、逗号运算符、`?:`、类型转换。
- **语句**：表达式语句、`return`、`if`/`else`、`for`、`while`、`do`/`while`、`switch`/`case`/`default`、`break`/`continue`、代码块。
- **声明**：局部/全局变量（含常量初始化）、函数（含多参数与递归）、`typedef`、`sizeof`、结构体标签与匿名结构体。
- **预处理器**：对象式与函数式宏、`#undef`、`#ifdef`/`#ifndef`/`#if`/`#elif`/`#else`/`#endif`、`#include "..."` 与 `<...>`、`#pragma`。
- **外部函数**：可直接调用 `printf` 等 C 运行库函数（遵循 Windows x64 ABI）。

## 构建

Git Bash / MSYS2 / Linux：

```sh
./build.sh
```

Windows `cmd`：

```bat
build.bat
```

## 使用

```sh
./ncclcc -S -o out.s input.c     # 仅生成汇编
./ncclcc -o out.exe input.c      # 生成汇编并汇编、链接成可执行文件
./ncclcc -Iinclude -o out.exe a.c
```

## 测试

```sh
./test.sh
```

测试套件会编译并运行完整的小程序，校验进程退出码（因此测试中程序返回值均在 0–255 范围内）。

## 目标平台说明

生成代码遵循 **Microsoft x64 调用约定**：

- 前 4 个整型参数放入 `rcx, rdx, r8, r9`，其余从 `[rbp+48]` 起放在栈上；
- 调用方预留 32 字节 shadow space，并保持 16 字节栈对齐；
- 返回值置于 `rax`；
- 变参调用前清空 `al`。

因此生成的可执行文件可在 Windows 上由 MinGW-w64 `gcc` 直接链接并原生运行。

## 目录结构

```
ncclcc.h      公共声明
tokenize.c    词法分析（关键字、字符/字符串字面量、行首/空格标记）
preprocess.c  预处理器（宏、条件编译、#include）
parse.c       语法分析（递归下降）与 AST
type.c        基本类型（char / int）
codegen.c     代码生成（x86-64，Microsoft ABI）
main.c        命令行驱动
test.sh       测试套件
```

## 实现说明

- 遵循“每课一个提交”的增量开发方式，`git log` 可看到从 step1 到 step17 的完整演进。
- 为简化实现，`int` 使用 8 字节存储（与 `long` 一致）；未实现浮点数与按值传递的结构体。
- 说明：本项目的提交历史中不含任何 `Co-authored-by` 尾注，`git log` 中仅有一位作者。

## 许可证

MIT
