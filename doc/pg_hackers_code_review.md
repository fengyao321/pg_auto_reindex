# PostgreSQL Hackers 视角 Code Review 评估报告: `pg_auto_reindex`

> **审查者**: PostgreSQL Community Hacker Standard Review  
> **目标项目**: `pg_auto_reindex` (Autonomous idle learning & background concurrent reindexing)  
> **目标文件**: `pg_auto_reindex.c`, `reindex_executor.c`, `idle_learner.c`, `bloat_estimator.c`, `pg_auto_reindex.h`  
> **结论 (Verdict)**: **NACK (拒绝对接主干/需重大重构)** —— 当前存在致命事务控制错误、内存静默损坏隐患及内核 API 滥用。

---

## 1. 概要 (Executive Summary)

`pg_auto_reindex` 旨在通过后台 Background Worker 自动学习系统空闲时间段，并对膨胀的 B-Tree 索引执行无感知的 `REINDEX CONCURRENTLY`。

然而，在对 C 源码进行深入审查后，发现该扩展在 **PostgreSQL 内核事务机制、内存安全、并发锁语义以及内核 C API 的使用** 上存在多处致命缺陷。在当前代码下，扩展在尝试运行重索引时会 **100% 触发引擎事务异常崩溃**，且存在静默内存损坏危险。

$$\text{Net Value} = \text{Benefit} - (\text{Crash Risk} + \text{Memory Corruption} + \text{Lock Contention})$$

当前评估结果显示，项目的 $\text{Net Value}$ 严重为负，无法在任何生产环境下安全运行。

---

## 2. 详细缺陷拆解 (Detailed Issue Breakdown)

### 2.1 致命缺陷：在 SPI 事务上下文中非法执行 `REINDEX CONCURRENTLY`

- **涉及文件**: [`reindex_executor.c`](file:///home/fengyao/code/pg_auto_reindex/reindex_executor.c#L261-L295)
- **代码位置**:
  ```c
  if (!IsTransactionState())
  {
      SetCurrentStatementStartTimestamp();
      StartTransactionCommand();
      started_reindex_xact = true;
  }

  SPI_connect();
  PushActiveSnapshot(GetTransactionSnapshot());

  PG_TRY();
  {
      SPI_exec(set_lock_timeout, 0);
      ret = SPI_exec(reindex_cmd, 0); /* reindex_cmd = "REINDEX INDEX CONCURRENTLY ..." */
  }
  ```

#### 原理解析与灾难后果
PostgreSQL 核心引擎（在 `src/backend/commands/indexcmds.c` 中）强制规定：**`REINDEX CONCURRENTLY` 与 `DROP INDEX CONCURRENTLY` 必须运行在非事务块（Non-transaction block）环境下**。

`SPI_connect()` 与 `StartTransactionCommand()` 强制将当前执行置于活跃事务 Block 内。当 `SPI_exec()` 试图执行 `REINDEX INDEX CONCURRENTLY` 时，内核会直接抛出 `ERRCODE_ACTIVE_SQL_TRANSACTION` 错误（`ERROR: REINDEX CONCURRENTLY cannot run inside a transaction block`）。

**后果**：Background Worker 在触发重索引时会 **100% 报错退出**，导致 Worker 进程陷入死循环重启。`CleanupInvalidIndexes()` 中的 `DROP INDEX CONCURRENTLY` 亦存在完全相同的问题。

#### 修复建议
不能使用 SPI 来驱动并发 DDL。Background Worker 必须脱离 SPI 事务包装，直接调用内核 C 级别的 `reindex_index()` API，或者通过 Portal / `exec_simple_query` 流驱动。

---

### 2.2 内存损坏风险：GUC 变量类型与指针不匹配 (Undefined Behavior)

- **涉及文件**: [`pg_auto_reindex.c`](file:///home/fengyao/code/pg_auto_reindex/pg_auto_reindex.c#L47-L118)
- **代码位置**:
  ```c
  int64 guc_min_bloat_bytes = 67108864; /* 声明为 8 字节 int64 */

  DefineCustomIntVariable("pg_auto_reindex.min_bloat_bytes",
                          "Minimum index size in bytes to trigger reindex",
                          NULL, (int *) &guc_min_bloat_bytes, 1048576, 0, INT_MAX,
                          PGC_SIGHUP, GUC_UNIT_BYTE, NULL, NULL, NULL);
  ```

#### 原理解析与灾难后果
在 64 位 Linux/x86_64 平台上，`int64` 占 8 字节，而 `DefineCustomIntVariable` 要求的参数类型为 `int *`（4 字节）。

代码中强行将 `int64 *` 强转为 `int *` 传入。在小端序（Little-Endian）系统上，当用户通过 `SET` 命令或 SIGHUP 重载配置时，GUC 框架只会覆盖低 4 字节，导致 `guc_min_bloat_bytes` 高 4 字节保留未初始化的随机内存垃圾，或者在改写时破坏相连接的 Shared Memory 内存结构。

#### 修复建议
在 PostgreSQL 15+ 中使用 `DefineCustomInt64Variable()` 来定义字节数大整数 GUC，或将 `guc_min_bloat_bytes` 的变量类型严格修改为 `int`。

---

### 2.3 性能与架构缺陷：高频采样中滥用 SPI 扫描 Catalog 视图

- **涉及文件**: [`idle_learner.c`](file:///home/fengyao/code/pg_auto_reindex/idle_learner.c#L65-L73)
- **代码位置**:
  ```c
  ret = SPI_exec("SELECT count(*) FROM pg_stat_activity "
                 "WHERE state = 'active' AND pid != pg_backend_pid();", 0);
  ```

#### 原理解析与灾难后果
Worker 默认每 60 秒轮询一次系统指标。为了获取当前活跃 Backend 数量，该扩展每次都启动事务、建立 SPI 连接、解析 SQL，并扫描 `pg_stat_activity` 视图。

`pg_stat_activity` 内部需要多次获取 `BackendStatusArray` 的 Lock 并完成大量的类型转换。在 C 语言扩展的 Background Worker 中用 SPI 执行 SQL 查询系统状态，属于典型的 **Gratuitous Abstraction（无意义的抽象过载）**。

#### 修复建议
Background Worker 与主 Backend 进程共享内存空间，应直接遍历共享内存中的 `BackendStatusArray` 或调用内核 API（如 `pgstat_fetch_stat_beentry()`），实现 $\mathcal{O}(N)$ 零 SQL 额外开销的内存采样。

---

### 2.4 算法与逻辑缺陷：粗暴且不可靠的 B-Tree 膨胀率估算公式

- **涉及文件**: [`bloat_estimator.c`](file:///home/fengyao/code/pg_auto_reindex/bloat_estimator.c#L25-L53)
- **代码位置**:
  ```sql
  ((c.relpages - GREATEST(CEIL(c.reltuples * (COALESCE(s.avg_width, 8) + 8) / (8192 - 64)), 1)) / c.relpages::numeric)
  ```

#### 原理解析与灾难后果
1. **表达式索引（Expression Index）失效**: 表达式索引在 `pg_index.indkey` 中属性编号记录为 `0`。SQL 中的 `s.staattnum = ANY(i.indkey)` 无法匹配 `pg_statistic`，导致 `s.avg_width` 缺失退化为默认值 `8`，计算结果严重失真。
2. **忽略 B-Tree 页物理布局**: B-Tree 页面必须扣除 `PageHeaderData` (24 bytes)、`BTPageOpaqueData` (16 bytes)、Line Pointer Array (`ItemIdData` 4 bytes/tuple) 以及 B-Tree 叶子页默认 `fillfactor=90` 的空间。使用 `(8192 - 64)` 分母计算，会导致正常健康的索引被误判为 15%+ 膨胀。
3. **`pg_statistic` 滞后风险**: 未考虑 `reltuples` 滞后情况。若表经历了批量删除但未执行 `ANALYZE`，公式可能计算出负数膨胀率或偏离事实的巨大值。

---

### 2.5 竞态条件与隐患：脆弱且具破坏性的无效索引清理逻辑

- **涉及文件**: [`reindex_executor.c`](file:///home/fengyao/code/pg_auto_reindex/reindex_executor.c#L91-L97)
- **代码位置**:
  ```sql
  WHERE i.indisvalid = false AND c.relname LIKE '%_ccnew'
  ```

#### 原理解析与灾难后果
硬编码匹配字符串 `'%_ccnew'` 极其脆弱。如果线上环境中有 DBA 或其他工具（如 `pg_repack`）正在并发执行 `REINDEX CONCURRENTLY`，其产生的临时索引也会被此处的 Worker 误识别为“遗留废弃索引”并强制 DROP，引发严重的锁争用或运维事故。

---

## 3. 重构路线图 (Refactoring Roadmap)

1. **[P0] 修复 DDL 执行方式**: 废除 SPI 方式执行 `REINDEX CONCURRENTLY`，改用内部无事务 Command 流或内核 `reindex_index()` C API。
2. **[P0] 修复 GUC 指针类型**: 将 `guc_min_bloat_bytes` 映射更新为 `DefineCustomInt64Variable()`。
3. **[P1] 优化空闲采样**: 废除 `pg_stat_activity` SPI 查询，改为直接读取 `BackendStatusArray`。
4. **[P1] 完善膨胀计算**: 完善对 Multi-column index、Expression index 的字节计算支持，考虑 B-Tree 叶子页结构开销。
5. **[L2] 安全废弃索引清理**: 增加对并发锁状态和事务属主的校验，避免误杀正常进行的运维任务。
