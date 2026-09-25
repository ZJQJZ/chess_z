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

启动时可用 `--time-ms` 设置整局默认的每步思考时间（正整数，单位毫秒），例如 2 秒：

```sh
./build/xiangqi_cli --time-ms 2000
./build/xiangqi_cli --engine red --time-ms 2000 --search-detail
```

不传此参数时使用引擎默认的 3000 毫秒。引擎先手时的第一步也使用该设置。

轮到玩家时，可以在走法后附加引擎下一步的思考时间（正整数，单位毫秒）：

```text
b2b9 2000
```

这表示走出 `b2b9` 后，引擎下一步最多思考约 2 秒；搜索也可能因达到深度限制等原因提前结束。
只输入走法（如 `b2b9`）则使用 `--time-ms` 指定的时间，未指定时为 3000 毫秒；深度限制仍为 10 层。
时间设置仅对紧接着的一次引擎搜索生效，下一次省略时间时恢复默认值。
时间为零、负数、小数、溢出或带有多余参数的输入会被拒绝，棋盘保持不变。
引擎先手时的第一步仍使用默认限制。整局复用同一张置换表，修改思考时间不会清空或重建它。

对局中输入 `undo`（或 `u`）可以悔棋：撤回玩家上一手以及引擎随后的一手，回到玩家走棋前。
可以连续输入以撤回多个回合；尚未走过玩家走法时会提示无法悔棋，引擎的开局首步不会单独撤回。
分出胜负后 CLI 保留输入提示，可以输入 `undo` 继续对局或 `quit` 退出；
如果玩家最后一手已获胜、引擎尚未回应，则只撤回玩家这一手。
悔棋会同步恢复棋盘和走棋历史，后续搜索仍复用原置换表。
上一手的临时时间限制不保留，重新走棋时可以再次指定；省略时仍使用启动时的默认时间。
开启 `--search-detail` 时，悔棋会追加 `--- undo ---` 记录（撤回步数和恢复后的 FEN），已有搜索记录保留。

输入 `flip` 可以将棋盘显示旋转 180 度，再次输入恢复原方向。
默认红方在下；反转后黑方在下，行号从上到下为 `0` 到 `9`，列标从左到右为 `i` 到 `a`。
反转仅影响显示，不修改棋盘存储、当前走棋方、走棋历史、FEN 或置换表。
走法仍按棋盘上标注的原始坐标输入，例如 `b2b9` 的含义不变。
显示方向会保持到再次输入 `flip`，走棋和悔棋不会重置它；对局结束后的输入提示中也可使用。

开启每次引擎走棋的搜索摘要：

```sh
./build/xiangqi_cli --search-detail
./build/xiangqi_cli --engine red --search-detail
```

默认不记录日志。开启后，向**当前工作目录**下的 `build/search_detail.txt` 追加文本；
缺少 `build` 目录时自动创建。每次启动写入时间、初始 FEN 和引擎执棋方，
每次引擎搜索结束后立即写入并刷新一份摘要。旧对局保留，人类走棋不会触发额外搜索。
文件创建或写入失败会输出警告并关闭本次运行的日志功能，对局继续。

每份摘要包含回合编号、走棋前 FEN、执棋方、搜索成功状态、最终走法，以及以下字段：

| 字段 | 含义 |
| --- | --- |
| `depth_limit` / `time_limit_ms` / `time_mode` / `depth_bonus` | 本次搜索配置，默认 10 层、3000 毫秒、单调墙钟、深度奖励 70；可通过 `--time-ms` 设置默认时间，输入走法时可指定下一步的时间限制 |
| `max_started_depth` | 实际开始搜索至少一个根走法的最大迭代深度 |
| `completed_depth` | 全部根候选走法均已完成的最大迭代深度 |
| `selected_move_depth` | 最终选中走法的已完成深度；未经搜索的回退走法为 0 |
| `nodes` | 普通搜索和静态搜索函数的进入次数，普通搜索叶节点转入静态搜索时各计一次 |
| `elapsed_ms` | 整次调用的单调墙钟耗时，读取失败显示 `unavailable` |
| `stopped` | 是否因时间限制或搜索计时错误提前中断 |
| `stats_available` / `cache_available` | 内部搜索统计／置换表统计是否可用 |
| `cache_probes` / `cache_hits` / `cache_hit_rate` | 本次置换表查询数、命中数和命中率；查询为零时为 `0.00%` |
| `cache_cutoffs` | 缓存分数直接复用或上下界导致剪枝的次数 |
| `cache_stores` / `cache_replacements` | 本次缓存写入和替换次数 |

三个实际深度均以半回合（ply）为单位，不包含静态搜索延伸；未开始搜索时为 0。
超时可能保留当前未完成迭代中已经完成的候选结果，因此所选走法深度可能高于完整完成深度。
缓存统计取本次调用前后的计数差，不清空缓存和累计计数。命中包含仅用于走法排序的命中，
不等同于可以直接复用缓存分数；无缓存时计数为零并标记 `cache_available: no`。

库调用方可使用 `xq_engine_find_best_move_with_stats(..., XqSearchStats *stats)` 获取相同统计；
`stats` 可以为 `NULL`，原有 `xq_engine_find_best_move` 接口保持兼容。
自定义搜索回调仅提供调用耗时，其内部深度、节点和缓存统计标记不可用。
安装 Python 3 时，CMake 会额外注册 CLI 日志集成测试；也可直接运行
`python3 tests/test_cli.py ./build/xiangqi_cli`。

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
搜索程序顺序读取一次文件，以最大深度 6、不限时的配置为每个局面搜索一步，适合作为性能分析的非交互式入口。

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
XqSearchLimits limits = xq_search_limits_default();
xq_engine_find_best_move(&engine, &pos, &limits, &best);
```

## 坐标约定

- `file` 范围是 `0..8`，`rank` 范围是 `0..9`。
- 红方底线是 `rank = 0`，黑方底线是 `rank = 9`。
- `xq_square_make(file, rank)` 将坐标映射为 `rank * 9 + file`。
- FEN 按黑方到红方的 10 行书写，使用大写表示红方，小写表示黑方。
