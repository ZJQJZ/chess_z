# Smart Expand Selection

一个不依赖 npm 包的本地 VS Code 扩展。它按以下自然边界逐级扩大选区，并在当前段落停止：

1. Unicode 单词或代码标识符
2. 环绕符内部与包含环绕符的外部
3. 句子
4. 当前物理行
5. 当前段落

候选始终按实际范围从小到大选择，因此嵌套括号、引号与句子的具体顺序取决于文本结构。

## 快捷键

- `Ctrl+Q`：扩展到下一级。
- `Ctrl+Shift+Q`：退回本轮扩展的上一级。

快捷键只在普通文本编辑器获得焦点时生效，不会占用集成终端中的按键。

## 安装

在仓库根目录执行：

```sh
ln -sfn "$PWD/tools/vscode-smart-expand" \
  "$HOME/.vscode/extensions/local.smart-expand-0.1.0"
```

随后在命令面板执行 `Developer: Reload Window`。若以后移动了仓库，需要重新创建软链接。

也可以复制安装：

```sh
cp -R tools/vscode-smart-expand \
  "$HOME/.vscode/extensions/local.smart-expand-0.1.0"
```

## VSCodeVim

VSCodeVim 默认可能接管 `Ctrl+Q`。当前机器的用户快捷键已经包含对应的解绑规则；其他环境可在 `keybindings.json` 中加入：

```json
{
  "key": "ctrl+q",
  "command": "-extension.vim_winCtrlQ",
  "when": "editorTextFocus && vim.active && vim.use<C-q> && !inDebugRepl"
}
```

也可以在 `settings.json` 的现有 `vim.handleKeys` 对象中加入：

```json
"<C-q>": false
```

扩展命令本身不依赖 Vim 状态，可在普通、插入和可视模式调用。

## 配置

- `smartExpand.includeLine`：是否加入完整物理行，默认开启。
- `smartExpand.wrapperPairs`：完整替换默认环绕符表；每项格式为 `["左", "右"]`。
- `smartExpand.enableMarkdownPairs`：识别 `**`、`__`、`~~`，默认关闭。
- `smartExpand.sentenceTerminators`：句末字符，默认 `. ! ? 。！ ？ ； ; …`。
- `smartExpand.maxParseCharacters`：句子和环绕符解析的最大段落长度，默认 `1000000`。

ASCII `<…>` 默认启用，但只有左右符号内侧都不是空白时才会匹配，以避免大多数比较表达式被误认为环绕结构。单引号位于单词内部时不会匹配，反斜杠转义的引号和反引号也会忽略。

## 测试

```sh
cd tools/vscode-smart-expand
npm test
```

测试只使用 Node 内置的 `node:test`，不会安装任何依赖。
