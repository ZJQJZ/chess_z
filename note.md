地平线效应: `./build/xiangqi_explain --depth 6 --fen "r1ba1abnr/4k4/2n3c2/2p3p1p/p3C4/7C1/P1P1P1P1P/N8/7R1/R1BAKAB2 b - - 0 1"   `

黑方引擎会走 g7g3，因为六步之内无法绝杀，可以用 g7g3 后的炮挡住（刚好第六步吃这个炮）

1nbakabnr/9/rc7/4C1p1p/p4c3/4P2C1/P2p2P1P/4B4/N3A3R/3RKABN1 b - -

又是没看懂的一步

1nbakabnr/9/1c1R5/1r2C1pCp/p7c/4P4/P5P1P/4B4/N3AR3/4KABN1 b - -

极度长考

长将测试：
```
./build/xiangqi_cli -f "4kaR2/4a4/4R4/9/9/9/9/9/r8/5K3 b - -" -e b
```

### English

##### 1

形容词做后置定语

```
a form suitable for storage
```

##### 2

现在/过去分词作后置定语的例子，以及相应的定语从句的形式

```
The function converting the score is defined in engine.c.
->
The function that converts the score is defined in engine.c.

The score stored in the transposition table is normalized.
->
The score that is stored in the transposition table is normalized.
```

##### 3

现在/过去分词短语作状语的例子，以及相应的状语从句的形式
现在分词通常表示逻辑主语主动执行动作；过去分词通常表示逻辑主语承受动作或处于某种状态。

1. 时间状语

```text
Walking home, I met an old friend.
```

对应状语从句：

```text
While I was walking home, I met an old friend.
```

含义：我在回家的路上遇到了一位老朋友。

`I` 主动执行 `walk`，因此使用现在分词 `walking`。

2. 原因状语

```text
Knowing the answer, she raised her hand.
```

对应状语从句：

```text
Because she knew the answer, she raised her hand.
```

含义：因为她知道答案，所以举起了手。

3. 条件状语

```text
Following these instructions, you can avoid the error.
```

对应状语从句：

```text
If you follow these instructions, you can avoid the error.
```

含义：如果按照这些说明操作，你就能避免这个错误。

4. 让步状语

```text
Although knowing the risks, he continued the experiment.
```

对应状语从句：

```text
Although he knew the risks, he continued the experiment.
```

含义：尽管知道风险，他还是继续进行了实验。

表示让步时，通常保留 `although` 或 `while`，否则关系可能不够清楚。

5. 伴随状语

```text
She sat by the window, reading a book.
```

可以展开为：

```text
She sat by the window while she was reading a book.
```

含义：她坐在窗边看书。

这里两个动作同时发生，`reading a book` 描述主句动作的伴随情况。

6. 结果状语

```text
The server crashed, causing the service to become unavailable.
```

可以展开为结果从句：

```text
The server crashed, so that the service became unavailable.
```

或者更自然地写成并列句：

```text
The server crashed, and as a result, the service became unavailable.
```

含义：服务器崩溃了，导致服务不可用。

这里 `causing...` 的逻辑主语实际上是前面“服务器崩溃”这件事。

过去分词短语作状语

1. 时间状语

```text
Asked about the error, he remained silent.
```

对应状语从句：

```text
When he was asked about the error, he remained silent.
```

含义：当被问及这个错误时，他保持了沉默。

`he` 是 `ask` 的承受者，因此使用过去分词 `asked`。

2. 原因状语

```text
Encouraged by her teacher, she tried again.
```

对应状语从句：

```text
Because she was encouraged by her teacher, she tried again.
```

含义：由于受到老师的鼓励，她又试了一次。

3. 条件状语

```text
Used correctly, this function returns a normalized score.
```

对应状语从句：

```text
If this function is used correctly, it returns a normalized score.
```

含义：如果使用正确，该函数会返回归一化后的分数。

4. 让步状语

```text
Although defeated, the army did not surrender.
```

对应状语从句：

```text
Although the army was defeated, it did not surrender.
```

含义：尽管战败，这支军队仍未投降。

5. 状态或背景状语

```text
Surrounded by mountains, the village remained isolated.
```

对应状语从句：

```text
Because the village was surrounded by mountains, it remained isolated.
```

也可以根据语境理解为：

```text
While the village was surrounded by mountains, it remained isolated.
```

含义：这个村庄四面环山，长期与外界隔绝。

三、核心转换方式

现在分词通常对应主动从句：

```text
Because she knew the answer, she raised her hand.
→ Knowing the answer, she raised her hand.
```

过去分词通常对应被动从句：

```text
Because she was encouraged by her teacher, she tried again.
→ Encouraged by her teacher, she tried again.
```

转换时通常省略：

```text
连词 + 与主句相同的主语
```

然后：

```text
主动谓语 → 现在分词
被动谓语 → 过去分词
```

最重要的前提是，分词短语的逻辑主语通常必须与主句主语一致：

```text
Walking home, I met an old friend.
```

这里必须是 `I was walking home`，不能让分词在语法上错误地修饰其他事物。

##### 4

so 和 therefore

```
Non-mate scores do not depend on ply, so they remain unchanged.
Non-mate scores do not depend on ply and therefore remain unchanged.
```

```
./build/xiangqi_cli \
--search parallel \
--depth 100 \
--threads 16 \
--split-strategy interleaved \
--progress \
--fen "3akarr1/1NC2P2C/9/9/R8/9/6p2/4B4/2n1p4/5K3 r - -" \
--engine r  

./build/xiangqi_cli \
--search parallel \
--depth 100 \
--time-ms 5000 \
--threads 16 \
--split-strategy interleaved \
--progress \
```

```
zhangjiaqi@zhangjiaqideMacBook-Pro-2 chess_z % ./build/xiangqi_cli \
--search parallel \
--depth 100 \
--time-ms 5000 \
--threads 16 \
--split-strategy interleaved \
--progress \
--engine b
Search mode: parallel, iterative deepening to max depth 100, threads 16, split strategy interleaved, split 50% (ignored for interleaved); default time limit 5000 ms; in-memory cache enabled.
Xiangqi demo: human red vs parallel black engine
Use coordinates a-i and ranks 0-9. Example: b2b9
commands:
  a0a1 [time_ms]  move; optional positive integer time limit for the next engine reply
                  e.g. a0a1 2000 (2 seconds); omit time to use the startup default
  a0a1 a9a8       play your move and specify the engine reply, without searching
                  both moves must be legal; invalid input leaves the whole turn unchanged
                  exactly two moves; no third move or additional time argument
  fen             print current FEN
  moves           print legal moves
  undo            take back your last move and the automatic or specified engine reply (alias: u)
  flip            rotate the board display 180 degrees; move coordinates stay unchanged
  quit            exit
9 車 馬 象 士 将 士 象 馬 車 
8 .  .  .  .  .  .  .  .  .  
7 .  炮 .  .  .  .  .  炮 .  
6 卒 .  卒 .  卒 .  卒 .  卒 
5 .  .  .  .  .  .  .  .  .  
4 .  .  .  .  .  .  .  .  .  
3 兵 .  兵 .  兵 .  兵 .  兵 
2 .  炮 .  .  .  .  .  炮 .  
1 .  .  .  .  .  .  .  .  .  
0 車 馬 相 仕 帥 仕 相 馬 車 
  a  b  c  d  e  f  g  h  i
side: red
red to move > flip
Board view: black at bottom. Move coordinates are unchanged.
0 車 馬 相 仕 帥 仕 相 馬 車 
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  .  .  .  炮 .  
3 兵 .  兵 .  兵 .  兵 .  兵 
4 .  .  .  .  .  .  .  .  .  
5 .  .  .  .  .  .  .  .  .  
6 卒 .  卒 .  卒 .  卒 .  卒 
7 .  炮 .  .  .  .  .  炮 .  
8 .  .  .  .  .  .  .  .  .  
9 車 馬 象 士 将 士 象 馬 車 
  i  h  g  f  e  d  c  b  a
side: red
red to move > b2e2 h9g7
black engine plays (specified by player): h9g7
0 車 馬 相 仕 帥 仕 相 馬 車 
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  .  .  .  
3 兵 .  兵 .  兵 .  兵 .  兵 
4 .  .  .  .  .  .  .  .  .  
5 .  .  .  .  .  .  .  .  .  
6 卒 .  卒 .  卒 .  卒 .  卒 
7 .  炮 馬 .  .  .  .  炮 .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 馬 車 
  i  h  g  f  e  d  c  b  a
side: red
red to move > b0c2
0 車 馬 相 仕 帥 仕 相 .  車 
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  馬 .  .  
3 兵 .  兵 .  兵 .  兵 .  兵 
4 .  .  .  .  .  .  .  .  .  
5 .  .  .  .  .  .  .  .  .  
6 卒 .  卒 .  卒 .  卒 .  卒 
7 .  炮 馬 .  .  .  .  炮 .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 馬 車 
  i  h  g  f  e  d  c  b  a
side: black
search depth 9/100  [--------------------] 0%  roots 0/42  elapsed 5.0s / 5.0s; time limit / clock stop; selected depth 8
black engine plays: g6g5
0 車 馬 相 仕 帥 仕 相 .  車 
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  馬 .  .  
3 兵 .  兵 .  兵 .  兵 .  兵 
4 .  .  .  .  .  .  .  .  .  
5 .  .  卒 .  .  .  .  .  .  
6 卒 .  .  .  卒 .  卒 .  卒 
7 .  炮 馬 .  .  .  .  炮 .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 馬 車 
  i  h  g  f  e  d  c  b  a
side: red
red to move > a0b0
0 車 馬 相 仕 帥 仕 相 車 .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  馬 .  .  
3 兵 .  兵 .  兵 .  兵 .  兵 
4 .  .  .  .  .  .  .  .  .  
5 .  .  卒 .  .  .  .  .  .  
6 卒 .  .  .  卒 .  卒 .  卒 
7 .  炮 馬 .  .  .  .  炮 .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 馬 車 
  i  h  g  f  e  d  c  b  a
side: black
search depth 9/100  [#-------------------] 6%  roots 3/43  elapsed 5.0s / 5.0s; time limit / clock stop; selected depth 8
black engine plays: b7c7
0 車 馬 相 仕 帥 仕 相 車 .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  馬 .  .  
3 兵 .  兵 .  兵 .  兵 .  兵 
4 .  .  .  .  .  .  .  .  .  
5 .  .  卒 .  .  .  .  .  .  
6 卒 .  .  .  卒 .  卒 .  卒 
7 .  炮 馬 .  .  .  炮 .  .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 馬 車 
  i  h  g  f  e  d  c  b  a
side: red
red to move > h0g2 
0 車 .  相 仕 帥 仕 相 車 .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 馬 .  炮 .  馬 .  .  
3 兵 .  兵 .  兵 .  兵 .  兵 
4 .  .  .  .  .  .  .  .  .  
5 .  .  卒 .  .  .  .  .  .  
6 卒 .  .  .  卒 .  卒 .  卒 
7 .  炮 馬 .  .  .  炮 .  .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 馬 車 
  i  h  g  f  e  d  c  b  a
side: black
search depth 9/100  [--------------------] 0%  roots 0/36  elapsed 5.0s / 5.0s; time limit / clock stop; selected depth 8
black engine plays: c7c3
0 車 .  相 仕 帥 仕 相 車 .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 馬 .  炮 .  馬 .  .  
3 兵 .  兵 .  兵 .  炮 .  兵 
4 .  .  .  .  .  .  .  .  .  
5 .  .  卒 .  .  .  .  .  .  
6 卒 .  .  .  卒 .  卒 .  卒 
7 .  炮 馬 .  .  .  .  .  .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 馬 車 
  i  h  g  f  e  d  c  b  a
side: red
red to move > c0a2 b9b7
second item must be a legal four-character engine reply or positive time_ms; no moves played.
Use a0a1, a0a1 2000, or a0a1 a9a8; type moves for legal first moves.
0 車 .  相 仕 帥 仕 相 車 .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 馬 .  炮 .  馬 .  .  
3 兵 .  兵 .  兵 .  炮 .  兵 
4 .  .  .  .  .  .  .  .  .  
5 .  .  卒 .  .  .  .  .  .  
6 卒 .  .  .  卒 .  卒 .  卒 
7 .  炮 馬 .  .  .  .  .  .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 馬 車 
  i  h  g  f  e  d  c  b  a
side: red
red to move > c0a2 b9c7
black engine plays (specified by player): b9c7
0 車 .  相 仕 帥 仕 .  車 .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 馬 .  炮 .  馬 .  相 
3 兵 .  兵 .  兵 .  炮 .  兵 
4 .  .  .  .  .  .  .  .  .  
5 .  .  卒 .  .  .  .  .  .  
6 卒 .  .  .  卒 .  卒 .  卒 
7 .  炮 馬 .  .  .  馬 .  .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 .  車 
  i  h  g  f  e  d  c  b  a
side: red
red to move > b0b4
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 馬 .  炮 .  馬 .  相 
3 兵 .  兵 .  兵 .  炮 .  兵 
4 .  .  .  .  .  .  .  車 .  
5 .  .  卒 .  .  .  .  .  .  
6 卒 .  .  .  卒 .  卒 .  卒 
7 .  炮 馬 .  .  .  馬 .  .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 .  車 
  i  h  g  f  e  d  c  b  a
side: black
search depth 9/100  [--------------------] 0%  roots 0/37  elapsed 5.0s / 5.0s; time limit / clock stop; selected depth 8
black engine plays: c3g3
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 馬 .  炮 .  馬 .  相 
3 兵 .  炮 .  兵 .  .  .  兵 
4 .  .  .  .  .  .  .  車 .  
5 .  .  卒 .  .  .  .  .  .  
6 卒 .  .  .  卒 .  卒 .  卒 
7 .  炮 馬 .  .  .  馬 .  .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 .  車 
  i  h  g  f  e  d  c  b  a
side: red
red to move > c2d4
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 馬 .  炮 .  .  .  相 
3 兵 .  炮 .  兵 .  .  .  兵 
4 .  .  .  .  .  馬 .  車 .  
5 .  .  卒 .  .  .  .  .  .  
6 卒 .  .  .  卒 .  卒 .  卒 
7 .  炮 馬 .  .  .  馬 .  .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 .  車 
  i  h  g  f  e  d  c  b  a
side: black
search depth 8/100  [####################] 100%  roots 37/37  elapsed 5.4s / 5.0s; time limit / clock stop; selected depth 8
black engine plays: c6c5
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 馬 .  炮 .  .  .  相 
3 兵 .  炮 .  兵 .  .  .  兵 
4 .  .  .  .  .  馬 .  車 .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  卒 .  .  .  卒 
7 .  炮 馬 .  .  .  馬 .  .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 .  車 
  i  h  g  f  e  d  c  b  a
side: red
red to move > i3i43
first move is illegal or is not a four-character coordinate move; no moves played.
Use a0a1, a0a1 2000, or a0a1 a9a8; type moves for legal first moves.
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 馬 .  炮 .  .  .  相 
3 兵 .  炮 .  兵 .  .  .  兵 
4 .  .  .  .  .  馬 .  車 .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  卒 .  .  .  卒 
7 .  炮 馬 .  .  .  馬 .  .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 .  車 
  i  h  g  f  e  d  c  b  a
side: red
red to move > i3i4
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 馬 .  炮 .  .  .  相 
3 .  .  炮 .  兵 .  .  .  兵 
4 兵 .  .  .  .  馬 .  車 .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  卒 .  .  .  卒 
7 .  炮 馬 .  .  .  馬 .  .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 .  車 
  i  h  g  f  e  d  c  b  a
side: black
search depth 8/100  [####################] 100%  roots 40/40  elapsed 5.3s / 5.0s; time limit / clock stop; selected depth 8
black engine plays: g3a3
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 馬 .  炮 .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 .  .  .  .  馬 .  車 .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  卒 .  .  .  卒 
7 .  炮 馬 .  .  .  馬 .  .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 .  車 
  i  h  g  f  e  d  c  b  a
side: red
red to move > g2f4
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 .  .  馬 .  馬 .  車 .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  卒 .  .  .  卒 
7 .  炮 馬 .  .  .  馬 .  .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 .  車 
  i  h  g  f  e  d  c  b  a
side: black
search depth 9/100  [--------------------] 0%  roots 0/39  elapsed 5.0s / 5.0s; time limit / clock stop; selected depth 8
black engine plays: h7h4
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 .  馬 .  馬 .  車 .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  卒 .  .  .  卒 
7 .  .  馬 .  .  .  馬 .  .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 .  車 
  i  h  g  f  e  d  c  b  a
side: red
red to move > b4b7 6000
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 .  馬 .  馬 .  .  .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  卒 .  .  .  卒 
7 .  .  馬 .  .  .  馬 車 .  
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 .  車 
  i  h  g  f  e  d  c  b  a
side: black
search depth 10/100  [--------------------] 0%  roots 0/40  elapsed 6.0s / 6.0s; time limit / clock stop; selected depth 9
black engine plays: a9a7
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 .  馬 .  馬 .  .  .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  卒 .  .  .  卒 
7 .  .  馬 .  .  .  馬 車 車 
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 .  .  
  i  h  g  f  e  d  c  b  a
side: red
red to move > b7a7
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 .  馬 .  馬 .  .  .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  卒 .  .  .  卒 
7 .  .  馬 .  .  .  馬 .  車 
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 .  .  
  i  h  g  f  e  d  c  b  a
side: black
search depth 10/100  [--------------------] 0%  roots 0/39  elapsed 5.0s / 5.0s; time limit / clock stop; selected depth 9
black engine plays: a3a7
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  .  .  相 
3 .  .  .  .  兵 .  .  .  .  
4 兵 炮 .  馬 .  馬 .  .  .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  卒 .  .  .  卒 
7 .  .  馬 .  .  .  馬 .  炮 
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 .  .  
  i  h  g  f  e  d  c  b  a
side: red
red to move > u
Undid 2 move(s). Your turn again.
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 .  馬 .  馬 .  .  .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  卒 .  .  .  卒 
7 .  .  馬 .  .  .  馬 車 車 
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 .  .  
  i  h  g  f  e  d  c  b  a
side: red
red to move > b7a7 c7a7
second item must be a legal four-character engine reply or positive time_ms; no moves played.
Use a0a1, a0a1 2000, or a0a1 a9a8; type moves for legal first moves.
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 .  馬 .  馬 .  .  .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  卒 .  .  .  卒 
7 .  .  馬 .  .  .  馬 車 車 
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 象 .  .  
  i  h  g  f  e  d  c  b  a
side: red
red to move > b7a7 c9a7
black engine plays (specified by player): c9a7
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 .  馬 .  馬 .  .  .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  卒 .  .  .  卒 
7 .  .  馬 .  .  .  馬 .  象 
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: red
red to move > d4e6
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 .  馬 .  .  .  .  .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  馬 .  .  .  卒 
7 .  .  馬 .  .  .  馬 .  象 
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: black
search depth 10/100  [####################] 100%  roots 37/37  elapsed 5.1s / 5.0s; time limit / clock stop; selected depth 10
black engine plays: c7e6
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 .  馬 .  .  .  .  .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  馬 .  .  .  卒 
7 .  .  馬 .  .  .  .  .  象 
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: red
red to move > f4e6
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 .  .  .  .  .  .  .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  馬 .  .  .  卒 
7 .  .  馬 .  .  .  .  .  象 
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: black
search depth 10/100  [--------------------] 0%  roots 0/37  elapsed 5.0s / 5.0s; time limit / clock stop; selected depth 9
black engine plays: g7e6
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  炮 .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 .  .  .  .  .  .  .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  馬 .  .  .  卒 
7 .  .  .  .  .  .  .  .  象 
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: red
red to move > e2e6
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  .  .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 .  .  .  .  .  .  .  
5 .  .  卒 .  .  .  卒 .  .  
6 卒 .  .  .  炮 .  .  .  卒 
7 .  .  .  .  .  .  .  .  象 
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: black
search depth 10/100  [--------------------] 0%  roots 0/28  elapsed 5.0s / 5.0s; time limit / clock stop; selected depth 9
black engine plays: g5g4
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  .  .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 卒 .  .  .  .  .  .  
5 .  .  .  .  .  .  卒 .  .  
6 卒 .  .  .  炮 .  .  .  卒 
7 .  .  .  .  .  .  .  .  象 
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: red
red to move > i0u1
first move is illegal or is not a four-character coordinate move; no moves played.
Use a0a1, a0a1 2000, or a0a1 a9a8; type moves for legal first moves.
0 車 .  相 仕 帥 仕 .  .  .  
1 .  .  .  .  .  .  .  .  .  
2 .  炮 .  .  .  .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 卒 .  .  .  .  .  .  
5 .  .  .  .  .  .  卒 .  .  
6 卒 .  .  .  炮 .  .  .  卒 
7 .  .  .  .  .  .  .  .  象 
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: red
red to move > i0i1
0 .  .  相 仕 帥 仕 .  .  .  
1 車 .  .  .  .  .  .  .  .  
2 .  炮 .  .  .  .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 卒 .  .  .  .  .  .  
5 .  .  .  .  .  .  卒 .  .  
6 卒 .  .  .  炮 .  .  .  卒 
7 .  .  .  .  .  .  .  .  象 
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: black
search depth 9/100  [--------------------] 0%  roots 0/22  elapsed 5.0s / 5.0s; time limit / clock stop; selected depth 8
black engine plays: g4g3
0 .  .  相 仕 帥 仕 .  .  .  
1 車 .  .  .  .  .  .  .  .  
2 .  炮 .  .  .  .  .  .  相 
3 .  .  卒 .  兵 .  .  .  炮 
4 兵 炮 .  .  .  .  .  .  .  
5 .  .  .  .  .  .  卒 .  .  
6 卒 .  .  .  炮 .  .  .  卒 
7 .  .  .  .  .  .  .  .  象 
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: red
red to move > i1g1 10000
0 .  .  相 仕 帥 仕 .  .  .  
1 .  .  車 .  .  .  .  .  .  
2 .  炮 .  .  .  .  .  .  相 
3 .  .  卒 .  兵 .  .  .  炮 
4 兵 炮 .  .  .  .  .  .  .  
5 .  .  .  .  .  .  卒 .  .  
6 卒 .  .  .  炮 .  .  .  卒 
7 .  .  .  .  .  .  .  .  象 
8 .  .  .  .  .  .  .  .  .  
9 車 .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: black
search depth 9/100  [#####---------------] 26%  roots 8/30  elapsed 10.0s / 10.0s; time limit / clock stop; selected depth 8
black engine plays: i9i7
0 .  .  相 仕 帥 仕 .  .  .  
1 .  .  車 .  .  .  .  .  .  
2 .  炮 .  .  .  .  .  .  相 
3 .  .  卒 .  兵 .  .  .  炮 
4 兵 炮 .  .  .  .  .  .  .  
5 .  .  .  .  .  .  卒 .  .  
6 卒 .  .  .  炮 .  .  .  卒 
7 車 .  .  .  .  .  .  .  象 
8 .  .  .  .  .  .  .  .  .  
9 .  .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: red
red to move > g1f1
0 .  .  相 仕 帥 仕 .  .  .  
1 .  .  .  車 .  .  .  .  .  
2 .  炮 .  .  .  .  .  .  相 
3 .  .  卒 .  兵 .  .  .  炮 
4 兵 炮 .  .  .  .  .  .  .  
5 .  .  .  .  .  .  卒 .  .  
6 卒 .  .  .  炮 .  .  .  卒 
7 車 .  .  .  .  .  .  .  象 
8 .  .  .  .  .  .  .  .  .  
9 .  .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: black
search depth 9/100  [--------------------] 0%  roots 0/34  elapsed 5.0s / 5.0s; time limit / clock stop; selected depth 8
black engine plays: g3g2
0 .  .  相 仕 帥 仕 .  .  .  
1 .  .  .  車 .  .  .  .  .  
2 .  炮 卒 .  .  .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 .  .  .  .  .  .  .  
5 .  .  .  .  .  .  卒 .  .  
6 卒 .  .  .  炮 .  .  .  卒 
7 車 .  .  .  .  .  .  .  象 
8 .  .  .  .  .  .  .  .  .  
9 .  .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: red
red to move > h2h1
0 .  .  相 仕 帥 仕 .  .  .  
1 .  炮 .  車 .  .  .  .  .  
2 .  .  卒 .  .  .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 .  .  .  .  .  .  .  
5 .  .  .  .  .  .  卒 .  .  
6 卒 .  .  .  炮 .  .  .  卒 
7 車 .  .  .  .  .  .  .  象 
8 .  .  .  .  .  .  .  .  .  
9 .  .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: black
search depth 9/100  [--------------------] 0%  roots 0/35  elapsed 5.0s / 5.0s; time limit / clock stop; selected depth 8
black engine plays: i7d7
0 .  .  相 仕 帥 仕 .  .  .  
1 .  炮 .  車 .  .  .  .  .  
2 .  .  卒 .  .  .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 .  .  .  .  .  .  .  
5 .  .  .  .  .  .  卒 .  .  
6 卒 .  .  .  炮 .  .  .  卒 
7 .  .  .  .  .  車 .  .  象 
8 .  .  .  .  .  .  .  .  .  
9 .  .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: red
red to move > f1f4
0 .  .  相 仕 帥 仕 .  .  .  
1 .  炮 .  .  .  .  .  .  .  
2 .  .  卒 .  .  .  .  .  相 
3 .  .  .  .  兵 .  .  .  炮 
4 兵 炮 .  車 .  .  .  .  .  
5 .  .  .  .  .  .  卒 .  .  
6 卒 .  .  .  炮 .  .  .  卒 
7 .  .  .  .  .  車 .  .  象 
8 .  .  .  .  .  .  .  .  .  
9 .  .  象 士 将 士 .  .  .  
  i  h  g  f  e  d  c  b  a
side: black
search depth 8/100  [#####---------------] 25%  roots 9/36  elapsed 0.9s / 5.0szsh: bus error



./build/xiangqi_cli \
--search parallel \
--depth 100 \
--time-ms 5000 \
--threads 16 \
--split-strategy interleaved \
--progress \
--fen "3akab2/9/b7r/p3C3p/2p6/7cP/c3P1p2/B6C1/5R3/3AKAB2 b - -"
```