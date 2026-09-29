#!/bin/sh
#
# run_gateway.sh —— C 网关（c_gateway）启动 / 停止 / 重启 / 状态 / 构建 脚本
# 兼容 POSIX sh（dash / bash 均可运行）。
#
# 说明：c_gateway 自身会 daemon 化（fork 到后台），因此本脚本直接执行二进制即可，
#       并通过 pgrep 匹配「二进制路径 + 配置文件路径」来定位真正的守护进程 PID。
#
# 用法：
#   ./run_gateway.sh start      # 启动网关（默认配置 gateway_config.json）
#   ./run_gateway.sh stop       # 停止网关
#   ./run_gateway.sh restart    # 重启
#   ./run_gateway.sh status     # 查看运行状态 + 健康检查端口探测
#   ./run_gateway.sh build      # 编译（若 bin/c_gateway 不存在会先自动编译）
#
# 可覆盖的环境变量：
#   GATEWAY_CONFIG   配置文件路径（默认：仓库根目录/gateway_config.json；HTTPS 用 gateway_config_https.json）
#   GATEWAY_BIN      二进制路径（默认：仓库根目录/bin/c_gateway）
#
set -u

# ---- 路径解析（脚本所在目录即仓库根目录；POSIX 下用 $0 而非 BASH_SOURCE）----
SCRIPT_DIR=$(cd "$(dirname "$0")" 2>/dev/null && pwd)
ROOT_DIR="$SCRIPT_DIR"

BIN="${GATEWAY_BIN:-$ROOT_DIR/bin/c_gateway}"
CONFIG="${GATEWAY_CONFIG:-$ROOT_DIR/gateway_config.json}"
PIDDIR="$ROOT_DIR/run"
PIDFILE="$PIDDIR/gateway.pid"
LOGDIR="$ROOT_DIR/logs"
LOGFILE="$LOGDIR/gateway.out"

# ---- 工具函数 ----
log() { echo "[run_gateway] $*"; }
die() { echo "[run_gateway] ERROR: $*" >&2; exit 1; }

# 把路径规范化为绝对路径，便于 pgrep 精确匹配进程命令行
canon() {
    p="$1"
    case "$p" in
        /*) ( cd "$(dirname "$p")" 2>/dev/null && echo "$(pwd)/$(basename "$p")" ) || echo "$p" ;;
        *)  echo "$(cd "$ROOT_DIR" && pwd)/$p" ;;
    esac
}
BIN=$(canon "$BIN")
CONFIG=$(canon "$CONFIG")

# 转义正则特殊字符，用于 pgrep -f 精确匹配
esc() { printf '%s' "$1" | sed 's/[][\.*^$/]/\\&/g'; }
PG_PATTERN="^$(esc "$BIN") $(esc "$CONFIG")\$"

# 返回匹配到的真实守护进程 PID（可能多行）
gw_pids() { pgrep -f "$PG_PATTERN" 2>/dev/null; }

is_running() {
    p=$(gw_pids)
    [ -n "$p" ]
}

do_build() {
    if [ -x "$BIN" ]; then
        log "二进制已存在：$BIN（如需重新编译请先 make clean）"
        return 0
    fi
    log "编译网关：$BIN"
    ( cd "$ROOT_DIR" && make ) || die "编译失败，请查看上面的错误输出"
}

detect_port() {
    grep -o '"service_port"[[:space:]]*:[[:space:]]*[0-9]*' "$CONFIG" 2>/dev/null \
        | grep -o '[0-9]*' | tail -1
}

do_start() {
    if is_running; then
        log "网关已在运行 (pid=$(gw_pids | head -1))"
        return 0
    fi

    [ -x "$BIN" ] || do_build
    [ -f "$CONFIG" ] || die "配置文件不存在：$CONFIG"

    mkdir -p "$PIDDIR" "$LOGDIR"
    log "启动网关："
    log "  二进制   = $BIN"
    log "  配置文件 = $CONFIG"
    log "  日志     = logs/gateway.log"

    # nohup 让网关忽略 SIGHUP；& 后台运行。c_gateway 自身会 fork+setsid 彻底脱离会话。
    nohup "$BIN" "$CONFIG" >>"$LOGFILE" 2>&1 < /dev/null &

    # 轮询等待守护进程出现（最多 ~3s）
    i=0
    pid=""
    while [ "$i" -lt 30 ]; do
        pid=$(gw_pids | head -1)
        [ -n "$pid" ] && break
        sleep 0.1
        i=$((i + 1))
    done

    if [ -z "$pid" ]; then
        log "警告：启动后未检测到守护进程，请查看日志 logs/gateway.log"
        return 1
    fi

    echo "$pid" >"$PIDFILE"
    port=$(detect_port)
    if [ -n "$port" ] && curl -s -o /dev/null -m 2 "http://127.0.0.1:${port}/health" 2>/dev/null; then
        log "网关已启动 (pid=$pid)，监听 ${port}，健康检查 OK"
    else
        log "网关已启动 (pid=$pid)（端口未就绪或 /health 未响应，请查看日志）"
    fi
}

do_stop() {
    pids=$(gw_pids)
    if [ -z "$pids" ]; then
        log "网关未运行"
        rm -f "$PIDFILE"
        return 0
    fi
    log "停止网关 (pid(s): $(echo "$pids" | tr '\n' ' ')) ..."
    for p in $pids; do
        kill "$p" 2>/dev/null
    done
    # 同时处理 pidfile 中记录的 pid（避免不一致）
    if [ -f "$PIDFILE" ]; then
        pf=$(cat "$PIDFILE" 2>/dev/null)
        [ -n "$pf" ] && kill "$pf" 2>/dev/null
    fi
    # 最多等待 5s 优雅退出
    i=0
    while [ "$i" -lt 50 ]; do
        is_running || break
        sleep 0.1
        i=$((i + 1))
    done
    if is_running; then
        log "未响应，强制终止 (SIGKILL)"
        for p in $(gw_pids); do kill -9 "$p" 2>/dev/null; done
    fi
    rm -f "$PIDFILE"
    log "网关已停止"
}

do_status() {
    if is_running; then
        log "运行中 (pid=$(gw_pids | head -1))"
        port=$(detect_port)
        if [ -n "$port" ]; then
            if curl -s -o /dev/null -m 2 "http://127.0.0.1:${port}/health" 2>/dev/null; then
                log "HTTP 探测 http://127.0.0.1:${port}/health -> OK"
            else
                log "HTTP 探测 http://127.0.0.1:${port}/health -> 无响应"
            fi
        fi
        return 0
    fi
    log "未运行"
    return 1
}

usage() {
    cat <<EOF
用法: $0 {start|stop|restart|status|build}

  start    启动网关（默认配置 gateway_config.json）
  stop     停止网关
  restart  重启网关
  status   查看运行状态并探测健康检查端口
  build    编译网关（若 bin/c_gateway 不存在）

环境变量：
  GATEWAY_CONFIG   配置文件路径（默认 $ROOT_DIR/gateway_config.json）
  GATEWAY_BIN      二进制路径（默认 $ROOT_DIR/bin/c_gateway）
EOF
}

case "${1:-}" in
    start)   do_start ;;
    stop)    do_stop ;;
    restart) do_stop; do_start ;;
    status)  do_status ;;
    build)   do_build ;;
    ""|-h|--help|help) usage ;;
    *) die "未知命令: $1（使用 $0 help 查看用法）" ;;
esac
