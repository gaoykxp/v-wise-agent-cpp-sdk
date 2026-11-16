#!/usr/bin/env bash
# 链路劣化注入（RPi 侧执行，需 sudo）：
#   ./dsda_netem.sh eth0 50 5 10   → eth0 出方向 时延50ms±5ms 抖动, 5% 丢包
#   ./dsda_netem.sh eth0 clear     → 清除 eth0 的注入
# 说明：netem 作用于 egress（模拟蜂窝上行劣化）；与双发选收测试配合使用。
set -u
IF="${1:?usage: dsda_netem.sh <ifname> [delay_ms [jitter_ms [loss_pct]]] | clear]}"
CMD="${2:-}"

if [ "$CMD" = "clear" ]; then
    sudo tc qdisc del dev "$IF" root 2>/dev/null
    echo "cleared netem on $IF"
    exit 0
fi

DELAY="${2:-50}"
JITTER="${3:-10}"
LOSS="${4:-5}"

sudo tc qdisc del dev "$IF" root 2>/dev/null
sudo tc qdisc add dev "$IF" root netem delay "${DELAY}ms" "${JITTER}ms" loss "${LOSS}%"
echo "netem on $IF: delay=${DELAY}ms jitter=${JITTER}ms loss=${LOSS}%"
tc qdisc show dev "$IF"
