#!/usr/bin/env bash
#=============================================================================
# pg_auto_reindex - 生产环境长时间模拟测试
#
# 测试场景:
#   1. 模拟多张业务表的 OLTP 读写负载 (INSERT/UPDATE/DELETE)
#   2. 通过大量 UPDATE/DELETE 制造索引膨胀
#   3. 启用 pg_auto_reindex 后台 worker
#   4. 周期性监控: EWMA 学习状态、索引膨胀率、自动重建历史
#   5. 验证: 索引空间是否被回收、审计日志是否正确记录
#
# 使用方法:
#   ./production_simulation.sh [选项]
#
# 选项:
#   --duration <minutes>   总运行时长 (默认: 10 分钟)
#   --pgbin <path>         pg_config 所在的 bin 目录
#   --port <port>          PostgreSQL 端口 (默认: 5432)
#   --db <dbname>          测试数据库名 (默认: auto_reindex_test)
#   --bloat-rounds <n>     膨胀制造轮次 (默认: 5)
#   --help                 显示帮助信息
#=============================================================================

set -euo pipefail

# ========================== 默认参数 ==========================
DURATION_MINUTES=10
PGBIN="/home/fengyao/code/release/mypg/bin"
PGPORT=5432
PGHOST="/tmp"
TESTDB="auto_reindex_test"
BLOAT_ROUNDS=5
MONITOR_INTERVAL=15   # 监控采样间隔 (秒)
LOG_DIR=""
CLEANUP_ON_EXIT=true

# ========================== 颜色输出 ==========================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m' # No Color

# ========================== 函数定义 ==========================

usage() {
    cat <<EOF
用法: $0 [选项]

pg_auto_reindex 生产环境长时间模拟测试

选项:
  --duration <minutes>     总运行时长 (默认: $DURATION_MINUTES 分钟)
  --pgbin <path>           PostgreSQL bin 目录 (默认: $PGBIN)
  --port <port>            PostgreSQL 端口 (默认: $PGPORT)
  --db <dbname>            测试数据库名 (默认: $TESTDB)
  --bloat-rounds <n>       膨胀制造轮次 (默认: $BLOAT_ROUNDS)
  --no-cleanup             测试结束后不清理数据库
  --help                   显示此帮助信息

示例:
  # 运行 30 分钟的模拟测试
  $0 --duration 30

  # 使用自定义端口和数据库
  $0 --duration 15 --port 5433 --db mytest
EOF
    exit 0
}

log_info()    { echo -e "${CYAN}[$(date +'%H:%M:%S')]${NC} ${GREEN}[INFO]${NC}  $*"; }
log_warn()    { echo -e "${CYAN}[$(date +'%H:%M:%S')]${NC} ${YELLOW}[WARN]${NC}  $*"; }
log_error()   { echo -e "${CYAN}[$(date +'%H:%M:%S')]${NC} ${RED}[ERROR]${NC} $*"; }
log_section() { echo -e "\n${BOLD}${CYAN}═══════════════════════════════════════════════════${NC}"; echo -e "${BOLD}  $*${NC}"; echo -e "${BOLD}${CYAN}═══════════════════════════════════════════════════${NC}\n"; }
log_step()    { echo -e "${CYAN}[$(date +'%H:%M:%S')]${NC} ${BOLD}▶${NC} $*"; }

PSQL="${PGBIN}/psql"

run_sql() {
    "${PSQL}" -h "${PGHOST}" -p "${PGPORT}" -d "$1" -X -A -t -c "$2" 2>/dev/null
}

run_sql_v() {
    "${PSQL}" -h "${PGHOST}" -p "${PGPORT}" -d "$1" -X -c "$2" 2>/dev/null
}

# ========================== 参数解析 ==========================
while [[ $# -gt 0 ]]; do
    case $1 in
        --duration)     DURATION_MINUTES="$2"; shift 2 ;;
        --pgbin)        PGBIN="$2"; PSQL="${PGBIN}/psql"; shift 2 ;;
        --port)         PGPORT="$2"; shift 2 ;;
        --db)           TESTDB="$2"; shift 2 ;;
        --bloat-rounds) BLOAT_ROUNDS="$2"; shift 2 ;;
        --no-cleanup)   CLEANUP_ON_EXIT=false; shift ;;
        --help)         usage ;;
        *)              log_error "未知选项: $1"; usage ;;
    esac
done

DURATION_SECONDS=$((DURATION_MINUTES * 60))
LOG_DIR="/tmp/pg_auto_reindex_test_$(date +'%Y%m%d_%H%M%S')"
mkdir -p "$LOG_DIR"

# ========================== 清理函数 ==========================
cleanup() {
    log_section "清理 & 收尾"

    # 停止所有后台作业
    jobs -p 2>/dev/null | xargs -r kill 2>/dev/null || true
    wait 2>/dev/null || true

    # 恢复全局配置
    run_sql "postgres" "ALTER SYSTEM RESET pg_auto_reindex.min_bloat_ratio;" 2>/dev/null || true
    run_sql "postgres" "ALTER SYSTEM RESET pg_auto_reindex.min_bloat_bytes;" 2>/dev/null || true
    run_sql "postgres" "SELECT pg_reload_conf();" 2>/dev/null || true

    if [ "$CLEANUP_ON_EXIT" = true ]; then
        log_step "删除测试数据库: ${TESTDB}"
        run_sql "postgres" "DROP DATABASE IF EXISTS ${TESTDB};" 2>/dev/null || true
    else
        log_info "保留测试数据库: ${TESTDB} (--no-cleanup)"
    fi

    log_info "日志目录: ${LOG_DIR}"
    log_info "清理完成"
}

trap cleanup EXIT

# ========================== 前置检查 ==========================
log_section "Phase 0: 环境检查"

if ! command -v "${PSQL}" &>/dev/null; then
    log_error "找不到 psql: ${PSQL}"
    exit 1
fi
log_info "psql: ${PSQL}"

PG_VERSION=$(run_sql "postgres" "SELECT version();" | head -1)
log_info "PostgreSQL: ${PG_VERSION}"

# 检查扩展是否已安装到系统
if ! run_sql "postgres" "SELECT 1 FROM pg_available_extensions WHERE name = 'pg_auto_reindex';" | grep -q 1; then
    log_error "pg_auto_reindex 扩展未安装到 PostgreSQL. 请先执行 make install."
    exit 1
fi
log_info "pg_auto_reindex 扩展已在可用列表中 ✓"

# ========================== Phase 1: 创建测试数据库 & schema ==========================
log_section "Phase 1: 创建测试数据库和表结构"

run_sql "postgres" "DROP DATABASE IF EXISTS ${TESTDB};"
run_sql "postgres" "CREATE DATABASE ${TESTDB};"
log_info "数据库 ${TESTDB} 创建完成"

# 安装扩展
run_sql "${TESTDB}" "CREATE EXTENSION pg_auto_reindex;"
log_info "pg_auto_reindex 扩展已安装"

# 创建模拟业务表
log_step "创建模拟业务表..."

run_sql "${TESTDB}" "
-- ===== 表1: 订单表 (高频写入 + 更新) =====
CREATE TABLE orders (
    id              bigserial PRIMARY KEY,
    user_id         bigint NOT NULL,
    product_id      bigint NOT NULL,
    order_status    smallint NOT NULL DEFAULT 0,
    amount          numeric(12,2) NOT NULL,
    created_at      timestamptz NOT NULL DEFAULT now(),
    updated_at      timestamptz NOT NULL DEFAULT now(),
    remark          text
);

CREATE INDEX idx_orders_user_id     ON orders (user_id);
CREATE INDEX idx_orders_product_id  ON orders (product_id);
CREATE INDEX idx_orders_status      ON orders (order_status);
CREATE INDEX idx_orders_created_at  ON orders (created_at);
CREATE INDEX idx_orders_composite   ON orders (user_id, order_status, created_at);

-- ===== 表2: 用户行为日志 (高频写入 + 大量删除) =====
CREATE TABLE user_events (
    id              bigserial PRIMARY KEY,
    user_id         bigint NOT NULL,
    event_type      varchar(50) NOT NULL,
    payload         jsonb,
    created_at      timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX idx_events_user_id     ON user_events (user_id);
CREATE INDEX idx_events_type        ON user_events (event_type);
CREATE INDEX idx_events_created_at  ON user_events (created_at);
CREATE INDEX idx_events_payload     ON user_events USING gin (payload);

-- ===== 表3: 会话表 (频繁更新 + 过期删除) =====
CREATE TABLE sessions (
    id              bigserial PRIMARY KEY,
    session_token   varchar(128) UNIQUE NOT NULL,
    user_id         bigint NOT NULL,
    ip_addr         inet,
    last_active     timestamptz NOT NULL DEFAULT now(),
    expires_at      timestamptz NOT NULL DEFAULT (now() + interval '30 minutes'),
    data            jsonb DEFAULT '{}'::jsonb
);

CREATE INDEX idx_sessions_user_id   ON sessions (user_id);
CREATE INDEX idx_sessions_expires   ON sessions (expires_at);
CREATE INDEX idx_sessions_active    ON sessions (last_active);

-- ===== 表4: 库存变动表 (频繁 UPDATE) =====
CREATE TABLE inventory (
    id              bigserial PRIMARY KEY,
    sku             varchar(64) NOT NULL,
    warehouse_id    int NOT NULL,
    quantity        int NOT NULL DEFAULT 0,
    reserved        int NOT NULL DEFAULT 0,
    updated_at      timestamptz NOT NULL DEFAULT now()
);

CREATE INDEX idx_inventory_sku          ON inventory (sku);
CREATE INDEX idx_inventory_warehouse    ON inventory (warehouse_id);
CREATE INDEX idx_inventory_qty          ON inventory (quantity);
CREATE UNIQUE INDEX idx_inventory_sku_wh ON inventory (sku, warehouse_id);
"

log_info "4 张业务表和 17 个索引创建完成"

# ========================== Phase 2: 灌入初始数据 ==========================
log_section "Phase 2: 灌入初始数据"

log_step "灌入 orders 表 (100,000 行)..."
run_sql "${TESTDB}" "
INSERT INTO orders (user_id, product_id, order_status, amount, remark)
SELECT
    (random() * 10000)::bigint,
    (random() * 5000)::bigint,
    (random() * 4)::smallint,
    (random() * 9999 + 1)::numeric(12,2),
    repeat(md5(random()::text), 2)
FROM generate_series(1, 100000);
"

log_step "灌入 user_events 表 (200,000 行)..."
run_sql "${TESTDB}" "
INSERT INTO user_events (user_id, event_type, payload)
SELECT
    (random() * 10000)::bigint,
    (ARRAY['click','view','purchase','search','login','logout','add_cart','remove_cart'])[1 + (random()*7)::int],
    jsonb_build_object('page', '/page/' || (random()*1000)::int, 'ts', now() - (random() * interval '30 days'))
FROM generate_series(1, 200000);
"

log_step "灌入 sessions 表 (50,000 行)..."
run_sql "${TESTDB}" "
INSERT INTO sessions (session_token, user_id, ip_addr, last_active, expires_at)
SELECT
    md5(random()::text || i::text),
    (random() * 10000)::bigint,
    ('192.168.' || (random()*255)::int || '.' || (random()*255)::int)::inet,
    now() - (random() * interval '2 hours'),
    now() + (random() * interval '1 hour') - interval '30 minutes'
FROM generate_series(1, 50000) AS s(i);
"

log_step "灌入 inventory 表 (20,000 行)..."
run_sql "${TESTDB}" "
INSERT INTO inventory (sku, warehouse_id, quantity, reserved)
SELECT
    'SKU-' || lpad(i::text, 8, '0'),
    (random() * 10)::int + 1,
    (random() * 1000)::int,
    (random() * 100)::int
FROM generate_series(1, 20000) AS s(i)
ON CONFLICT DO NOTHING;
"

# 更新统计信息
run_sql "${TESTDB}" "ANALYZE;"
log_info "初始数据灌入完成, 已执行 ANALYZE"

# 打印初始大小
log_step "初始索引大小汇总:"
run_sql_v "${TESTDB}" "
SELECT
    n.nspname || '.' || c.relname AS index_name,
    pg_size_pretty(pg_relation_size(c.oid)) AS size,
    c.reltuples::bigint AS est_tuples
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE c.relkind = 'i'
  AND n.nspname = 'public'
ORDER BY pg_relation_size(c.oid) DESC;
"

# ========================== Phase 3: 制造索引膨胀 ==========================
log_section "Phase 3: 制造索引膨胀 (${BLOAT_ROUNDS} 轮)"

for round in $(seq 1 "${BLOAT_ROUNDS}"); do
    log_step "膨胀轮次 ${round}/${BLOAT_ROUNDS}..."

    # orders: 大量更新索引列，导致索引页面碎片化
    run_sql "${TESTDB}" "
    UPDATE orders
    SET order_status = (order_status + 1) % 5,
        updated_at = now(),
        amount = amount + (random() * 10 - 5)
    WHERE id IN (
        SELECT id FROM orders
        ORDER BY random()
        LIMIT 30000
    );
    "

    # user_events: 删除旧数据 + 插入新数据 (模拟日志轮转)
    run_sql "${TESTDB}" "
    DELETE FROM user_events
    WHERE id IN (
        SELECT id FROM user_events
        ORDER BY random()
        LIMIT 40000
    );
    INSERT INTO user_events (user_id, event_type, payload)
    SELECT
        (random() * 10000)::bigint,
        (ARRAY['click','view','purchase','search','login','logout'])[1 + (random()*5)::int],
        jsonb_build_object('page', '/page/' || (random()*1000)::int, 'round', ${round})
    FROM generate_series(1, 20000);
    "

    # sessions: 过期删除 + 重新创建 (模拟会话更替)
    run_sql "${TESTDB}" "
    DELETE FROM sessions
    WHERE id IN (
        SELECT id FROM sessions
        ORDER BY random()
        LIMIT 15000
    );
    INSERT INTO sessions (session_token, user_id, ip_addr, last_active, expires_at)
    SELECT
        md5(random()::text || '${round}' || i::text),
        (random() * 10000)::bigint,
        ('10.' || (random()*255)::int || '.' || (random()*255)::int || '.' || (random()*255)::int)::inet,
        now(),
        now() + interval '30 minutes'
    FROM generate_series(1, 10000) AS s(i);
    "

    # inventory: 频繁 UPDATE (库存变动)
    run_sql "${TESTDB}" "
    UPDATE inventory
    SET quantity = GREATEST(0, quantity + (random() * 200 - 100)::int),
        reserved = GREATEST(0, reserved + (random() * 50 - 25)::int),
        updated_at = now()
    WHERE id IN (
        SELECT id FROM inventory
        ORDER BY random()
        LIMIT 10000
    );
    "

    log_info "  轮次 ${round} 完成"
done

# 更新统计信息
run_sql "${TESTDB}" "ANALYZE;"

# 显示膨胀后的索引大小
log_step "膨胀后索引大小汇总:"
run_sql_v "${TESTDB}" "
SELECT
    n.nspname || '.' || c.relname AS index_name,
    pg_size_pretty(pg_relation_size(c.oid)) AS size,
    c.reltuples::bigint AS est_tuples,
    c.relpages AS pages
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE c.relkind = 'i'
  AND n.nspname = 'public'
ORDER BY pg_relation_size(c.oid) DESC;
"

# ========================== Phase 4: 显示初始膨胀报告 ==========================
log_section "Phase 4: 索引膨胀评估报告"

log_step "调用 pg_auto_reindex_bloat_report():"
run_sql_v "${TESTDB}" "
SELECT
    schemaname,
    indexname,
    pg_size_pretty(current_bytes) AS current_size,
    round((estimated_bloat_ratio * 100)::numeric, 1) || '%' AS bloat_pct,
    pg_size_pretty(estimated_bloat_bytes) AS bloat_size
FROM pg_auto_reindex_bloat_report()
ORDER BY estimated_bloat_bytes DESC;
"

log_step "调用 pg_auto_reindex_stats() 查看 EWMA 学习状态 (当前时段):"
run_sql_v "${TESTDB}" "
SELECT slot_id, day_of_week, hour_of_day,
       round(ewma_loadavg::numeric, 3) AS load_avg,
       round(ewma_active_backends::numeric, 1) AS backends,
       sample_count,
       is_current_slot
FROM pg_auto_reindex_stats()
WHERE is_current_slot = true;
"

log_step "调用 pg_auto_reindex_status() 查看 worker 状态:"
run_sql_v "${TESTDB}" "SELECT * FROM pg_auto_reindex_status();"

# ========================== Phase 5: 配置低阈值并手动触发 ==========================
log_section "Phase 5: 调整 GUC 参数 & 手动触发 Reindex 测试"

# 降低阈值使得更容易触发 reindex
log_step "降低膨胀阈值以便测试 (min_bloat_ratio=0.10, min_bloat_bytes=1MB)..."
run_sql "${TESTDB}" "ALTER SYSTEM SET pg_auto_reindex.min_bloat_ratio = 0.10;"
run_sql "${TESTDB}" "ALTER SYSTEM SET pg_auto_reindex.min_bloat_bytes = '1MB';"
run_sql "${TESTDB}" "SELECT pg_reload_conf();"
# 等待配置生效
sleep 2

log_step "手动触发 pg_auto_reindex_trigger()..."
TRIGGER_RESULT=$(run_sql "${TESTDB}" "SELECT pg_auto_reindex_trigger();")
log_info "trigger 返回: ${TRIGGER_RESULT}"

# 检查审计日志
log_step "检查审计历史 pg_auto_reindex_history:"
run_sql_v "${TESTDB}" "
SELECT
    id,
    schemaname,
    indexname,
    pg_size_pretty(bytes_before) AS before,
    pg_size_pretty(bytes_after) AS after,
    pg_size_pretty(bytes_saved) AS saved,
    status,
    end_time - start_time AS duration
FROM pg_auto_reindex_history
ORDER BY id;
"

# ========================== Phase 6: 长时间并发 OLTP 模拟 ==========================
log_section "Phase 6: 长时间并发 OLTP 模拟 (${DURATION_MINUTES} 分钟)"

START_EPOCH=$(date +%s)
END_EPOCH=$((START_EPOCH + DURATION_SECONDS))
ITERATION=0

# --- 后台 Writer 1: 订单写入器 ---
(
    while [ "$(date +%s)" -lt "${END_EPOCH}" ]; do
        "${PSQL}" -h "${PGHOST}" -p "${PGPORT}" -d "${TESTDB}" -X -A -t -c "
        INSERT INTO orders (user_id, product_id, order_status, amount, remark)
        SELECT
            (random() * 10000)::bigint,
            (random() * 5000)::bigint,
            0,
            (random() * 9999 + 1)::numeric(12,2),
            repeat(md5(random()::text), 1)
        FROM generate_series(1, 500);

        UPDATE orders
        SET order_status = (order_status + 1) % 5,
            updated_at = now()
        WHERE id IN (
            SELECT id FROM orders
            WHERE order_status < 4
            ORDER BY random()
            LIMIT 200
        );
        " >/dev/null 2>&1
        sleep 2
    done
) &
WRITER1_PID=$!
log_info "后台 Writer-Orders 已启动 (PID: ${WRITER1_PID})"

# --- 后台 Writer 2: 事件日志写入器 ---
(
    while [ "$(date +%s)" -lt "${END_EPOCH}" ]; do
        "${PSQL}" -h "${PGHOST}" -p "${PGPORT}" -d "${TESTDB}" -X -A -t -c "
        INSERT INTO user_events (user_id, event_type, payload)
        SELECT
            (random() * 10000)::bigint,
            (ARRAY['click','view','purchase','search','login'])[1 + (random()*4)::int],
            jsonb_build_object('page', '/p/' || (random()*500)::int, 'ts', extract(epoch from now()))
        FROM generate_series(1, 1000);

        DELETE FROM user_events
        WHERE id IN (
            SELECT id FROM user_events ORDER BY random() LIMIT 800
        );
        " >/dev/null 2>&1
        sleep 3
    done
) &
WRITER2_PID=$!
log_info "后台 Writer-Events 已启动 (PID: ${WRITER2_PID})"

# --- 后台 Writer 3: 会话更新器 ---
(
    while [ "$(date +%s)" -lt "${END_EPOCH}" ]; do
        "${PSQL}" -h "${PGHOST}" -p "${PGPORT}" -d "${TESTDB}" -X -A -t -c "
        DELETE FROM sessions WHERE expires_at < now();

        INSERT INTO sessions (session_token, user_id, ip_addr, last_active, expires_at)
        SELECT
            md5(random()::text || now()::text || i::text),
            (random() * 10000)::bigint,
            ('10.' || (random()*255)::int || '.' || (random()*255)::int || '.' || (random()*255)::int)::inet,
            now(),
            now() + interval '10 minutes'
        FROM generate_series(1, 300) AS s(i)
        ON CONFLICT (session_token) DO UPDATE SET last_active = now();

        UPDATE sessions
        SET last_active = now(),
            data = jsonb_build_object('visits', (random()*100)::int)
        WHERE id IN (SELECT id FROM sessions ORDER BY random() LIMIT 200);
        " >/dev/null 2>&1
        sleep 2
    done
) &
WRITER3_PID=$!
log_info "后台 Writer-Sessions 已启动 (PID: ${WRITER3_PID})"

# --- 后台 Writer 4: 库存更新器 ---
(
    while [ "$(date +%s)" -lt "${END_EPOCH}" ]; do
        "${PSQL}" -h "${PGHOST}" -p "${PGPORT}" -d "${TESTDB}" -X -A -t -c "
        UPDATE inventory
        SET quantity = GREATEST(0, quantity + (random() * 50 - 25)::int),
            reserved = GREATEST(0, reserved + (random() * 20 - 10)::int),
            updated_at = now()
        WHERE id IN (SELECT id FROM inventory ORDER BY random() LIMIT 500);
        " >/dev/null 2>&1
        sleep 4
    done
) &
WRITER4_PID=$!
log_info "后台 Writer-Inventory 已启动 (PID: ${WRITER4_PID})"

# --- 后台 Reader: 模拟查询负载 ---
(
    while [ "$(date +%s)" -lt "${END_EPOCH}" ]; do
        "${PSQL}" -h "${PGHOST}" -p "${PGPORT}" -d "${TESTDB}" -X -A -t -c "
        SELECT count(*) FROM orders WHERE user_id = (random()*10000)::bigint;
        SELECT count(*) FROM user_events WHERE event_type = 'click' AND created_at > now() - interval '1 hour';
        SELECT count(*) FROM sessions WHERE user_id = (random()*10000)::bigint AND expires_at > now();
        SELECT sum(quantity) FROM inventory WHERE warehouse_id = (random()*10)::int + 1;
        " >/dev/null 2>&1
        sleep 1
    done
) &
READER_PID=$!
log_info "后台 Reader-Queries 已启动 (PID: ${READER_PID})"

# --- 后台定期触发器: 周期性手动触发 reindex ---
(
    while [ "$(date +%s)" -lt "${END_EPOCH}" ]; do
        sleep 60
        "${PSQL}" -h "${PGHOST}" -p "${PGPORT}" -d "${TESTDB}" -X -A -t -c "
        SELECT pg_auto_reindex_trigger();
        " >/dev/null 2>&1
    done
) &
TRIGGER_PID=$!
log_info "后台 Trigger (每60s) 已启动 (PID: ${TRIGGER_PID})"

log_info "所有后台进程已启动, 开始 ${DURATION_MINUTES} 分钟监控..."
echo ""

# ========================== Phase 7: 监控循环 ==========================
MONITOR_LOG="${LOG_DIR}/monitor.log"
echo "timestamp,iteration,total_index_size_mb,bloated_count,reindexed_count,bytes_saved,is_idle,load_avg,active_backends" > "${MONITOR_LOG}"

while [ "$(date +%s)" -lt "${END_EPOCH}" ]; do
    ITERATION=$((ITERATION + 1))
    ELAPSED=$(( $(date +%s) - START_EPOCH ))
    REMAINING=$(( END_EPOCH - $(date +%s) ))

    # 采集指标
    TOTAL_IDX_SIZE=$(run_sql "${TESTDB}" "
        SELECT coalesce(round(sum(pg_relation_size(c.oid)) / 1024.0 / 1024.0, 2), 0)
        FROM pg_class c
        JOIN pg_namespace n ON n.oid = c.relnamespace
        WHERE c.relkind = 'i' AND n.nspname = 'public';
    " 2>/dev/null || echo "0")

    BLOATED_COUNT=$(run_sql "${TESTDB}" "
        SELECT count(*) FROM pg_auto_reindex_bloat_report();
    " 2>/dev/null || echo "0")

    REINDEX_INFO=$(run_sql "${TESTDB}" "
        SELECT coalesce(total_reindexed_count, 0) || '|' ||
               coalesce(total_bytes_saved, 0) || '|' ||
               coalesce(is_idle::text, 'unknown')
        FROM pg_auto_reindex_status();
    " 2>/dev/null || echo "0|0|unknown")

    REINDEXED_COUNT=$(echo "$REINDEX_INFO" | cut -d'|' -f1)
    BYTES_SAVED=$(echo "$REINDEX_INFO" | cut -d'|' -f2)
    IS_IDLE=$(echo "$REINDEX_INFO" | cut -d'|' -f3)

    HISTORY_COUNT=$(run_sql "${TESTDB}" "SELECT count(*) FROM pg_auto_reindex_history;" 2>/dev/null || echo "0")

    LOAD_AVG=$(run_sql "${TESTDB}" "
        SELECT round(ewma_loadavg::numeric, 2)
        FROM pg_auto_reindex_stats()
        WHERE is_current_slot = true
        LIMIT 1;
    " 2>/dev/null || echo "0")

    ACTIVE_BACKENDS=$(run_sql "${TESTDB}" "
        SELECT round(ewma_active_backends::numeric, 1)
        FROM pg_auto_reindex_stats()
        WHERE is_current_slot = true
        LIMIT 1;
    " 2>/dev/null || echo "0")

    BYTES_SAVED_PRETTY=$(run_sql "${TESTDB}" "SELECT pg_size_pretty(${BYTES_SAVED}::bigint);" 2>/dev/null || echo "0 bytes")

    # 输出到控制台
    printf "${CYAN}[%s]${NC} 迭代 %3d | 经过 %dm%02ds | 剩余 %dm%02ds | " \
        "$(date +'%H:%M:%S')" "${ITERATION}" \
        $((ELAPSED/60)) $((ELAPSED%60)) \
        $((REMAINING/60)) $((REMAINING%60))
    printf "索引 ${BOLD}%s MB${NC} | 膨胀 ${YELLOW}%s${NC} | 已重建 ${GREEN}%s${NC} | 节省 ${GREEN}%s${NC} | 历史 %s | 空闲 %s | load ${LOAD_AVG}\n" \
        "${TOTAL_IDX_SIZE}" "${BLOATED_COUNT}" "${REINDEXED_COUNT}" \
        "${BYTES_SAVED_PRETTY}" "${HISTORY_COUNT}" "${IS_IDLE}"

    # 写入日志
    echo "$(date -Iseconds),${ITERATION},${TOTAL_IDX_SIZE},${BLOATED_COUNT},${REINDEXED_COUNT},${BYTES_SAVED},${IS_IDLE},${LOAD_AVG},${ACTIVE_BACKENDS}" >> "${MONITOR_LOG}"

    sleep "${MONITOR_INTERVAL}"
done

# 等待后台进程结束
log_info "等待后台写入器结束..."
wait "${WRITER1_PID}" "${WRITER2_PID}" "${WRITER3_PID}" "${WRITER4_PID}" "${READER_PID}" "${TRIGGER_PID}" 2>/dev/null || true

# ========================== Phase 8: 最终报告 ==========================
log_section "Phase 8: 最终测试报告"

log_step "1) 最终索引大小:"
run_sql_v "${TESTDB}" "
SELECT
    n.nspname || '.' || c.relname AS index_name,
    pg_size_pretty(pg_relation_size(c.oid)) AS size,
    c.reltuples::bigint AS est_tuples,
    c.relpages AS pages
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
WHERE c.relkind = 'i'
  AND n.nspname = 'public'
ORDER BY pg_relation_size(c.oid) DESC;
"

log_step "2) 最终膨胀报告:"
run_sql_v "${TESTDB}" "
SELECT
    schemaname,
    indexname,
    pg_size_pretty(current_bytes) AS current_size,
    round((estimated_bloat_ratio * 100)::numeric, 1) || '%' AS bloat_pct,
    pg_size_pretty(estimated_bloat_bytes) AS bloat_size
FROM pg_auto_reindex_bloat_report()
ORDER BY estimated_bloat_bytes DESC;
"

log_step "3) Worker 最终状态:"
run_sql_v "${TESTDB}" "SELECT * FROM pg_auto_reindex_status();"

log_step "4) 自动重建完整历史:"
run_sql_v "${TESTDB}" "
SELECT
    id,
    schemaname,
    indexname,
    pg_size_pretty(bytes_before) AS before_size,
    pg_size_pretty(bytes_after) AS after_size,
    pg_size_pretty(bytes_saved) AS saved,
    round(extract(epoch from (end_time - start_time))::numeric, 2) || 's' AS duration,
    status
FROM pg_auto_reindex_history
ORDER BY id;
"

log_step "5) EWMA 学习矩阵 (有样本的时段):"
run_sql_v "${TESTDB}" "
SELECT
    slot_id,
    CASE day_of_week
        WHEN 0 THEN 'Sun' WHEN 1 THEN 'Mon' WHEN 2 THEN 'Tue'
        WHEN 3 THEN 'Wed' WHEN 4 THEN 'Thu' WHEN 5 THEN 'Fri'
        WHEN 6 THEN 'Sat'
    END AS day,
    hour_of_day || ':00' AS hour,
    round(ewma_loadavg::numeric, 3) AS load_avg,
    round(ewma_active_backends::numeric, 1) AS backends,
    sample_count,
    CASE WHEN is_current_slot THEN '◀' ELSE '' END AS current
FROM pg_auto_reindex_stats()
WHERE sample_count > 0
ORDER BY slot_id;
"

log_step "6) 表数据量统计:"
run_sql_v "${TESTDB}" "
SELECT
    relname AS table_name,
    pg_size_pretty(pg_total_relation_size(oid)) AS total_size,
    pg_size_pretty(pg_relation_size(oid)) AS table_size,
    pg_size_pretty(pg_indexes_size(oid)) AS indexes_size,
    reltuples::bigint AS est_rows
FROM pg_class
WHERE relkind = 'r' AND relnamespace = 'public'::regnamespace
ORDER BY pg_total_relation_size(oid) DESC;
"

# ========================== 汇总结论 ==========================
FINAL_REINDEX_COUNT=$(run_sql "${TESTDB}" "SELECT count(*) FROM pg_auto_reindex_history;" 2>/dev/null || echo "0")
FINAL_BYTES_SAVED=$(run_sql "${TESTDB}" "SELECT coalesce(sum(bytes_saved), 0) FROM pg_auto_reindex_history WHERE upper(status) = 'SUCCESS';" 2>/dev/null || echo "0")
FINAL_BYTES_PRETTY=$(run_sql "${TESTDB}" "SELECT pg_size_pretty(${FINAL_BYTES_SAVED}::bigint);" 2>/dev/null || echo "0 bytes")
FINAL_SUCCESS=$(run_sql "${TESTDB}" "SELECT count(*) FROM pg_auto_reindex_history WHERE upper(status) = 'SUCCESS';" 2>/dev/null || echo "0")
FINAL_FAILED=$(run_sql "${TESTDB}" "SELECT count(*) FROM pg_auto_reindex_history WHERE upper(status) != 'SUCCESS';" 2>/dev/null || echo "0")
EWMA_SLOTS_WITH_DATA=$(run_sql "${TESTDB}" "SELECT count(*) FROM pg_auto_reindex_stats() WHERE sample_count > 0;" 2>/dev/null || echo "0")

echo ""
log_section "测试结论"
echo -e "  ${BOLD}运行时长:${NC}          ${DURATION_MINUTES} 分钟"
echo -e "  ${BOLD}膨胀制造轮次:${NC}      ${BLOAT_ROUNDS} 轮"
echo -e "  ${BOLD}监控迭代次数:${NC}      ${ITERATION} 次"
echo -e "  ${BOLD}EWMA 有数据时段:${NC}   ${EWMA_SLOTS_WITH_DATA} / 168"
echo -e "  ${BOLD}Reindex 总执行次数:${NC} ${FINAL_REINDEX_COUNT} 次"
echo -e "  ${BOLD}  - 成功:${NC}           ${FINAL_SUCCESS} 次"
echo -e "  ${BOLD}  - 失败:${NC}           ${FINAL_FAILED} 次"
echo -e "  ${BOLD}总节省空间:${NC}         ${FINAL_BYTES_PRETTY}"
echo -e "  ${BOLD}监控日志:${NC}           ${MONITOR_LOG}"
echo ""

# 判断测试是否通过
if [ "${FINAL_REINDEX_COUNT}" -gt 0 ]; then
    echo -e "  ${GREEN}${BOLD}✅ 测试通过: pg_auto_reindex 在并发负载下正确执行了索引自动重建${NC}"
else
    echo -e "  ${YELLOW}${BOLD}⚠️  未检测到自动重建记录 (可能因膨胀未达到阈值或运行时间太短)${NC}"
    echo -e "  ${YELLOW}    建议增加 --duration 或 --bloat-rounds${NC}"
fi

echo ""
log_info "完成! 日志保存在: ${LOG_DIR}"

# 恢复默认配置
run_sql "${TESTDB}" "ALTER SYSTEM RESET pg_auto_reindex.min_bloat_ratio;" 2>/dev/null || true
run_sql "${TESTDB}" "ALTER SYSTEM RESET pg_auto_reindex.min_bloat_bytes;" 2>/dev/null || true
run_sql "${TESTDB}" "SELECT pg_reload_conf();" 2>/dev/null || true
