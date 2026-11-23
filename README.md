# Tesla FSD Controller — ESP32 / ESP32-S3 Wi-Fi 桥接版


## 项目简介

本固件让 ESP32 / ESP32-S3 同时扮演两个角色：

1. **Wi-Fi 桥接**：ESP32 连接手机热点或家用路由（STA），再创建一个供车机连接的 AP；车机流量经 ESP32 NAT 转发。
2. **DNS 过滤 + FSD 下发**：车机使用 ESP32 下发的 DNS。固件按白名单 / 黑名单放行或拦截域名，并可把 `api-prd.vn.cloud.tesla.cn` 的 `/feature_configuration` 请求劫持到本地，返回自定义 FSD 配置。

所有设置都通过浏览器完成，无需每次改代码重新烧录。

<p align="center">
  <img src="images/screenshot_zh.jpg" width="70%" alt="Web 控制面板">
</p>

---

## 默认值

| 项目 | 默认值 |
|---|---|
| Wi-Fi 热点名称（SSID） | `fsd-esp32` |
| Wi-Fi 密码 | `058005800580` |
| 设备 AP 地址 | `9.9.9.9` |
| 管理面板 | `http://9.9.9.9:10010/` |
| mDNS | `http://fsd.local:10010/` |
| DNS 过滤 | 首次启动默认开启 |
| 默认白名单 | 40 条基础 / 地图 / 音乐 / 豆包域名（见下文） |
| 默认黑名单 | `api-prd.vn.cloud.tesla.cn` + `mothership.vn.teslamotors.com` / `mothership.vn.cloud.tesla.cn` |
| FSD 下发 | 默认关闭（可在面板一键开启，`reply` 模式，443/4567 按需启停） |

> 首次启动、恢复出厂或执行 `erase_flash` 后，设备使用上述默认值。之后在面板里改过的配置会保存在 NVS，重启不会丢失。

---

## 固件选择

每个环境生成两个文件：

- **`<env>_v1.4.35.bin`**：合并固件，含 bootloader + 分区表 + app，首次刷机 / 清空重刷时从 `0x0` 写入。
- **`<env>_v1.4.35_ota.bin`**：仅 app，供 Web 面板 OTA 或在线升级使用。

完整资产见 [Releases](../../releases/latest)。

| 环境 | 板子 / 用途 | 合并固件 | 说明 |
|---|---|---|---|
| `esp32-wifi` | 标准 ESP32 Wi-Fi 桥接 | `esp32_wifi_v1.4.35.bin` | 纯 Wi-Fi 桥接 + DNS 过滤，精简管理 UI，无 443 FSD 下发 |
| **`esp32s3-waveshare-wifi`** | **Waveshare ESP32-S3 Wi-Fi 桥接** | **`esp32s3_waveshare_wifi_v1.4.35.bin`** | **主力固件：Wi-Fi 桥接 + DNS 过滤 + FSD 下发（TLS 1.3）** |

> 仓库已完全移除历史 CAN 注入、TWAI 驱动、BMS/灯光/空调/GPS/性能遥测、服务模式及对应大体积页面，所有构建均专注 Wi-Fi 桥接、DNS 过滤、OTA 与日志（ESP32-S3 版另含按需启停的 FSD 下发与 client-config）。

---

## 快速开始

### 1. 刷入固件

**方式 A：浏览器（推荐首次使用）**

1. 使用 Chrome 或 Edge 打开 [ESP Web Flasher](https://esp.huhn.me/)。
2. USB 连接板子，点击 **Connect** 选择串口。
3. 选择 `esp32s3_waveshare_wifi_v1.4.35.bin`，地址填 `0x0`。
4. 点击 **Program**，等待完成。

**方式 B：esptool（清空重刷）**

```sh
# 先彻底清空 NVS / 分区 / bootloader
python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem21201 erase_flash

# 再写入合并固件
python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem21201 \
  --baud 921600 write_flash 0x0 esp32s3_waveshare_wifi_v1.4.35.bin
```

Windows 把串口换成 `COMx`。ESP32-S3 进入下载模式：按住 **BOOT**，点一下 **RST**，松开 RST 后再松开 BOOT。

### 2. 连接设备

1. 手机或电脑连接 Wi-Fi：`fsd-esp32`，密码 `058005800580`。
2. 浏览器打开 `http://9.9.9.9:10010/`（或 `http://fsd.local:10010/`）。
3. 如果设备没有自动弹出配置页，手动输入地址。

### 3. 配置上游 Wi-Fi

1. 在管理页找到「上游 Wi-Fi」。
2. 添加手机热点或家用路由的 SSID / 密码。
3. 启用上游，等待状态变成「已连接」。
4. 再让车机连接 `fsd-esp32`。

> 建议顺序：先配好上游和 DNS 规则，最后再让车机连入。直接让车机裸连公网，会让 Tesla 云端收到非官方网络流量，存在账号风控风险。

---

## Wi-Fi 桥接与 DNS 过滤

### 网络拓扑

```
手机热点 / 家用路由
        │  STA
        ▼
   ESP32 / ESP32-S3
    AP 9.9.9.9 + DNS :53
        │
        ▼
      车机
```

- AP 的 DHCP 会把 `9.9.9.9` 作为 DNS 下发给车机。
- 只有先经过 ESP32 DNS 的查询才能被黑白名单判断。
- 固件只配置 IPv4 AP、IPv4 DHCP DNS 和 IPv4 NAPT；不会给车机分配 IPv6，也不转发 IPv6。
- 上游未连接时，AP 仍可用，但车机无法访问公网。

### 默认白名单

首次启动会写入以下 40 条规则。固件内部用空格分隔，这里按类别换行，方便阅读和复制：

```text
connman.vn.cloud.tesla.cn
nav-prd-maps.tesla.cn
media-server-me.tesla.cn
maps-cn-prd.go.tesla.services
www.tesla.cn

volcengine.com
volces.com
volcengineapi.com
volccdn.com

baidu.com
bdimg.com
bcebos.com

qq.com
qlogo.cn
gtimg.cn
qpic.cn
tencentmusic.com
bdycdn.cn

163.com
126.com
126.net
netease.com
163jiasu.com
163yun.com

*.apple.com
mzstatic.com

doubao.com
doubao.cn
doubaoapps.com
doubaocdn.com
butterflyai.cn
butterfly-static.cn
picpic-ai.cn
pixeldance.com
byteimg.com
bytescm.com
bytednsdoc.com
pstatp.com
snssdk.com
bytedance.com
```

> 说明：`baidu.com` / `bdimg.com` / `bcebos.com` 覆盖百度地图与 CDN；`qq.com` 覆盖 QQ 音乐全部子域；`163.com` / `126.com` / `126.net` / `netease.com` 覆盖网易云音乐；`*.apple.com` 覆盖 Apple Music 子域，`mzstatic.com` 覆盖封面与静态资源；豆包及字节 CDN 均使用父域规则，天然覆盖其所有子域。

### 默认黑名单

```text
api-prd.vn.cloud.tesla.cn
mothership.vn.teslamotors.com
mothership.vn.cloud.tesla.cn
```

### 特殊域名

**`hermes-prd.vn.cloud.tesla.cn`（默认不放行）**

- 手动加入白名单后可以远程车控。
- 可能触发 Tesla 风控，甚至导致账号 / 车辆被封禁。
- 默认不加入白名单，请谨慎使用。

**`api-prd.vn.cloud.tesla.cn`（默认黑名单）**

- 车机恢复出厂设置后，可能遇到 **QQ 音乐无法显示扫码登录、网易云音乐异常** 等问题。
- 此时可先把 `api-prd.vn.cloud.tesla.cn` 加入白名单再试。黑白名单长度相同时**白名单优先**，因此不需要删除黑名单里的同名条目。
- 放行后 Tesla 云端可能改写 FSD 配置，需要自行承担风险。
- **音乐软件恢复后请立刻把它移回黑名单，或直接断开 Wi-Fi**，不要长期放行。
- FSD 下发的劫持判断排在 DNS 黑白名单之前：只要「FSD 下发」开启，即使 `api-prd` 在黑名单中，`/feature_configuration` 仍会被本地劫持。

**`mothership.vn.teslamotors.com` / `mothership.vn.cloud.tesla.cn`（默认外网黑名单 / 本地劫持）**

- 默认加入外网黑名单；FSD 下发关闭时，车机不能直连母舰，避免云端改写配置。
- FSD 下发开启时按 Windows TeslaDG 的逻辑劫持：`mothership.*` 与 `api-prd` 都解析到 `9.9.9.9`，443（TLS）和 4567（明文 HTTP）上的 `/feature_configuration` 由本地回包，其余路径按原始 Host 反代。
- 与 `api-prd` 一样，黑名单只影响劫持关闭后的直连，不影响 FSD 开启时的本地回包。

### 规则语义

- 规则按空格、换行、逗号或分号分隔。
- 域名会转小写、去掉首尾空白和末尾的点。
- 支持精确匹配和子域名后缀匹配：`tesla.cn` 匹配 `tesla.cn`、`a.b.tesla.cn`，不匹配 `not-tesla.cn`。
- 支持 `*.qq.com`：匹配任意层级子域，**不匹配 `qq.com` 本身**；要同时覆盖主域和子域，写 `qq.com *.qq.com`。
- 白名单 = 放行；黑名单 = 屏蔽。
- 同时命中时，**更长、更具体的后缀优先**（`*.` 不计长度）；长度相同白名单优先。
- 白名单非空时，未命中的域名默认拒绝（默认拒绝模式）。
- 白名单为空时，未命中黑名单的域名默认放行（只黑名单模式）。
- 白名单和黑名单旁各有「恢复默认」按钮：一键使用固件内置默认值并自动启用 DNS 过滤。对应 API：`/api/wifi-bridge/set?dnsReset=allow|block|all`。

### IP 层拦截（可选，默认关闭）

开启后，固件会周期性解析黑名单域名，维护最多 256 条 IPv4 缓存，并在 lwIP 转发层丢弃命中 IP 的包。

- 缓存默认保留 1 小时，完整刷新约 5 分钟。
- `*.qq.com` 这类通配规则不会预解析，只有具体子域被查询时才尝试加入缓存。
- 它只能补一部分“客户端已经拿到 IP 后继续直连”的场景，不是完整防火墙，无法覆盖 CDN、动态 IP、DoH 或硬编码 IP。

### DoH / DoT 注意事项

- 上游路由器把普通 DNS 再用 DoH 转发出去，通常仍安全：车机查询已经先经过 ESP32，黑白名单仍能生效。
- 如果车机、App 或系统「私人 DNS」直接向外部 DoH/DoT 服务器查询，ESP32 看不到域名，过滤会失效。
- 更稳妥的做法：关闭车机 / 测试设备的 Private DNS；在上游阻断常见 DoH/DoT 解析器和 `853` 端口；必要时在上游防火墙做真正的出口白名单。

---

## FSD 下发（`/feature_configuration`）

`esp32s3-waveshare-wifi` 内置与 Windows TeslaDG 对齐的下发链路，默认关闭（不启动 443/4567 监听以释放约 28 KB 内部 RAM 供 Wi-Fi/NAPT 使用）；在管理面板一键开启后自动拉起监听，关闭后自动释放资源，无需重启。

### 工作流程

1. **DNS 劫持**：AP 的 DNS 把 `api-prd.vn.cloud.tesla.cn` 和任意 `mothership.*`（含 `mothership.vn.teslamotors.com`、`mothership.vn.cloud.tesla.cn`）解析为 `9.9.9.9`。A 查询返回 AP 地址，AAAA 和其他类型返回空答案。
2. **443 TLS / 4567 HTTP 回包**：车机请求任意路径中的 `/feature_configuration`（可带 VIN 前缀、结尾 `/`、`?` 或 `#`）时返回：
   - `200 OK`
   - `Content-Type: application/json`
   - `X-Checksum: md5(compact body)`
   - `Cache-Control: no-store`
   - body 为当前 compact JSON 载荷
3. **其他路径**：443 反代到 `https://api-prd.vn.cloud.tesla.cn`（Host 重写为 api-prd）；4567 按 PC `svc/stack.py` 的 `reverse:http://192.0.2.1:4567` 语义处理——非 FSD 路径直接返回与 mitmproxy 同格式的 `502` HTML，不再访问真实母舰，避免云端改写配置。FSD 命中路径不受影响。

### 回复 / 观察模式

- `reply`：直接返回 ESP32 载荷。
- `observe`：只记录请求并转发到上游，不返回本地载荷。

可在面板「DNS → FSD 下发」卡片切换。

### 为什么「拦截次数」一直是 0（1 小时并不算异常）

车机通常每 **12 小时 ± 2 小时**才取一次 `feature_configuration`；重启车机后约 **5–10 分钟**会催一次。命中一次后短时间内不会再来。PC 版 README 也是同样的节奏（`Q5：等很久一直没命中？车机 12 小时才查一次`）。

按下面顺序看 `/fsd` 看板，基本能定位：

1. **FSD DNS 重定向** 一直是 0：车机没有把 `api-prd` / `mothership.*` 查询发到 ESP32。检查车机是否连的是 `fsd-esp32`、上游 Wi-Fi 是否已连接、是否开了私人 DNS/DoH。
2. **DNS 重定向 > 0，但握手成功/失败都是 0**：DNS 到了，但车机没有建立 443/4567 连接，或连接被中途重置。
3. **有握手，但车机请求总数一直是 0**：TLS 建立了但没发 HTTP，通常是车机还在等待/重试。
4. **有请求但命中 0**：看「车机最近请求」的路径是否包含 `/feature_configuration`；本固件匹配 `/feature_configuration`、`/feature_configuration/`、`...?x`、`.../#x`，不匹配 `/feature_configurations`。
5. **想立刻验证监听是否正常**：点看板上的 **「本地自测」**（或 `POST /api/fsd-mock/selftest`）。它会像 PC `web/server.py` 的 `/api/selftest` 一样向本机 4567（优先）或 443 发一次 `POST /vehicles/WEBTEST/feature_configuration`，返回 HTTP 状态、字节数、`X-Checksum` 和 body。自测标记为 `SELFTEST`，**不会计入「拦截次数」，也不会触发 2 分钟自动关闭**。`observe` 模式下自测按 observe 语义透传（4567 会得到 PC 同款 502），不是本地回包；要验证本地载荷请先切到 `reply`。

### 默认载荷预设

| 预设 | 字节 | md5 | 来源 |
|---|---:|---|---|
| ESP32 默认 22 字段 | 654 | `bd262907ff9fb2709bb54f9d3e78d07d` | Windows 20 字段（`disable_cabin_camera:false`）+ `enable_eu_fsd_behavior:false` + `autoshift_dr:true` |
| Windows 20 字段 | 602 | `241cb01b81a2b9c8b9f389a471bcbc94` | `payload.json` 原版 |
| 16 字段 | 476 | `b61035d9dea38e737fbf8195ccb39f2c` | `payload-16f.json` |
| 官方 282B | 282 | `c212a75ca858750ac302093c138991f2` | `payload-282b.json` |

默认载荷保存到 SPIFFS，重启保持。命中 / 握手日志只保存在 RAM，重启清空，不占闪存。

### 端口与证书

- `10010`：管理面板、client-config、OTA、日志。
- `443`：只跑 FSD 下发（TLS catch-all，命中 `/feature_configuration` 回包，其余路径反代）。证书 SAN 覆盖 `api-prd.vn.cloud.tesla.cn`、`mothership.vn.teslamotors.com`、`mothership.vn.cloud.tesla.cn`。
- `4567`：PC 版母舰明文 HTTP 反代（TeslaDG `svc/stack.py` 的 `reverse:http://192.0.2.1:4567`）。命中 `/feature_configuration` 回包；其余路径与 PC 一样得到 `502`（`Server: mitmproxy 12.3.1` + HTML），避免真实母舰改写配置。`web/server.py` 旧面板会解析真实母舰地址，但它不是 `start_all` / `start_hotspot` 的主链路。
- 443 使用与 Windows `--emudhra` 相同的 eMudhra 证书材料，而不是 mitmproxy 自签 CA。CA 链（EM OV TLS CA - G3C → emSign Root TLS CA - G3 → emSign ECC Root CA - G3，加 AAA 交叉签名）和私钥与 mitmproxy 12.3.1 `EMUDHRA_CHAIN_PEM` 逐字节一致；车机通常已信任 emSign 根，无需安装证书。
- Windows 版由 mitmproxy 按 SNI 动态生成短有效期叶子证书；ESP32 使用一张静态叶子证书（同样的 CA 私钥/公钥、issuer、EKU、AKI），有效期沿用 `certs2/mitmproxy-ca-cert.pem` 的 2026-11-07 → 2036-11-04，并一次覆盖 `api-prd` 和两个 `mothership` 域名。因此叶子证书不可能与 PC 每次动态生成的叶子逐字节相同，但链、密钥和校验结果一致。
- 如果车机不信任，可通过串口 `client-config` 命令导出 `EM OV TLS CA - G3C` 中间 CA，装进车机信任库。
- 443 同时只保留 1 个车机 TLS socket；上游未连接时，非 FSD 路径返回与 mitmproxy 同格式的 `502` HTML（`Server: mitmproxy 12.3.1`），而不是 JSON。

### TLS 版本

- **ESP32-S3 Wi-Fi 版**：Arduino 3.3.12 / ESP-IDF 5.5.5 / mbedTLS 3.6，支持 **TLS 1.2 + TLS 1.3**，车机支持时优先协商 1.3。
- **标准 ESP32 / esp32-wifi**：ESP-IDF 4.4 / mbedTLS 2.28，仅 TLS 1.2。
- TLS 1.1 与 TLS 1.3 无法在同一套 mbedTLS 上同时支持；本项目按 TLS 1.2 + 1.3 对齐。

### 常用接口

| 接口 | 方法 | 说明 |
|---|---|---|
| `/api/wifi-bridge/status` | GET | 桥接、DNS、FSD 状态；`fsdMock` 字段含载荷、命中、握手等 |
| `/api/fsd-mock/payload` | GET | 当前 compact JSON |
| `/api/fsd-mock/payload` | POST | 写入 JSON 对象，上限 2048 字节 |
| `/api/fsd-mock/set?enabled=1&mode=reply&preset=default` | GET | 启停、切换模式、加载预设 |
| `/api/fsd-mock/selftest` | POST | 本机 4567/443 回包自测（同 PC `web/server.py` `/api/selftest`） |
| `/api/fsd-mock/hits` / `handshakes` | GET | RAM JSONL 导出 |
| `/api/log` / `/api/log/download` | GET | RAM 诊断日志 / SPIFFS 持久日志 |

### 网页入口

| 页面 | 路径 | 说明 |
|---|---|---|
| 管理页 | `/` | 精简版：AP、上游、DNS、FSD（S3）、OTA、日志 |
| PC 风格 FSD 看板 | `/fsd` | 仅 S3 版：命中 / 握手 / 时间线 / 载荷字段编辑 |
| 车机客户端配置 | `/client-config` | 仅 S3 版：client-config JSON 发布 / 回滚 / 确认 |

---

## 自有客户端 HTTPS 配置发布

ESP32-S3 Wi-Fi 桥接固件在 `http://9.9.9.9:10010/client-config` 提供配置页面 / API。`443` 固定留给 FSD 下发，不再注册 client-config。

1. 连接 USB 串口（115200），发送 `client-config` 并换行，获取管理令牌、客户端令牌和证书信任包。
2. 连接设备热点，打开 `http://9.9.9.9:10010/client-config`。
3. 用管理令牌读取、发布或回滚配置；JSON 对象上限 1024 字节。
4. 客户端可拉取配置：

   ```sh
   python3 tools/client-config/pull.py --output client-settings.json
   # 默认 http://9.9.9.9:10010
   ```

管理凭据可读 / 发布 / 回滚；客户端凭据可读 / 确认。设备保留当前版本和上一份配置。

---

## OTA 升级

### 在线升级

管理页「在线更新」会查询 GitHub Releases，比较版本号。有新版本时下载对应 `<env>_v<ver>_ota.bin`，校验后写入并重启。

### 本地上传

1. 编译后项目根目录生成 `<env>_v<ver>_ota.bin`。
2. 管理页选择该文件上传。
3. 等待进度完成，设备自动重启。

> ⚠️ OTA 必须用 `_ota.bin`，不能用合并固件，否则会报 `Wrong Magic Byte`。
>
> ⚠️ 切换 env（例如从 `esp32` 切到 `esp32s3-waveshare-wifi`）分区布局不同，必须用 USB 重刷合并固件。
>
> ⚠️ 合并固件从 `0x0` 写入会覆盖 NVS。日常升级优先用 OTA，或只写 app：
>
> ```sh
> python3 -m esptool --chip esp32s3 -p <PORT> write_flash 0x10000 \
>   .pio/build/esp32s3-waveshare-wifi/firmware.bin
> ```

---

## 串口恢复命令

串口参数：`115200`，每条命令以换行结束。

| 命令 | 作用 |
|---|---|
| `fsd-payload` | 打印当前 compact JSON |
| `fsd-default` / `fsd-16f` / `fsd-282b` | 加载预设并写入 SPIFFS |
| `net-test` | 检查上游 DNS + TCP 到 `api-prd` |
| `wifi-diag` | 打印载荷 / 命中 / 握手计数、堆、PSRAM、SPIFFS |
| `wifi-list` | 列出已保存上游 |
| `wifi-add <ssid> <password>` | 添加 / 更新上游并立即连接 |
| `wifi-del <ssid>` | 删除上游 |
| `wifi-up` | 启用上游 |
| `wifi-ap-only` | 关闭上游并强制 AP 回到信道 1 |
| `client-config` | 打印管理令牌、客户端令牌和 443 信任包 |

NVS 被清空后，可以用这组命令重新配置，无需先连上 AP。

---

## 从源码构建

### 环境要求

- VS Code + PlatformIO IDE
- Python 3.10+
- 首次构建会下载 ESP32 工具链（约 200–500 MB）

### 编译 / 烧录

```sh
# 主力 Wi-Fi + FSD 下发固件
pio run -e esp32s3-waveshare-wifi

# USB 烧录
pio run -e esp32s3-waveshare-wifi -t upload

# 清空 flash（可选，会清除 Wi-Fi / PIN / 规则）
pio run -e esp32s3-waveshare-wifi -t erase
```

构建脚本会自动：

1. 用 `compress_html.py` 把当前 env 对应 HTML 压缩进 `include/web_ui_gz.h`。
2. 用 `merge_firmware.py` 在项目根目录生成合并固件和 `_ota.bin`。

### TLS 1.3 构建注意

`esp32s3-waveshare-wifi` 首次构建会下载 Arduino 3.3.12 / ESP-IDF 5.5.5。如果 PlatformIO 的 ESP-IDF Python 环境创建失败（Python 3.15 + 旧版 pydantic-core），执行：

```sh
PYO3_USE_ABI3_FORWARD_COMPATIBILITY=1 ~/.platformio/penv/.espidf-5.5.5/bin/pip install \
  idf-component-manager -r ~/.platformio/packages/framework-espidf/tools/requirements/requirements.core.txt
```

然后重新运行 `pio run -e esp32s3-waveshare-wifi`。

---

## 常见问题

**连上 Wi-Fi 后打不开 `9.9.9.9`？**

手机可能因为热点“无互联网”自动切回移动数据。关闭「智能 WiFi / 自动切换更优网络」，或暂时关闭移动数据。

**面板打不开 / 忘记密码？**

- 确认连接的是 `fsd-esp32`（或你改过的 SSID）。
- 管理面板端口是 `10010`，不是 80。
- 忘记密码可清空 NVS 后重刷：`pio run -e esp32s3-waveshare-wifi -t erase`，再重新烧录。

**QQ 音乐不能扫码 / 网易云音乐异常？**

把 `api-prd.vn.cloud.tesla.cn` 临时加入白名单（同名时白名单优先）。音乐恢复后立即移回黑名单或断开 Wi-Fi，避免云端改写 FSD 配置。

**车机连上了但没有网络？**

- 检查上游 Wi-Fi 是否已连接。
- 检查 DNS 是否开启、白名单是否包含所需域名。
- 检查上游是否强制客户端 DoH / DoT。

**FSD 没有下发？**

- 确认「FSD 下发」开关已开启，模式为 `reply`。
- 确认车机确实使用 ESP32 的 DNS。
- 查看 `/fsd` 看板的命中 / 握手记录。
- `api-prd` / `mothership.*` 在黑名单中不影响 FSD 劫持；劫持判断优先于 DNS 规则。

**OTA 报 `Wrong Magic Byte`？**

上传了合并固件。请改用 `_ota.bin`。

**烧录时报 `Failed to connect`？**

按住 BOOT，点一下 RST，松开 RST 后再松开 BOOT，然后重新上传。

---

## 项目结构

```text
src/
  main.cpp                入口：Wi-Fi AP/STA、DNS 任务、Web 服务、OTA
  client_config.cpp       client-config 与按需 443/4567 监听管理
  fsd_mock.cpp            FSD mock 状态 / 证书 / 回包实现

include/
  fsd_version.h           固件版本
  mod_wifi_bridge.h       STA↔AP 桥接、NAT、事件驱动重连与上游管理
  mod_dns.h               DNS 黑白名单、LRU 缓存、IP 拦截缓存
  mod_fsd_mock.h          FSD 下发状态与接口
  mod_client_config.h     client-config 发布 / 回滚
  mod_ota.h               在线 OTA
  mod_log.h               RAM / SPIFFS 日志
  mod_thermal.h           芯片温度监控与热回退
  lwip_hooks.h            lwIP 转发钩子
  fsd_tls_identity.h      443 证书链
  web_ui_manage.h         Wi-Fi 精简管理页
  web_ui_fsd.h            PC 风格 FSD 看板（/fsd，S3）
  web_ui_client_config.h  client-config 页面（S3）
  web_ui_gz.h             构建时自动生成的 gzip 页面

platformio.ini            构建环境（esp32-wifi / esp32s3-waveshare-wifi）
idf_o2_compat.cmake       ESP-IDF 5.5 -O2 上游 esp-modbus 兼容钩子
min_spiffs.csv            分区表
compress_html.py          构建前压缩 HTML
merge_firmware.py         构建后生成合并 / OTA 固件
tools/client-config/      client-config 客户端脚本
tests/                    DNS / Wi-Fi 桥接 / FSD 载荷回归测试
```

---

## 安全与免责声明

1. **仅限已购买 FSD 的车辆**。本设备不绕过 Tesla 服务器端授权。
2. **风险自担**。修改车辆网络、CAN 总线或云端配置可能导致功能异常、配置被改写、账号风控或车辆损坏。
3. **保修风险**。改装可能使 Tesla 官方保修失效。
4. **遵守当地法律**。辅助驾驶功能的使用须符合所在地区法规。
5. **驾驶责任**。使用期间驾驶员必须保持专注、手握方向盘并随时接管。
6. **先测试再上路**。初次使用请在封闭 / 私有场地验证。

本项目仅供学习、研究和实验使用。作者与贡献者不对任何直接或间接损失负责。

---

## Credits

| 项目 | 用途 | 许可证 |
|---|---|---|
| [ev-open-can-tools](https://github.com/ev-open-can-tools/ev-open-can-tools) | 上游 CAN 注入核心逻辑 | GPLv3 |
| [ev-open-can-tools-plugins](https://github.com/ev-open-can-tools/ev-open-can-tools-plugins) | HW4 offset 上限、TLSSC 旁路参考 | GPLv3 |
| [opendbc](https://github.com/commaai/opendbc) | Tesla DBC 信号定义 | MIT |
| [flipper-tesla-fsd](https://github.com/hypery11/flipper-tesla-fsd) | bit / checksum 交叉验证 | GPLv3 |

## 许可证

GPLv3 — 基于 [ev-open-can-tools](https://github.com/ev-open-can-tools/ev-open-can-tools)（原 tesla-open-can-mod）开发。
