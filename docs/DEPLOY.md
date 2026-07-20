# StackChan Relay 部署手册(Ubuntu 24.04 VPS)

前提:VPS一台(离你近的机房)、域名一个、DNS A记录(DNS only/灰云)指向VPS IP。
以下命令在VPS上执行。

## 1. 装依赖
    apt update && apt install -y python3-venv caddy

## 2. 部署服务
    mkdir -p /opt/stackchan-relay && cd /opt/stackchan-relay
    # 上传 relay/server.py 和 requirements.txt(Mac: scp server.py requirements.txt root@<VPS_IP>:/opt/stackchan-relay/)
    python3 -m venv .venv
    .venv/bin/pip install -r requirements.txt

## 2.1 下载Piper英式英语声音
    mkdir -p /opt/stackchan-relay/voices
    /opt/stackchan-relay/.venv/bin/python -m piper.download_voices \
      --data-dir /opt/stackchan-relay/voices en_GB-vctk-medium

仓库默认使用VCTK的`p275`（speaker ID 55）。模型在relay启动时加载一次，
后续每句话复用同一个ONNX session。`PIPER_LENGTH_SCALE=1.0`为正常语速；
数值越小越快，越大越慢。

## 3. 生成token(先存密码管理器,再使用!)
    openssl rand -hex 24

## 4. systemd托管
把 deploy/stackchan-relay.service 里的 <TOKEN> 换成上一步的值,然后:
    cp stackchan-relay.service /etc/systemd/system/
    systemctl daemon-reload && systemctl enable --now stackchan-relay
    systemctl status stackchan-relay   # 应为 active (running)

## 5. Caddy反代+自动HTTPS
把 deploy/Caddyfile 里的域名换成你的,然后:
    cp Caddyfile /etc/caddy/Caddyfile
    systemctl reload caddy
注意:Caddyfile里的 header_up Host 那行是必须的——MCP SDK有DNS重绑定防护,
反代不改Host会返回 421 Invalid Host header。

## 6. 验收
    curl https://你的域名/healthz
    # 期望: {"ok":true,"robot_online":false}
    journalctl -u stackchan-relay -n 30 --no-pager
    # 应看到: Loaded Piper model ..., speaker p275 (55) ...

## 7. 挂进Claude.ai
Settings → Connectors → Add custom connector → https://你的域名/mcp
保存后新开对话,五个工具出现即成功。

## 8. 安全备忘
- token只存两处:VPS的service文件、ESP32固件配置。不进任何聊天记录。
- 粘贴token后务必检查行尾无多余字符(一个'>'能让你排障一晚上)。
- VPS防火墙只放行22和443。

## 9. 运维
    journalctl -u stackchan-relay -f
    systemctl restart stackchan-relay
