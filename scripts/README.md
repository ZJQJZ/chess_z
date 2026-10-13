# C 注释格式化

`clang-format` 的 `ReflowComments: true` 会折行，但可能保留已有短行。
本项目的 `clang_format_with_comments.py` 在整篇格式化后，把块注释中的普通文字段落
及 `@brief`、`@param`、`@return`、`@retval` 内容合并并按 100 列重新折行。
只额外处理独占行的块注释。段落之间的空行、列表、表格、缩进代码及带代码围栏或
Doxygen 代码块的注释保留边界。
`clang-format off/on` 区域不会进行额外重排。`//` 注释仍使用 clang-format 的行为。

工作区 `.vscode/settings.json` 已配置该适配器，`Cmd+Option+F` 整篇格式化即可使用。
`.vscode/` 被 Git 忽略；新检出可在设置中加入：

```json
"C_Cpp.formatting": "clangFormat",
"C_Cpp.clang_format_style": "file",
"C_Cpp.clang_format_path": "${workspaceFolder}/scripts/clang_format_with_comments.py"
```

适配器需要 Python 3，优先使用本机 VS Code C/C++ 扩展附带的 clang-format，
也可通过 `CHESS_CLANG_FORMAT_BINARY` 环境变量指定二进制文件。
当前适配器面向 macOS/Linux。格式化选区仍交给 clang-format，不额外重排选区之外的注释。
适配器只扩展编辑器使用的 XML 替换协议；`--version` 等其他调用直接转发。

独立使用 `python3 scripts/format_c_comments.py src/engine.c` 也能重排块注释，
现有“C: 格式化 Doxygen 注释”任务继续可用。

验证：

```sh
python3 -m unittest discover -s tests -p 'test_format_comments.py'
```
