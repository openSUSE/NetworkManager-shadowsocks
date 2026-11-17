# 默认大陆 IPv4 / IPv6 网段库

这些文件由 Python 标准库工具 `nm-shadowsocks-update-routes` 从 APNIC 官方 delegated 统计生成，主插件保持 C 实现。

- 来源：https://ftp.apnic.net/stats/apnic/delegated-apnic-latest
- 规则：registry=apnic、country=CN、type=ipv4/ipv6、status=allocated/assigned；不包含 HK、MO、TW 记录。
- IPv4 的地址数量按实际起始地址拆分为 CIDR，支持非 2 的幂；IPv6 value 作为前缀长度。
- china-ipv4.cidr / china-ipv6.cidr：独立地址族文件；china.cidr：插件使用的合并文件。
- china.json：APNIC 版本、各族网段数量及合并文件 SHA-256。哈希用于一致性核对，不是独立数字签名。
- APNIC 分配国家信息不保证 IP 的实际使用位置，也不是实时 BGP 路由库。

APNIC 允许下载和使用其统计，但不担保准确性或对使用结果负责。条件与字段定义：https://ftp.apnic.net/stats/apnic/README.TXT

重新生成仓库快照：

```sh
build/tools/nm-shadowsocks-update-routes --output data/china
# 或使用已下载统计（不联网）：
build/tools/nm-shadowsocks-update-routes --source delegated-apnic-latest --output data/china
```

构建和安装均不会自动联网。正常用户运行更新器写入自己的数据目录；管理员运行时写入系统状态目录。插件按用户更新库、系统更新库、内置快照顺序读取。已有连接使用其保存的网段快照，更新后需在编辑器选择“使用最新默认网段”、保存并重连。

更新器还支持 `--region JP,SG`、`--registry arin|ripencc|lacnic|afrinic|all` 和 `--family ipv4|ipv6|dual`。自定义结果必须显式指定输出目录，按区域代码生成独立文件，不替换本目录的默认 CN 快照；例如 `jp-sg.cidr`。多注册局结果在 JSON 元数据中分别记录每个源的版本。用法见项目 README 的“自定义区域”。
