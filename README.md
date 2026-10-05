# nccl-cc

一个用 C 语言从零编写的 **类 C 语言编译器**，目标平台为 **x86-64 Windows（Microsoft x64 调用约定）**。
编译器按“每课新增一个语言特性”的方式增量实现，从最简单的“返回常量”一路生长到支持完整的 C 子集。

生成的汇编为 AT&T 语法，使用随附的 MinGW-w64 `gcc` 进行汇编与链接。

## 路线图

| 课程 | 语言特性 | 状态 |
| ---- | -------- | ---- |
| step1  | 返回常量 (`return N;`) | ✅ |
| step2  | 一元运算 (`+ - ! ~`) | ⬜ |
| step3  | 二元算术 (`+ - * / %`) | ⬜ |
| step4  | 比较与逻辑 (`== != < <= > >= && \|\| & \| ^`) | ⬜ |
| step5  | 局部变量与多条语句 | ⬜ |
| step6  | 全局变量 | ⬜ |
| step7  | 赋值 | ⬜ |
| step8  | 条件分支 (`if`/`else`/`?:`) | ⬜ |
| step9  | 循环 (`for`/`while`) | ⬜ |
| step10 | 函数定义与调用 | ⬜ |
| step11 | 递归 | ⬜ |
| step12 | 字符与字符串 | ⬜ |
| step13 | 数组 | ⬜ |
| step14 | 指针 | ⬜ |
| step15 | 结构体 | ⬜ |
| step16 | 预处理器 | ⬜ |
| step17 | 完整 C 子集 | ⬜ |

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
./ncclcc -S -o out.s input.c   # 仅生成汇编
./ncclcc -o out.exe input.c    # 生成汇编并汇编、链接成可执行文件
```

## 测试

```sh
./test.sh
```

## 目标平台说明

生成代码遵循 **Microsoft x64 调用约定**：

- 前 4 个整型参数放入 `rcx, rdx, r8, r9`，其余压栈；
- 调用方预留 32 字节 shadow space；
- 栈保持 16 字节对齐；
- 返回值置于 `rax`。

因此生成的可执行文件可在 Windows 上由 MinGW-w64 `gcc` 直接链接并原生运行。

## 目录结构

```
ncclcc.h     公共声明
tokenize.c   词法分析
parse.c      语法分析（递归下降）与 AST
codegen.c    代码生成（x86-64）
main.c       命令行驱动
test.sh      测试套件
```

## 许可证

MIT
