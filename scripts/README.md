# C 注释格式化

`clang-format` 的 `ReflowComments: true` 会折行，但可能保留已有短行。
本项目的 `format_c_comments.py` 把块注释中的普通文字段落
及 `@brief`、`@param`、`@return`、`@retval` 内容合并并按 100 列重新折行。
只额外处理独占行的块注释。段落之间的空行、列表、表格、缩进代码及带代码围栏或
Doxygen 代码块的注释保留边界。
`clang-format off/on` 区域不会进行额外重排。`//` 注释仍使用 clang-format 的行为。

`Ctrl+Command+Shift+F` 先保存当前 C 文件，再通过“C: 格式化 Doxygen 注释”任务
运行 `format_c_comments.py`，只处理注释。该功能需要 Python 3。
也可以独立运行 `python3 scripts/format_c_comments.py src/engine.c`。

`Option+Shift+F` 仍使用原生 clang-format 和 `.clang-format` 配置进行整篇格式化，
工作区没有启用额外的注释重排适配器。`Command+Shift+F` 保持原来的全局搜索行为。
`clang_format_with_comments.py` 文件保留，但未接入当前编辑器的格式化流程。

验证：

```sh
python3 -m unittest discover -s tests -p 'test_format_comments.py'
```
