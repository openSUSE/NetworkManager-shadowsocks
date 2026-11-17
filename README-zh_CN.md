# NetworkManager Shadowsocks

[English](README.md)

基于 **C / Meson / libnm / GTK 3 + GTK 4** 的 Linux NetworkManager VPN 插件，以 **shadowsocks-rust 的 sslocal TUN** 为后端。


# 功能
- 手动编辑节点，或导入标准 Shadowsocks JSON、shadowsocks-rust 多节点 JSON、SIP008 JSON；多节点文件可选择索引。
- **TCP / UDP 分别使用独立节点**，也可共用节点。两个节点均需要支持其对应协议。
- 全局代理、局域网例外、区域 IP（默认为中国大陆 IP）+ 局域网例外；内置大陆 IPv4/IPv6 网段库，支持更新和额外直连 CIDR。
- 可修改 DNS，UDP DNS 走 UDP 节点，TCP DNS 走 TCP 节点。配置的 DNS IP 始终经代理。
- IPv4、IPv6、双栈；单地址族模式阻止另一地址族的非例外 TCP/UDP 流量。
- 原生 `NMVpnEditorPlugin` / `NMVpnEditor`，支持 `nm-connection-editor` 和 `nmcli connection import`。

## 构建与安装

openSUSE 的 rpm spec 模板文件位于 [rpm/NetworkManager-shadowsocks.spec](rpm/NetworkManager-shadowsocks.spec)，源码归档和构建说明见 [rpm/README.md](rpm/README.md)。其他采用 rpm 包管理的发行版可以参考这个模板文件。

依赖：C 编译器、Meson ≥ 0.61、Ninja、gettext 工具、Python ≥ 3.9、libnm ≥ 1.20 开发包、GLib/GIO ≥ 2.66、JSON-GLib ≥ 1.6、GTK 3 ≥ 3.22 和 GTK 4 ≥ 4.0 开发包。插件运行需要 Linux TUN、iproute2（支持 `ip rule ipproto`）、NetworkManager、编译启用 `local-tun` 的 shadowsocks-rust `sslocal`；主插件仍为 C，只有独立网段更新工具和其测试使用 Python 标准库。

```sh
meson setup build --prefix=/usr
meson compile -C build
meson test -C build --print-errorlogs
sudo meson install -C build
```

## 使用

运行 `nm-connection-editor`，新增 VPN → **Shadowsocks**。分别填写 TCP、UDP 节点，或者勾选“UDP 共用 TCP 节点”。在各节点页选择索引后可导入 JSON。指定模式、地址族、DNS；选择区域例外模式即可使用默认中国大陆双栈网段库，无需自己寻找列表。界面显示 IPv4/IPv6 网段数量，也可手动导入自定义 CIDR。选定内容保存在连接内，激活时不读取原始文件。

设置界面会出现无效的 ipv4/ipv6 地址设置的配置选项，这是 NetworkManager 为 VPN 配置的通用界面，无法屏蔽，无视即可。

命令行导入独立节点：

```sh
nm-shadowsocks import --name '我的透明代理' \
  --tcp tcp-node.json --udp udp-node.json \
  --mode lan --ip-mode dual --dns '1.1.1.1,2606:4700:4700::1111'
nm-shadowsocks list
nmcli connection up uuid '<上一步输出的 UUID>'
nmcli connection down uuid '<UUID>'
```

省略 `--udp` 时共用 TCP 节点；`--tcp-index` / `--udp-index` 从 0 开始。修改已保存连接可加 `--uuid UUID`，或在原生编辑器中修改。

```sh
# 只在本地生成配置，不写入 NetworkManager；文件包含密码，权限 0600。
nm-shadowsocks import --tcp tcp.json --udp udp.json --output my-profile.json
# 离线校验，打印路由计划；均不需要 root，不修改网络。
nm-shadowsocks validate my-profile.json
nm-shadowsocks plan my-profile.json
# 保存完整配置到 NetworkManager。
nm-shadowsocks apply my-profile.json
# 也可使用原生导入接口；支持完整配置或单节点文件。
nmcli connection import type shadowsocks file my-profile.json
```

区域例外（默认为中国大陆 IP）：在 import 时加 `--mode china` 即可自动加载默认库；`--china-cidrs FILE` 用于自定义覆盖。完整随包数据在 `data/china/china-ipv4.cidr`、`china-ipv6.cidr` 和合并的 `china.cidr`，当前快照 APNIC 版本为 **20261113**，包含 **8798 条 IPv4、2048 条 IPv6**。`examples/regional-example.cidr` 仍只用于格式演示。

更新网段库（仅更新数据，不修改系统路由）：

```sh
nm-shadowsocks-update-routes          # 当前用户更新库
sudo nm-shadowsocks-update-routes     # 系统更新库，供所有用户使用
# 离线转换或更新源码中的随包快照：
nm-shadowsocks-update-routes --source delegated-apnic-latest --output data/china
```

更新器使用 Python 标准库 `urllib.request` 和 `ipaddress`，通过 HTTPS 从 APNIC 官方下载，不需要 curl 或 pip 依赖。它校验 CN 双栈记录后原子替换合并库，下载或解析失败保留旧数据。默认优先级为用户更新库 → 系统更新库 → 安装的内置快照；构建、安装和 VPN 激活均不自动联网。已有连接保留保存时的网段，在编辑器点击“使用最新默认网段”、保存并重连后应用新库。只配置 IPv4 代理时使用 `--ip-mode ipv4 --dns 1.1.1.1`，IPv6 DNS 同理。

网段依据 APNIC 的 `CN` 分配信息，排除 HK/MO/TW；这是国家分配数据，不保证实际地理位置，也不是实时 BGP 路由。数据来源、许可条件和元数据见 [网段库说明](data/china/README.md)。

### 自定义区域

更新工具支持 `--region`（别名 `--regions`）选择两字母国家/地区代码，可逗号分隔或重复指定；默认仍为 `CN`。自定义区域、地址族或注册局时必须指定 `--output`，避免覆盖默认大陆库。

```sh
# Japan and Singapore, using APNIC records.
nm-shadowsocks-update-routes --region JP,SG --output ./regional-routes
# US allocations held by ARIN.
nm-shadowsocks-update-routes --region US --registry arin --output ./us-routes
# Merge all five RIRs for the selected regions.
nm-shadowsocks-update-routes --region CN,HK,MO --registry all --output ./regional-routes
# Export only IPv4; useful when a region has no IPv6 allocations.
nm-shadowsocks-update-routes --region JP --family ipv4 --output ./jp-routes
```

`--registry` 支持 `apnic`（默认）、`arin`、`ripencc`、`lacnic`、`afrinic` 和 `all`。单一数据源只覆盖该注册局报告的分配；如需合并不同注册局中的同一国家/地区记录，使用 `all`。离线时可重复传入 `--source FILE`，从文件头识别注册局（不指定 `--registry` 或指定 `all` 时），只转换提供的文件；重复注册局、无匹配区域、不完整数据或所需地址族缺失时拒绝更新。`--family` 可选 `dual`（默认）、`ipv4`、`ipv6`。自定义结果会合并相邻网段，并校验最终网段数量上限。

例如 `JP,SG` 生成 `jp-sg.cidr`、`jp-sg-ipv4.cidr`、`jp-sg-ipv6.cidr`、`jp-sg.json`，元数据记录所选区域、地址族、各注册局版本与校验和。代码顺序不影响文件名，统一按字母排序。默认 CN/APNIC/双栈继续生成兼容的 `china.*` 文件；自定义 CN 多注册局输出 `cn.*`。生成文件不自动改变连接或系统路由，可在编辑器导入，或配置为额外直连网段：

```sh
nm-shadowsocks import --tcp node.json --mode lan --bypass-cidrs ./regional-routes/jp-sg.cidr
```

这里的效果是所选区域**直连**，其他非例外 TCP/UDP 经代理；不是“仅代理这些区域”。更新工具为 `nm-shadowsocks-update-routes`，默认更新 CN 网段。源码仍为英文，提示通过 PO 翻译。

## 行为与边界

- 代理本机 TCP/UDP 流量，包含 UDP/QUIC；这不是路由器/网关配置工具。不自动开启转发，不更改防火墙，不代理原始 ICMP 等协议，不能用 ping 判断节点可用性。
- “全局”仍保留 loopback、链路本地、多播等系统必要地址；“局域网”额外排除私网和 CGNAT；“区域”再排除所选区域 CIDR（默认为中国大陆 IP）。
- 节点域名在建立路由前使用原网络解析，后端配置固定解析结果；节点 IP 变化或原网络变化后请重连。底层传输地址族取决于节点和物理网络，与代理目标地址族独立。
- DNS 地址直接通过 NetworkManager 发布，优先级为 -5353、路由域为 `~.`。不启动本地 DNS 转发器，也不监听本地 53 端口。一个 TUN 同时接收 TCP/UDP，后端的两条服务器记录分别限制为 `tcp_only` / `udp_only`，系统解析器绑定 VPN 接口时也能选用对应节点。应用内置 DoH 仍按普通 TCP/UDP 规则处理，不能由此强制应用使用所配置 DNS。
- 密码保存为 NetworkManager VPN secrets（系统持有，支持无人值守），不写入 `vpn.data`；运行时配置置于 `/run` 的 0700 目录、0600 文件中。导出文件含密码，拒绝覆盖现有文件或符号链接。提供 GNOME VPN 认证辅助程序，接收 NetworkManager 传入的已保存密码；需要交互时由 GNOME Shell 显示密码对话框。
- 仅支持所列 AEAD / Shadowsocks 2022 加密方式。暂不支持 `ss://`、订阅下载、JSON5。导入时不会继承文件里的监听器或 ACL。
- 同时只能激活一个本插件连接。保留接口名 `nmss-tun`、路由表 51820/51821、规则优先级 10500/10501/10502/10503/10510/10511、SO_MARK 0x5353；占用时拒绝连接。自定义策略路由/其他 VPN 若有更高优先级规则，可能改变实际路径，需管理员协调。
- 正常断开、启动失败和 SIGTERM 会回滚路由并终止后端。**无持久 kill switch**，断开后恢复原网络；服务 SIGKILL/崩溃的残留策略需管理员清理，参见 DESIGN.md。启动成功表示本地通道建立，不代表远端密码或网络可用性已验证。

实现说明、分流顺序及验收清单见 [DESIGN.md](DESIGN.md)。

### GNOME Settings editor compatibility

编辑器入口不链接 GTK，按宿主进程选择独立的 GTK 3 或 GTK 4 界面模块。现代 GNOME 设置使用 GTK 4，`nm-connection-editor` 使用 GTK 3；两者不会在同一进程加载另一代 GTK。升级旧版时需同时安装入口及两个界面模块，并完全退出、重新打开设置窗口。 认证辅助程序和 VPN 描述文件也须同步更新；缺少认证入口会导致 GNOME Shell 密码请求失败或超时。安装后若 GNOME Shell 仍缓存旧描述文件，需要注销并重新登录。

### SIP003 obfuscation plugins

GTK 3/4 编辑器的 TCP、UDP 节点分别提供混淆插件、插件选项、额外参数（JSON 数组）。插件留空表示禁用；例如 `obfs-local` 配合 `obfs=http;obfs-host=www.example.com`，或 `v2ray-plugin` 配合服务端要求的选项。可导入节点 JSON 中的 `plugin`、`plugin_opts`、`plugin_args`，CLI 的节点导入同样支持。插件传输协议可选择仅 TCP、仅 UDP 或 TCP 和 UDP，并按所选值传给后端；未指定时默认仅 TCP。仅在插件支持 UDP 入口时选择包含 UDP 的模式。

```json
{
  "server": "ss.example.com",
  "server_port": 443,
  "method": "aes-256-gcm",
  "password": "replace-me",
  "plugin": "obfs-local",
  "plugin_opts": "obfs=tls;obfs-host=www.example.com",
  "plugin_args": [],
  "plugin_mode": "tcp_only"
}
```

插件传输模式为 `tcp_only` 时，UDP 节点仍代理应用的 UDP 数据报，但通过节点的原生 Shadowsocks UDP 端口转发，不交给混淆插件；`mode=quic` 指插件自身的外层传输，不改变这一行为。插件可执行文件需另行安装，后端需启用 SIP003 支持。插件选项和额外参数随连接配置保存，以便编辑器在未加载 secrets 时也能恢复；这些字段中的令牌等敏感内容将具有与连接配置相同的可见性。节点密码仍作为 NetworkManager secrets 保存；导出 JSON 包含这些值。

服务允许系统安装的 `obfs-local`、`v2ray-plugin`、`xray-plugin`、`kcptun-client`、`shadow-tls`，按名称查找 `/usr/bin` 和 `/usr/local/bin`，也可填写绝对路径。自定义插件需由管理员在 `/usr/libexec/NetworkManager/shadowsocks/plugins/` 放置可执行文件或符号链接，并填写其名称或该目录下的完整路径。程序及路径必须属于 root，且不能被组或其他用户写入；服务不搜索用户 PATH，也不通过 shell 执行参数。

插件服务器在启动前解析地址，实际使用的 IPv4/IPv6 地址自动直连，避免插件自身流量循环进入 TUN；该地址上的其他 TCP/UDP 流量也直连。上游 DNS 不能与插件服务器使用同一地址。TLS/WebSocket 等依赖域名的插件请显式配置其 host/SNI 选项，因为后端接收的是启动时解析的服务器 IP。断开时清理整个后端进程组，包括插件子进程。

