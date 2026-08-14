# `pg_auto_reindex` 架构重构与设计规范 (Architectural Redesign Specification)

> **版本**: v2.0-Redesign  
> **设计目标**: 遵循 PostgreSQL 社区核心标准，构建高可靠、零 Crash 风险、策略与机制解耦的自动化索引重构扩展方案。

---

## 1. 架构核心原则 (Core Architectural Principles)

1. **机制在内核，策略在外部 (Mechanism in Extension, Policy Outside)**
   - **内核扩展 (C Extension)**：专注于提供高精度、低开销的物理页/元数据膨胀评估算法，提供安全无阻塞的 C 级执行接口与共享内存状态暴露。
   - **控制平面 (Control Plane Daemon)**：负责空闲时间学习、多指标阈值监控、多数据库调度、紧急熔断与告警通知。
2. **主库零崩溃风险 (Zero-Crash Guarantee)**
   - 彻底解耦控制逻辑与 Postgres 主进程空间，即使控制面 Daemon 崩溃，PostgreSQL 主库不受任何影响。
   - C 扩展内部严格遵循 Defensive Programming，绝对禁止在 SPI 事务块内运行 `REINDEX CONCURRENTLY`。
3. **数据持久化与高可用感知 (Persistence & Failover Awareness)**
   - 学习模型与运维历史记录存储于标准数据库表或外部 Observability 存储中，跨 Postmaster 重启与主备切换（Failover）无缝继承。
4. **全实例与多数据库支持 (Multi-Database Native)**
   - 天然支持单个 PostgreSQL 实例下的多 Database 膨胀扫描与分布式调度。

---

## 2. 整体系统架构图 (System Architecture Diagram)

```text
┌─────────────────────────────────────────────────────────────────────────┐
│                    外部控制平面 (External Control Plane)                 │
│         (pg_auto_reindex_daemon in Go/Python 或 pg_cron Runner)          │
│                                                                         │
│  ┌───────────────────────┐  ┌───────────────────────┐  ┌─────────────┐  │
│  │ 168h EWMA 空闲学习模型 │  │ cgroups/I/O 资源监控  │  │ 调度与熔断  │  │
│  └───────────┬───────────┘  └───────────┬───────────┘  └──────┬──────┘  │
└──────────────┼──────────────────────────┼─────────────────────┼─────────┘
               │                          │                     │
               └──────────────────────────┼─────────────────────┘
                                          │ 标准 SQL / libpq 连接
                                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                     PostgreSQL 内核扩展 (C Extension)                   │
│                                                                         │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │ 1. 估算层 (Bloat Estimator Engine)                                 │  │
│  │    - 精确考虑 B-Tree 叶子页 Header、Padding、Fillfactor、Expression  │  │
│  │    - C 函数: pg_auto_reindex_bloat_check(relation regclass)       │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │ 2. 执行层 (Safe Execution Engine)                                  │  │
│  │    - 独立脱离 SPI 事务块的安全并发重索引接口                        │  │
│  │    - 长事务感知与 lock_timeout 防护                                 │  │
│  └───────────────────────────────────────────────────────────────────┘  │
│  ┌───────────────────────────────────────────────────────────────────┐  │
│  │ 3. 状态与审计层 (State & Audit Catalog)                            │  │
│  │    - Shared Memory 实时进度与锁状态暴露                            │  │
│  │    - 审计表: pg_auto_reindex_history                              │  │
│  └───────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 3. 子系统详细设计 (Detailed Subsystem Architecture)

### 3.1 子系统一：内核级索引膨胀估算引擎 (C Engine Bloat Estimator)

放弃上一代简单粗暴的表级 SQL，改用深度贴合 B-Tree 物理结构的 C 语言估算函数。

#### 算法模型改进：
1. **精确物理页开销计算**：
   $$\text{UsablePageSize} = 8192 - \text{SizeOfPageHeaderData}(24) - \text{BTPageOpaqueData}(16)$$
   $$\text{EffectiveFillFactor} = \frac{\text{fillfactor}}{100.0} \quad (\text{Leaf Pages Default } 90\%)$$
   $$\text{TuplesPerPage} = \lfloor \frac{\text{UsablePageSize} \times \text{EffectiveFillFactor}}{\text{AvgTupleSize} + \text{SizeOfItemIdData}(4)} \rfloor$$
2. **表达式与多列索引对齐支持**：
   - 遍历 `pg_index.indkey`，对表达式列（`attnum = 0`）自动回退到表级属性平均宽度或采样估算，避免计算为 NULL。
   - 引入 Null Bitmap 存在性判定与 MAXALIGN 对齐补齐（Padding）。
3. **统计信息不可靠回退 (Stale Statistics Fallback)**：
   - 增加对 `pg_class.relpages` 与真实 tuple 数量偏离度的检验，标记 `is_reliable` 标志位。

#### 核心 SQL 接口设计：
```sql
CREATE FUNCTION pg_auto_reindex_bloat_check(
    IN relation regclass,
    OUT bloat_bytes bigint,
    OUT bloat_ratio double precision,
    OUT is_reliable boolean,
    OUT recommendation text
) RETURNS record ...;
```

---

### 3.2 子系统二：安全并发重索引执行器 (Transaction-Safe Execution Engine)

解决原本在 SPI 事务块中直接执行 `REINDEX CONCURRENTLY` 引发的崩溃缺陷。

#### 执行架构设计：
1. **脱离 SPI 事务块执行**：
   - C 扩展内部通过直接调用内核 `reindex_index()` 接口或使用纯 C 连接上下文，确保 `REINDEX CONCURRENTLY` 运行在独立的非事务环境（Non-transaction Block）。
2. **长事务与锁冲突熔断机制 (Lock & Long-Xact Shield)**：
   - 在发起重索引前，检测当前数据库是否有处于 `idle in transaction` 或运行时间超过 `max_xact_duration` 的长事务。若存在长事务，**直接拒绝触发**，防止 `REINDEX CONCURRENTLY` 在等待 Old Transaction 时卡死。
   - 显式设置 `lock_timeout` 与 `statement_timeout`，并在捕获超时信号时优雅退出。
3. **取消与信号处理 (Cancel Integration)**：
   - 注册 `ProcessInterrupts()` 响应，当接收到 `pg_cancel_backend()` 或 SIGHUP 时，安全清理临时索引状态并退出。

---

### 3.3 子系统三：独立控制平面 Daemon (Control Plane Orchestrator)

将原本放在 C 语言 Worker 中的 EWMA 学习模型与调度逻辑提升至独立控制面（提供 Go / Python 语言实现的轻量 Daemon，也可通过 `pg_cron` 调用）。

#### 功能模块设计：
1. **多维度空闲评估器 (Multi-Metric Idle Evaluator)**：
   - **cgroups CPU/Memory 感知**：读取 `/sys/fs/cgroup/...` 真实容器资源利用率，避免 OS `getloadavg()` 盲区。
   - **数据库物理 I/O 与锁监控**：读取 `pg_stat_io` 与 `pg_stat_activity` 中的 `wait_event` 数量。
2. **168h EWMA 状态持久化**：
   - 学习矩阵持久化存储于 `pg_auto_reindex_learning_stats` 表或 Daemon 本地 SQLite/JSON 文件中。重启后自动恢复学习基线。
3. **多数据库自动化轮询 (Multi-DB Scanner)**：
   - 单个 Daemon 自动扫描实例下的所有 Database，根据并发限制与优先级依次调度重索引任务。

---

### 3.4 子系统四：状态暴露与审计系统 (Observability & Audit Catalog)

#### 1. 实时进度与状态共享内存 (`pg_stat_progress_reindex` 集成)
暴露无锁读取的视图 `pg_auto_reindex_status`：
```sql
SELECT * FROM pg_auto_reindex_status;
-- 返回：current_db, current_index, phase, bytes_reclaimed, active_workers
```

#### 2. 审计历史表 (`pg_auto_reindex_history`)
```sql
CREATE TABLE pg_auto_reindex_history (
    id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    dbname name NOT NULL,
    schemaname name NOT NULL,
    indexname name NOT NULL,
    start_time timestamptz NOT NULL,
    end_time timestamptz NOT NULL,
    bytes_before bigint NOT NULL,
    bytes_after bigint NOT NULL,
    status text NOT NULL, /* 'SUCCESS', 'LOCK_TIMEOUT', 'CANCELLED' */
    error_message text
);
```

---

## 4. 重构后的代码目录规范 (New Repository Layout)

```text
pg_auto_reindex/
├── Makefile                    # PGXS 构建文件
├── meson.build                 # Modern Meson 构建支持
├── pg_auto_reindex.control     # 扩展控制文件
├── pg_auto_reindex--2.0.sql    # 扩展 SQL 接口与视图定义
├── src/                        # 内核 C 扩展源码
│   ├── bloat_estimator.c       # 精确物理页 B-Tree 膨胀评估
│   ├── executor_safe.c         # 非事务块安全 REINDEX 执行器
│   ├── shmem_status.c          # 共享内存无锁状态暴露
│   └── pg_auto_reindex.c       # 模块初始化与 GUC 注册
├── daemon/                     # 控制平面 Daemon (Go / Python)
│   ├── main.go                 # Daemon 入口
│   ├── learner/                # 168h EWMA 学习引擎与 cgroups 采样
│   └── scheduler/              # 多库调度与熔断控制
├── doc/                        # 架构与技术文档
│   ├── redesign_architecture.md
│   └── pg_hackers_code_review.md
├── test/                       # 自动化测试套件
│   ├── sql/                    # pg_regress 回归测试
│   └── spec/                   # 长事务与并发死锁压力测试
└── README.md
```

---

## 5. 架构演进对照表 (Architecture Comparison Matrix)

| 维度 | 旧架构 (v1.0) | 重构后新架构 (v2.0) |
| :--- | :--- | :--- |
| **执行载体** | 内核 C Worker 内部直接跑调度与 DDL | **机制归内核 C，策略归外部 Control Daemon** |
| **Crash 风险** | C 逻辑异常可导致整个 PG 主库崩溃 | **主库零 Crash 风险**，控制面解耦 |
| **事务安全性** | SPI 事务块内直接调用 `REINDEX CONCURRENTLY` (100% 崩溃) | **脱离 SPI 事务块**，安全并发执行与长事务熔断 |
| **空闲判定** | 操作系统 `getloadavg()`（容器环境失真） | **cgroups + 数据库物理 I/O + 锁等待综合评估** |
| **学习基线** | 纯 Shmem 数组（重启即丢失） | **表 / 文件持久化存储**，跨重启与 Failover 继承 |
| **多库支持** | 仅支持单 GUC 指定数据库 | **原生全实例多 Database 调度** |
