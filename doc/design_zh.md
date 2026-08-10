# pg_auto_reindex 插件中文设计文档

## 1. 项目概述

`pg_auto_reindex` 是一个为 PostgreSQL 设计的自动化索引重建插件。它的主要目标是解决数据库运行过程中 B-Tree 索引由于频繁更新、删除操作而产生的膨胀（Bloat）问题。
传统的索引重建通常需要 DBA 定期手动评估和执行，这可能导致资源争用或维护不及时。本插件通过 **空闲学习（Idle Learning）** 和 **后台并发重建（Background Concurrent Reindexing）**，能够在系统负载较低的空闲时段自动识别高膨胀索引并进行安全的重建操作，从而实现数据库性能的自我调节和优化，降低运维成本。

## 2. 核心架构

`pg_auto_reindex` 采用 PostgreSQL 的 Background Worker 架构，独立于常规的客户端连接运行，并利用共享内存（Shared Memory）在各个时间槽及重建周期之间维护状态。

### 架构图

```mermaid
graph TD
    A[Background Worker main loop] -->|Naptime 唤醒| B[CollectSystemMetricsAndLearnerUpdate]
    A --> C{IsSystemIdle?}
    C -->|Yes| D[CleanupInvalidIndexes]
    D --> E[ExecuteAutoReindexCycle]
    E --> F[更新 pg_auto_reindex_history]
    C -->|No| A
    B --> G[(Shared Memory EWMA Slots)]
    C --> G
    
    subgraph 共享内存状态 (AutoReindexSharedState)
        G1[168 时间槽状态矩阵]
        G2[当前重建的 Index OID]
        G3[上次重建时间]
        G4[累计重建次数和节省空间]
    end
    G -.-> 共享内存状态
```

### 共享内存设计

插件在启动时申请了一块 `AutoReindexSharedState` 共享内存结构，并使用 `pg_auto_reindex` 命名 LWLock 进行并发保护。主要存储内容包括：
- **168 时间槽 EWMA 矩阵**：记录一周内每个小时（7 * 24 = 168）的平均负载和活跃连接数。
- **全局状态指标**：当前正在重建的索引 OID (`current_reindexing_index`)、上次完成重建的时间戳 (`last_reindex_time`)、总计重建索引数量 (`total_reindexed_count`) 以及累计回收的字节数 (`total_bytes_saved`)。
- **空闲判定状态**：当前是否处于空闲状态 (`is_idle`) 及连续空闲采样的次数 (`consecutive_idle_count`)。

## 3. 四大核心模块

### 3.1 空闲学习器 (idle_learner)
- **168 时间槽 EWMA 算法**：插件根据一周 168 个小时划分时间槽。在每次唤醒采样时，计算当前时间所属的时间槽，并通过指数加权移动平均（EWMA，Alpha = 0.20）算法更新该槽位的历史负载（Load Average）和活跃后端数。
- **指标采集**：通过 `getloadavg()` 获取 CPU 1 分钟平均负载，通过查询 `pg_stat_activity` 统计状态为 `active` 的后端进程数。
- **空闲判定逻辑**：要求连续 3 次（通常代表 3 个 naptime 周期）满足空闲条件才判定系统为 IDLE。空闲条件包括绝对阈值（当前负载和连接数低于 `max_idle_load` 和 `max_idle_backends`）和相对基线阈值（当前值不高于历史同槽位 EWMA 值的 `idle_ratio_threshold`）。

### 3.2 膨胀评估器 (bloat_estimator)
- **基于元数据的估算**：为了避免全表扫描带来的高昂代价，评估器通过查询 `pg_class`、`pg_index` 和 `pg_statistic` 等系统目录元数据，利用统计信息中的平均列宽（`avg_width`）和元组数（`reltuples`）估算索引理论上需要占用的页面数。
- **SQL 查询设计**：查询排除了系统目录（`pg_catalog` 等），过滤出有效（`indisvalid = true`）的 B-Tree 索引。只有当索引当前大小超过 `min_bloat_bytes` 且估算的膨胀率高于 `min_bloat_ratio` 时，才会被选为候选索引。按膨胀字节数倒序排列，优先处理膨胀最严重的索引。

### 3.3 重建执行器 (reindex_executor)
- **REINDEX CONCURRENTLY**：采用并发模式重建索引（`REINDEX INDEX CONCURRENTLY`），避免在重建过程中阻塞表上的读写操作。
- **锁超时保护**：在执行重建 SQL 前，会设置会话级的 `lock_timeout`（默认为 5000 毫秒）。如果获取锁超时，操作会安全中止，不会长期阻塞其他事务。
- **PG_TRY/CATCH 错误处理**：通过 PostgreSQL C 语言环境下的 `PG_TRY()` / `PG_CATCH()` 机制捕获重建失败、锁超时等异常，确保 Background Worker 不会因单个索引重建失败而崩溃。
- **无效索引自愈**：并发重建如果失败或被强制中断，可能会留下后缀带有 `_ccnew` 的无效索引（`indisvalid = false`）。执行器在每次重建周期开始前，会自动执行 `CleanupInvalidIndexes` 尝试并发删除这些残留的无效索引。

### 3.4 审计日志 (pg_auto_reindex_history)
- **表结构与记录内容**：插件自动创建了 `pg_auto_reindex_history` 表。其中记录了每次重建任务的详细信息，包含：
  - `schemaname` 和 `indexname`：模式名与索引名
  - `start_time` 和 `end_time`：重建开始与结束的时间戳
  - `bytes_before` 和 `bytes_after`：重建前后的索引字节大小
  - `bytes_saved`：自动计算回收的空间（生成列）
  - `status`：执行结果状态（如 `SUCCESS` 或 `TIMEOUT`）

## 4. GUC 参数详解

| 参数名称 | 类型 | 默认值 | 范围 | 用途说明 |
|----------|------|--------|------|----------|
| `pg_auto_reindex.enabled` | boolean | `true` | - | 开启或关闭插件的自动重建功能 |
| `pg_auto_reindex.database` | string | `'postgres'` | - | 设定 Background Worker 连接的目标数据库 |
| `pg_auto_reindex.naptime` | integer | `60` | 1 - 3600 | 采样和检查周期间隔（秒） |
| `pg_auto_reindex.idle_ratio_threshold` | real | `0.70` | 0.10 - 1.00 | 当前负载相对于历史 EWMA 基线的最大比例，低于此比例视为相对空闲 |
| `pg_auto_reindex.max_idle_load` | real | `2.0` | 0.1 - 100.0 | 系统被视为空闲状态允许的最大 1 分钟 CPU 平均负载绝对值 |
| `pg_auto_reindex.max_idle_backends` | integer | `15` | 0 - 1000 | 系统被视为空闲状态允许的最大活跃后端连接数绝对值 |
| `pg_auto_reindex.min_bloat_ratio` | real | `0.30` | 0.05 - 0.99 | 触发索引重建的最低估算膨胀率（例如 0.30 代表 30% 膨胀） |
| `pg_auto_reindex.min_bloat_bytes` | int64 | `67108864` | - | 触发索引重建的最低物理大小（默认 64MB） |
| `pg_auto_reindex.lock_timeout_ms` | integer | `5000` | 100 - 300000 | 执行并发重建前获取所需锁的超时时间（毫秒），防阻塞保护机制 |
| `pg_auto_reindex.max_reindexes_per_idle` | integer | `2` | 1 - 100 | 每次确认空闲状态后，单次周期内最多尝试重建的索引数量 |

## 5. SQL 函数接口

- `pg_auto_reindex_stats()`：返回包含所有 168 个时间槽统计信息的视图，包含 `slot_id`, `ewma_loadavg`, `ewma_active_backends`, `sample_count` 等字段，便于分析系统负载历史。
- `pg_auto_reindex_bloat_report()`：根据当前配置阈值，动态返回满足条件的索引膨胀报告（评估大小、膨胀比率和膨胀字节数），主要用于 DBA 手动审核和校验。
- `pg_auto_reindex_status()`：返回当前后台工作进程的运行状态，包含是否处于空闲（`is_idle`）、当前重建的索引 OID（`current_reindexing_index`）、最后一次重建时间、总重建次数及总节省空间。
- `pg_auto_reindex_trigger()`：手动触发一次无效索引清理和重建判定流程，返回 `true`。

## 6. 安装与使用

### 编译与安装
确保 PostgreSQL 源码环境配置正确，在插件目录下执行：
```bash
make
make install
```

### 配置与启用
在 `postgresql.conf` 中配置加载插件及设置目标数据库：
```conf
shared_preload_libraries = 'pg_auto_reindex'
pg_auto_reindex.database = 'your_target_db'
```

重启 PostgreSQL 实例。

在目标数据库中创建扩展以部署相关表和函数：
```sql
CREATE EXTENSION pg_auto_reindex;
```

## 7. 安全性设计

- **锁超时保护 (Lock Timeout)**：自动重建最容易导致的问题是由于 `REINDEX CONCURRENTLY` 等待锁资源而阻塞正常业务查询。通过在执行前动态设置 `SET lock_timeout`，如果无法在指定短时间内获得锁，重建操作将自动失败回滚并被捕获，不会发生严重的死锁或长查询阻塞。
- **事务隔离**：Background Worker 的各种操作（如清理无效索引、读取膨胀候选者、执行并发重建）分别在各自独立的短事务中执行。
- **错误恢复**：借助 `PG_TRY`/`PG_CATCH`，即使发生内存不足、超时或执行 SQL 报错，工作进程也能自我恢复，不影响下次调度。并发构建遗留的无效索引会在下一次周期执行前自动通过 `DROP INDEX CONCURRENTLY IF EXISTS` 尝试清理，实现自我修复。

## 8. 膨胀评估公式推导

`estimated_bloat_ratio` 的计算公式如下：

```sql
ROUND(
  (CASE WHEN c.relpages > 0 THEN
    ((c.relpages - GREATEST(CEIL(c.reltuples * (COALESCE(s.avg_width, 16) + 8) / (8192 - 64)), 1)) / c.relpages::numeric)
  ELSE 0 END)::numeric, 2)::float8
```

**推导说明**：
1. **单条索引元组占用大小**：
   从 `pg_statistic` 中累加取得索引列的平均宽度 `s.avg_width`。加上 8 字节（PostgreSQL 中常见的索引元组 Header 开销）。
2. **总理论数据量**：
   索引行数 `c.reltuples` 乘以 `(avg_width + 8)` 得到存储这些行所需的理论有效载荷。
3. **单页可用空间**：
   标准块大小为 `8192` 字节，减去页头（Page Header）开销及特殊空间等保留量 `64` 字节，单页实际可用空间约为 `8192 - 64` 字节。
4. **理论所需页数**：
   通过 `CEIL( 总理论数据量 / (8192 - 64) )` 计算紧凑排布下所需的页面数（并利用 `GREATEST(..., 1)` 确保至少需要 1 页）。
5. **膨胀率计算**：
   物理分配的页面数 `c.relpages` 减去“理论所需页数”得到膨胀浪费的页面数，再除以 `c.relpages`，即为膨胀率。最后通过 `ROUND(..., 2)` 保留两位小数。
