# nccl-cc

一个用 C 语言从零编写的 **类 C 语言编译器**，目标平台为 **x86-64 Windows**。

生成的汇编为 AT&T 语法，使用随附的 MinGW-w64 `gcc` 进行汇编与链接。

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

## 目标平台说明

生成的可执行文件可在 Windows 上由 MinGW-w64 `gcc` 直接链接并原生运行。

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

## 许可证

MIT
