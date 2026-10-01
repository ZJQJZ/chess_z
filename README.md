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

启动时可用 `--depth N` 设置整局最大搜索深度，默认 10 层，每层为一方走一步（ply）：

```sh
./build/xiangqi_cli --depth 12 --time-ms 5000 --search-detail
```

`N` 必须是 1 至 `INT_MAX` 的整数；缺少参数、零、负数、小数、溢出或多余非空白字符会被拒绝。
该设置也用于引擎先手的第一步，悔棋后继续保留；走法后附加时间不会改变深度上限。
搜索采用迭代加深，实际达到的深度受时间预算影响；获准超时延长也只完成当前层，不超过深度上限。

轮到玩家时，可以在走法后附加引擎下一步的思考时间（正整数，单位毫秒）：

```text
b2b9 2000
```

这表示走出 `b2b9` 后，引擎下一步最多思考约 2 秒；搜索也可能因达到深度限制等原因提前结束。
只输入走法（如 `b2b9`）则使用 `--time-ms` 指定的时间，未指定时为 3000 毫秒；深度限制使用 `--depth` 设置，默认为 10 层。
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

在玩家输入提示或对局结束后的提示处，输入 `save-tt PATH` 可以保存当前置换表：

```text
save-tt build/cache.tt
save-tt "build/my cache.tt"
```

路径必须指定；命令名后的全部内容作为路径，可以包含空格，也支持外层单引号或双引号。
交互输入不做 shell 变量、`~` 或转义展开；相对路径从当前工作目录解析。
整行最多 4095 字节，超长输入会完整丢弃，不会使用截断的文件名。
保存成功后替换已有文件，不自动创建父目录。写入采用同目录唯一临时文件，完整写入并成功关闭后再替换，
失败时保留旧文件并继续对局。保存不改变内存中的置换表、统计、棋盘和搜索时间。

下次启动时通过 `--tt-file PATH` 加载：

```sh
./build/xiangqi_cli --tt-file build/cache.tt --time-ms 5000
./build/xiangqi_cli --engine red --tt-file "build/my cache.tt"
```

加载在第一次搜索前完成，包括引擎先手的情况；之后继续复用和更新这张表。
文件不存在、损坏、版本不兼容或内存不足时会报错退出，不会悄悄改用空缓存。
不传 `--tt-file` 时沿用原行为。程序不会自动保存，也不提供运行时加载命令。
**保存的是搜索缓存，不是对局存档**：不保存或恢复棋盘、FEN、走棋历史、显示方向或时间设置。
初始局面仍由标准开局或 `--fen` 决定；加载后走棋、悔棋和翻转显示仍复用该表。

缓存使用小端二进制格式，包含格式版本、引擎缓存兼容版本、表尺寸、代数和有效条目，
并使用 IEEE CRC32 校验完整性。加载保留条目原槽位、深度、分数类型和代数，统计从零开始。
空表也可以保存和加载；加载会先验证临时存储中的全部数据，失败不改变原内存表。
`engine.c` 中的 `XQ_TT_FORMAT_VERSION` 管理文件布局，`XQ_TT_ENGINE_VERSION` 管理搜索缓存语义。
修改评估、搜索分数语义、棋子编码或 Zobrist 哈希时必须升级引擎缓存版本，旧版本文件将被拒绝。
版本检查不会自动识别未升级版本号的代码修改；该格式仅用于内置引擎，不应混用自定义评估或搜索的缓存。

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
| `depth_limit` / `time_limit_ms` / `time_mode` | 本次搜索配置，默认 10 层、3000 毫秒、单调墙钟；可通过 `--depth` 设置最大深度、`--time-ms` 设置默认时间，输入走法时可指定下一步的时间限制 |
| `max_started_depth` | 实际开始搜索至少一个根走法的最大迭代深度 |
| `iteration_progress` | `max_started_depth` 所属轮次已完成的根候选数量／过滤后的候选总数及百分比，例如 `12/44 (27.3%)`；从未开始搜索时为 `not-started (0/N)` |
| `completed_depth` | 全部根候选走法均已完成的最大迭代深度 |
| `selected_move_depth` | 最终选棋依据所属的搜索深度；超时回退时对应上一完整轮，未经搜索的兜底走法为 0 |
| `nodes` | 普通搜索和静态搜索函数的进入次数，普通搜索叶节点转入静态搜索时各计一次 |
| `elapsed_ms` | 整次调用的单调墙钟耗时，读取失败显示 `unavailable` |
| `stopped` | 是否检测到时间限制或搜索计时错误；获准延长并完成当前轮时也为 `yes` |
| `stats_available` / `cache_available` | 内部搜索统计／置换表统计是否可用 |
| `cache_probes` / `cache_hits` / `cache_hit_rate` | 本次置换表查询数、命中数和命中率；查询为零时为 `0.00%` |
| `cache_cutoffs` | 缓存分数直接复用或上下界导致剪枝的次数 |
| `cache_stores` / `cache_replacements` | 本次缓存写入和替换次数 |

搜索进度在每步搜索结束后写入摘要，按根候选数量计算，不代表耗时或搜索节点的完成比例。
中途被打断的候选不计入完成数；完整轮次显示 `N/N (100.0%)`。超时回退到上一完整轮选棋时，
进度仍保留最深已开始轮次的实际完成情况；若下一轮尚未开始就超时，则保留上一轮的完整进度。
没有合法候选或候选全部被历史循环过滤时显示 `not-started (0/0)`，不计算百分比。

三个实际深度均以半回合（ply）为单位，不包含静态搜索延伸；未开始搜索时为 0。
首次检测到时间限制时，若当前轮已完成根候选比例**严格超过 70%**，允许超出原时间预算，
继续完成当前轮并采用其结果，然后退出，不再进入下一层。例如 `29/42` 停止，`30/42` 继续；
恰好 `70%` 仍停止。阈值由 `src/engine.c` 中的 `XQ_FINISH_ITERATION_THRESHOLD_PERCENT` 常量控制。
剩余候选的耗时不均，额外用时不设上限；延长后 `elapsed_ms` 可超过预算，`stopped` 为 `yes`，
成功完成时进度为 `100.0%`，完成深度和所选走法深度均对应该轮。计时失败始终立即停止。
未获准延长或延长期间计时失败导致本轮中断时，选棋使用上一完整轮结果；仅在第 1 轮尚未
完成时使用该轮已完成候选，此时所选走法深度为 1、完整完成深度为 0。
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

## 搜索路径解释

`xiangqi_explain` 默认从初始局面搜索 4 层并进入交互浏览，支持 `--fen FEN`
和 `--depth N`。设置 `--time-ms`、`--moves` 或 `--tt-file` 后，只输出一次目标节点的信息并退出：

```sh
# 从根局面迭代加深，总搜索预算为 2 秒；默认最大深度与 CLI 相同，为 10。
./build/xiangqi_explain --time-ms 2000

# 载入置换表，单次解释根节点；也可组合时间预算和目标路径。
./build/xiangqi_explain --tt-file build/cache.tt
./build/xiangqi_explain --tt-file build/cache.tt --time-ms 2000 --moves a3a4 a6a5

# 从初始局面迭代加深至最多 4 层，查看路径 a3a4 a6a5；默认预算 3 秒。
./build/xiangqi_explain --depth 4 --moves a3a4 a6a5

# 也支持带引号的走法序列、参数换序和自定义根局面。
./build/xiangqi_explain --moves "e0d0 e5e4" --depth 4 --time-ms 2000 \
  --fen "4k4/9/9/9/4p4/9/9/9/9/4K4 r - -"
```

`--moves` 指定从根开始的完整搜索路径；程序在副本上检查每步是否合法，再从原始
根局面搜索。路径长度会消耗根搜索的深度：根深度为 4、路径长度为 2 时，目标节点
剩余普通深度为 2。只记录剩余普通深度至少为 1 的目标节点；到达深度 0 或进入
静态搜索的路径不生成解释记录。迭代加深时，从根深度大于路径长度的轮次起才可能记录目标。
重复的 `--moves` 按出现顺序追加；每段序列读取到下一个 `--` 选项。

单次模式的搜索规则与 CLI 一致，默认最大深度 **10**、时间预算 **3000 毫秒**。
`--time-ms` 必须为正整数毫秒，指定整个根搜索的单调墙钟预算；
路径校验、加载缓存和结果输出不计入预算。所有搜索都从 1 层迭代加深，同时设置
`--depth` 与 `--time-ms` 时，以先到的限制为准，但到时限时进度严格超过 70% 可完成当前层后退出。
库调用可设置 `time_limit_ms=0` 来禁用
截止时间，仍逐层迭代到 `max_depth`。单次模式拒绝 `--depth 0`；原有交互模式默认 4 层，
仍支持 `--depth 0` 和静态搜索浏览。

解释搜索不再采用独立的根缓存返回、根排序或额外计时规则。根驱动在每个根着法搜索
前后检查时间，普通递归与静态搜索沿用每 1024 节点检查；解释记录不增加时钟检查。
相同根局面、限制、历史、回调和初始置换表会使用相同的搜索规则，但记录信息本身有开销，
两次独立的墙钟限时运行仍可能在不同节点停止，因此不保证每次 6 秒搜索的落子完全相同。

`root_selection` 是本次根搜索最终选中的着法，与 CLI 的选棋策略一致。
完整迭代结束时按根分数选择；到时限时满足上述阈值则完成当前轮并采用其结果。若当前轮
因超时或计时失败被中断，则返回上一完整轮的最佳着法，候选表也恢复
为该完整轮的快照，不混用不同深度的评分。若第 1 轮尚未完成，选择该轮已完成候选中的
最佳着法；没有任何候选完成时使用与 CLI 相同的初始兜底着法。同分保留搜索顺序中最先
达到该分数的着法。若最后一个候选完成后才检测到超时，仍采用刚完成的整轮结果。

`policy` 为 `completed-iteration`（完整轮结果）、`timeout-completed-iteration`（较深轮中断后
回退）、`partial-first-iteration`（首轮部分结果）或 `fallback`（未经搜索的兜底）。
超时选棋表包含 `score`、`completed_depth` 和 `selected`，展示实际用于最终选择的快照。
分数是递归返回值，可能是边界；有效分数属于同一轮，未完成候选显示 `-`，不把占位值
当作有效分数。`selected_move_depth` 对应该快照中选中着法的深度。

输出的 `root_depth` 是目标记录所属的根迭代深度，`remaining_depth` 是目标节点剩余
普通深度；`alpha`、`beta` 和候选表来自实际根搜索，分数以目标节点行棋方为视角。
候选表的编号相对于目标节点；`best` 是该次访问的最佳候选行号，**不是最终根选棋结果**。
尤其超时时，保留的完整目标记录与最终选择可能使用不同轮次的数据。
`exact`、`upper`、`lower` 分别表示精确值、上界和下界。
`complete=yes` 包含正常剪枝或直接复用缓存完成的节点，并不意味着遍历了其所有候选。

程序保留目标节点最近一次**已完成**的搜索记录，即使所属的根迭代尚未完成。
`completed_root_depth` 和 `stopped` 描述整个根搜索；它们可能与目标记录的轮次不同。
若目标从未完成，则展示最近一次访问的部分记录（`complete=no`、`partial_score`）；
被中断候选的占位返回值不会进入表格。若路径因剪枝、深度或超时而未被访问，输出
`path not visited with remaining depth >= 1 in this search`，正常退出且不额外搜索该局面。

库接口 `xq_engine_explain_path()` 使用独立的 `builtin_search_for_explain()` 根驱动，
按 `builtin_search()` 的规则执行搜索，复用相同的排序、计时、置换表及超时选棋辅助函数。
它接受适配器提供的评估、排序、置换表和历史信息，不调用自定义搜索回调。解释状态和
记录逻辑集中在独立上下文、`builtin_search_for_explain()`、`negamax_for_explain()` 中；
普通 `builtin_search()`、`negamax()`、`quiescence()` 和 `SearchContext` 不依赖解释记录。
两条命令从同一 FEN 启动时没有过去对局的历史；如要重现进行中的 CLI 对局，
库调用方还需提供相同历史。原有逐层解释接口和交互导航保持兼容。

`--tt-file PATH` 使用与 CLI 相同的缓存格式和兼容性检查；加载失败报错退出，不回退。
没有指定文件时，与 CLI 一样创建一张空置换表。搜索正常更新内存中的条目、代次及替换
信息，输入文件不会被改写。

根节点只在迭代前查询一次缓存着法用于排序，**不会直接复用根缓存分数**。
后续根迭代按上一轮分数排序，不再查询根缓存。`root_ordering_tt` 保留首次排序查询；
根目标记录在第一轮后的 `target_tt=not-probed` 表示该轮没有重新查询，而不是没有启用缓存。
内部目标节点仍允许按深度和分数边界直接返回，显示
`returned directly from transposition table; no candidates expanded`，缓存着法在 `target_tt`
中单独展示，此时 `best=0` 表示没有候选行。终局根节点在开始迭代前返回，`root_depth=0`，
没有最终着法。

新增缓存状态如下。它们描述节点入口处的实际查询，不是搜索结束后重新查询的结果。

| 状态 | 含义 |
| --- | --- |
| `disabled` | 没有提供置换表 |
| `not-probed` | 未查询，例如深度为 0、查询前判定终局，或第二轮及之后的根节点 |
| `miss` | 查询没有匹配条目 |
| `hit` | 找到条目但未直接返回；可能是深度/窗口不允许截断，或根节点只查询排序着法 |
| `direct` | 此节点的返回分数直接来自缓存 |

`target_tt` 显示目标节点的查询；候选表的 `child_tt`、`tt_depth`、`tt_score`、`tt_kind`、
`tt_best`、`hash_used` 显示直接子节点的查询。`tt_score` 和 `tt_kind` 以**子节点行棋方**
为视角，原有 `score` 和 `kind` 以**目标节点行棋方**为视角。缓存分数已按查询节点的 ply
还原将死距离。`hash_used=yes` 表示缓存着法通过候选匹配并用于排序，之后 PV 提示仍可能
优先；它不表示直接返回。`reason=ordering-only` 表示根节点仅查询排序提示，
不根据缓存深度或分数决定返回。子树深处命中缓存不会把直接子节点标成 `direct`。

若严格祖先通过缓存直接返回，会显示 `latest_ancestor_cache_block`、祖先路径、当时的
窗口和剩余深度，以及 `ancestor_tt` 条目信息。这条事件与保留的目标记录各自标明
`root_depth`：它只说明那一轮在该祖先停止展开，不代表每一轮或最后一轮都因此未访问目标。
目标本身直接命中缓存算作已访问，不算祖先阻断。没有实际观察到缓存阻断时，不会把
未访问原因推断为缓存。程序不会为了补出解释而绕过缓存或沿缓存 PV 伪造访问记录。

`cache probes/hits/cutoffs/stores/replacements` 是本次调用的累计计数增量，不清零表中
原有统计。条目可能来自输入文件，也可能来自本次搜索的较早分支或迭代；这里不区分来源。

新测试可通过 CTest 运行，命令行测试也可直接运行：

```sh
python3 tests/test_explain_cli.py ./build/xiangqi_explain
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
