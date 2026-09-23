#!/bin/bash
# ==============================================================================
# V-Wise SDK 树莓派上电自启动安装脚本（只需以 root 运行一次）
#
# 用法（在树莓派上）：
#   sudo bash install_autostart.sh
#
# 效果：
#   1. 安装 systemd 服务 quectel-cm.service
#      —— 后台常驻拨号：sudo ./quectel-CM -s cmnet，异常退出自动拉起
#   2. 安装 systemd 服务 v-wise-agent.service
#      —— 等待蜂窝网络接口拿到 IP 后启动 SDK（v_wise_agent），异常退出自动拉起
#   3. 两个服务均已 enable，开机自动启动；立即启动无需重启
#
# 卸载：
#   sudo systemctl disable --now quectel-cm v-wise-agent
#   sudo rm /etc/systemd/system/quectel-cm.service /etc/systemd/system/v-wise-agent.service
#   sudo systemctl daemon-reload
# ==============================================================================
set -e

# ---------------- 配置区（按实际环境修改） ----------------
CM_BIN="/home/pi/Quectel_QConnectManager_Linux_V1.6.8/build/quectel-CM"
CM_ARGS="-s cmnet"
CM_DIR="$(dirname "$CM_BIN")"

AGENT_BIN="/home/pi/v-wise-agent-cpp-sdk/build/v_wise_agent"
AGENT_DIR="/home/pi/v-wise-agent-cpp-sdk/build"
AGENT_CONF_SRC="/home/pi/v-wise-agent-cpp-sdk/data/config.json"
AGENT_CONF_DST="/home/pi/v-wise-agent-cpp-sdk/build/conf/config.json"

# ---------------- 前置检查 ----------------
if [ "$(id -u)" -ne 0 ]; then
    echo "[错误] 请用 sudo 运行：sudo bash $0"
    exit 1
fi

if [ ! -x "$CM_BIN" ]; then
    echo "[错误] 未找到拨号程序：$CM_BIN"
    exit 1
fi
if [ ! -x "$AGENT_BIN" ]; then
    echo "[错误] 未找到 SDK 可执行文件：$AGENT_BIN"
    exit 1
fi

# SDK 依赖 /conf/config.json（缺失则从工程 data 目录拷一份）
if [ ! -f "$AGENT_CONF_DST" ]; then
    mkdir -p /conf
    if [ -f "$AGENT_CONF_SRC" ]; then
        cp "$AGENT_CONF_SRC" "$AGENT_CONF_DST"
        echo "[信息] 已拷贝配置 $AGENT_CONF_SRC -> $AGENT_CONF_DST"
    else
        echo "[警告] 未找到 $AGENT_CONF_SRC，SDK 启动后请自行确认 $AGENT_CONF_DST"
    fi
fi

# ---------------- 安装 quectel-cm 服务 ----------------
echo "[安装] quectel-cm.service ..."
cat > /etc/systemd/system/quectel-cm.service <<EOF
[Unit]
Description=Quectel QConnectManager cellular dial-up (quectel-CM ${CM_ARGS})
After=dev-ttyUSB0.device
Wants=dev-ttyUSB0.device

[Service]
Type=simple
WorkingDirectory=${CM_DIR}
ExecStart=${CM_BIN} ${CM_ARGS}
Restart=always
RestartSec=5
# quectel-CM 前台常驻，日志进 journal：
#   journalctl -u quectel-cm -f
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

# ---------------- 安装 v-wise-agent 服务 ----------------
echo "[安装] v-wise-agent.service ..."
cat > /etc/systemd/system/v-wise-agent.service <<EOF
[Unit]
Description=V-Wise agent SDK (v_wise_agent)
# 依赖拨号进程；后者就绪 ≠ 接口已拿到 IP，由 ExecStartPre 轮询兜底
Requires=quectel-cm.service
After=quectel-cm.service

[Service]
Type=simple
WorkingDirectory=${AGENT_DIR}
# 等待蜂窝接口（wwan*/usb*）获得 IPv4，最多 120s；超时也继续启动（SDK 内部有重连）
ExecStartPre=/bin/bash -c 'for i in \$(seq 1 60); do ip -4 addr show | grep -E "wwan|usb" | grep -q "inet " && exit 0; sleep 2; done; echo "[警告] 等待网络 IP 超时，仍启动 SDK"; exit 0'
ExecStart=${AGENT_BIN}
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

# ---------------- 启用并立即启动 ----------------
systemctl daemon-reload
systemctl enable quectel-cm.service
systemctl enable v-wise-agent.service
systemctl restart quectel-cm.service
systemctl restart v-wise-agent.service

echo ""
echo "=============================================="
echo " 安装完成，两个服务已设为开机自启并已启动"
echo "=============================================="
echo ""
echo " 查看状态："
echo "   systemctl status quectel-cm v-wise-agent"
echo " 跟踪日志："
echo "   journalctl -u quectel-cm -f"
echo "   journalctl -u v-wise-agent -f"
echo ""
