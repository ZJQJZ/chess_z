# chess_z

一个用 C 语言实现的中国象棋软件框架。当前重点是规则核心和 AI 接入边界：

- 90 格棋盘使用双 `uint64_t` 位棋盘表示，每方每类棋子都有独立 bitboard。
- 同步维护 `board[90]` 邮箱数组，便于规则生成和调试。
- 支持初始局面、FEN 读写、走子、合法着法生成、将军检测和 perft。
- 提供 `XqEngineAdapter`，后续可以接入外部象棋 AI 搜索/评估函数。
- 内置一个很小的材料与合法着法机动性评估 + negamax 搜索，主要用于框架冒烟测试。

## 目录

```text
include/xiangqi/bitboard.h   位棋盘基础操作
include/xiangqi/types.h      基础类型、棋子、着法
include/xiangqi/position.h   局面表示、FEN、走子
include/xiangqi/movegen.h    伪合法/合法着法、将军检测、perft
include/xiangqi/engine.h     AI 引擎适配接口
src/                         核心实现
examples/cli.c               示例命令行入口
stats/generate_positions.c   随机合法局面生成入口
stats/search_positions.c     批量局面搜索入口
stats/random_fen/            随机局面 FEN 数据
stats/data/                  性能分析结果
tests/test_core.c            基础规则测试
```

## 构建

推荐使用 CMake：

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

如果本机只有 GCC，也可以直接编译测试：

```sh
gcc -std=c99 -Wall -Wextra -Wpedantic -I include src/position.c src/movegen.c src/engine.c tests/test_core.c -o build/xiangqi_core_tests
./build/xiangqi_core_tests
```

编译交互式人机示例：

```sh
gcc -std=c99 -Wall -Wextra -Wpedantic -I include src/position.c src/movegen.c src/engine.c examples/cli.c -o build/xiangqi_cli
./build/xiangqi_cli
```

示例入口默认是“你执红，内置简易引擎执黑”。输入格式是 `起点+终点`，文件用 `a..i`，行号用 `0..9`，例如：

```text
b2b9
```

可用命令：

```text
moves  打印当前所有合法着法
fen    打印当前 FEN
help   打印帮助
quit   退出
```

## 随机局面生成与批量搜索

构建后可以先生成一组可复现的随机合法局面，再让内置引擎逐个搜索：

```sh
./build/xiangqi_generate_positions
./build/xiangqi_search_positions
```

两个程序默认使用 `stats/random_fen/random_positions.fen`，也可以为二者指定相同的文件路径：

```sh
./build/xiangqi_generate_positions build/profile_positions.fen
./build/xiangqi_search_positions build/profile_positions.fen
```

生成器使用固定随机种子，从初始局面随机行走 `0..100` 步，共输出 100 个局面，每行一个 FEN。
搜索程序顺序读取一次文件，以深度 4 为每个局面搜索一步，适合作为性能分析的非交互式入口。

## AI 接入

外部 AI 可以直接复用规则层：

```c
XqMoveList legal;
xq_generate_legal(&pos, &legal);

for (int i = 0; i < legal.count; ++i) {
    XqPosition next = pos;
    xq_position_make_move(&next, legal.moves[i]);
    /* 在 next 上做搜索或评估 */
}
```

也可以通过 `XqEngineAdapter` 接入自定义搜索：

```c
static bool my_search(const XqPosition *pos, unsigned depth, XqMove *best, void *user) {
    (void)user;
    /* 使用 xq_generate_legal / xq_position_make_move 实现自己的搜索 */
    return false;
}

XqEngineAdapter engine = {
    .evaluate = NULL,
    .search = my_search,
    .user = NULL,
};

XqMove best;
xq_engine_find_best_move(&engine, &pos, 4, &best);
```

## 坐标约定

- `file` 范围是 `0..8`，`rank` 范围是 `0..9`。
- 红方底线是 `rank = 0`，黑方底线是 `rank = 9`。
- `xq_square_make(file, rank)` 将坐标映射为 `rank * 9 + file`。
- FEN 按黑方到红方的 10 行书写，使用大写表示红方，小写表示黑方。
