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