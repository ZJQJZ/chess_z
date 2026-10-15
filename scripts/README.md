# C 格式化

`Option+Shift+F` 使用 `clang_format_with_alignment.py`：先运行原生 clang-format，
再把连续的简单变量声明中的初始化 `=` 对齐，允许中间夹着没有初始化的声明。
声明顺序和初始化写法保持不变。例如：

```c
bool   rejected[XQ_MAX_MOVES] = {false};
size_t end;
size_t length;
int    count                  = 0;
int    original_count         = list->count;
```

工作区的 `C_Cpp.clang_format_path` 已接入这个适配器，需要 Python 3。
设置通过 `${workspaceFolder:chess_z}` 引用当前工作区；如果改变工作区文件夹名称，
需同步更新这个名称。
它自动寻找已安装的 VS Code C/C++ 扩展自带的 clang-format，再尝试 `PATH`；
也可以用环境变量 `CHESS_CLANG_FORMAT_BINARY` 指定实际 clang-format 的路径。
该变量不能指向包装脚本本身。

对齐只处理同一缩进下连续的单行、单变量声明。空行、注释、宏、普通语句及复杂声明
会结束一组；对齐后任一行超过有效配置中的 `ColumnLimit` 时，保留该组的原生格式。
`TabWidth` 和 `AlignConsecutiveAssignments` 也从 clang-format 的有效配置读取，
关闭赋值对齐时不会额外开启。`clang-format off/on` 区域保留原样。
选区格式化只额外处理完全包含在选区内的声明组。

命令行从 stdin 读取 `.c` / `.h` 时，也可以使用同一流程：

```sh
scripts/clang_format_with_alignment.py --assume-filename=src/engine.c < src/engine.c
```

VS Code 使用的 XML replacements 模式同样受支持。直接传入文件名、`-i`、
`--dry-run`、`--version`、`--dump-config` 等操作会原样交给 clang-format；
这些文件操作不增加声明对齐。没有 `.c` / `.h` 的 `--assume-filename`、其他语言
以及文本模式的 `--cursor` 也沿用原生行为。

## C 注释重排

`clang-format` 的 `ReflowComments: true` 会折行，但可能保留已有短行。
本项目的 `format_c_comments.py` 把块注释中的普通文字段落
及 `@brief`、`@param`、`@return`、`@retval` 内容合并并按 100 列重新折行。
只额外处理独占行的块注释。段落之间的空行、列表、表格、缩进代码及带代码围栏或
Doxygen 代码块的注释保留边界。
`clang-format off/on` 区域不会进行额外重排。`//` 注释仍使用 clang-format 的行为。

`Ctrl+Command+Shift+F` 先保存当前 C 文件，再通过“C: 格式化 Doxygen 注释”任务
运行 `format_c_comments.py`，只处理注释。该功能需要 Python 3。
也可以独立运行 `python3 scripts/format_c_comments.py src/engine.c`。

`Option+Shift+F` 使用 `.clang-format` 配置及上面的声明对齐步骤，
工作区没有启用额外的注释重排适配器。`Command+Shift+F` 保持原来的全局搜索行为。
`clang_format_with_comments.py` 文件保留，但未接入当前编辑器的格式化流程。

注释重排验证：

```sh
python3 -m unittest discover -s tests -p 'test_format_comments.py'
```
