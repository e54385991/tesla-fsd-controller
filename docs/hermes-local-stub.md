# Hermes 本地桩（零外网方向）— 设计与实车验证计划

> 目标：车机以为自己在 Hermes 上在线，但 Hermes 流量**从不触达 Tesla**。
>
> 边界：仅适用于已购买/订阅 FSD 的车辆；不用于绕过付费或地区授权。若车端对
> Hermes 做证书 pinning，立即停止，不尝试绕过。详细免责声明见仓库 README。

---

## 1. 为什么先做本地桩，而不是长期白名单

长期把 `hermes-prd.vn.cloud.tesla.cn` 放白名单意味着：

- Tesla 可以通过 Hermes 下发命令、改写配置；
- 车机配置与云端不一致时，存在 FSD 权益被回收或账号风控的风险；
- 只要车机 LTE 还在，Wi-Fi 侧的放行/阻断并不能决定 Tesla 是否能看到车辆；
- 一旦云端推来新配置，本地 `/feature_configuration` 载荷可能被覆盖。

本地桩把 Hermes 变成“只连接本机”的服务，Wi-Fi 侧不再有 Tesla 暴露。但它
**不能解决 LTE**：真正的零外网必须物理断蜂窝（下 SIM/断 modem），否则车机仍可
走 LTE 连 Tesla。先确认这一点，再决定是否值得做本地桩。

---

## 2. 目标架构

```text
车机
  │ Wi-Fi（唯一出口）
  ▼
ESP32 / Linux AP（DNS + NAT + 防火墙）
  │
  ├─ api-prd / mothership → 本地 FSD mock（现有 443/4567）
  ├─ hermes-* / signaling → 本地 Hermes 桩（新增）
  └─ 其他域名 → 按白名单，或全部阻断
```

关键约束：

1. **本地桩只监听，不向上游转发。**
2. **不发送真实签名命令。** 只实现“在线 + 配置推送 + ACK/ping”。
3. **DNS 必须把车机实际使用的 Hermes 主机名指到本地。** 不同车型/固件可能使用
   `hermes-prd`、`hermes-stream-prd`、`signaling`、`hermes-prd1` 等不同名字，先抓
   DNS 再决定，不要凭假设。
4. **443 需要至少两个并发 TLS socket**：一个长连接 WebSocket，一个
   `/feature_configuration` 请求。现在 ESP32 的 443 在 PSRAM 板子上保留 4 个
   TLS socket（无 PSRAM 回退为 2 个），满足 WS + 配置 + 重试；仍建议先用独立
   Linux 探针验证车型行为。

---

## 3. 阶段门（go / no-go）

### Gate 0：基线（不改代码）

用现有固件做 2×2 对照，确认 Hermes 是否真的影响配置拉取：

| LTE | Wi-Fi Hermes | 观察 |
|---|---|---|
| 开 | 阻断 | 若仍拉配置 → Hermes 非必要 |
| 开 | 放行 | 对照 |
| 关 | 阻断 | 基线 |
| 关 | 放行 | 若拉配置 → Hermes 必要 |

每次 10 分钟，重复 3 次。记录 `/feature_configuration` 命中、时间戳、DNS 查询、
握手结果。测试时关闭 `autoOffOnHit`。

> 如果不想让 Hermes 触达 Tesla，可跳过“放行”两组，直接做 Gate 1 的本地 TLS
> 探针；但会少一个基线对照。

### Gate 1：TLS 信任 / pinning

使用 `tools/hermes-probe/` 的 `tls` 阶段：

- 本地监听 443，加载带 Hermes SAN 的 eMudhra 叶子证书；
- 记录 SNI、ALPN、TLS 版本、cipher、TLS alert；
- 用“api-prd 叶子 + api-prd SNI”做阳性对照，用“错误 SAN/自签证书”做阴性对照。

判读：

| 结果 | 结论 |
|---|---|
| Hermes 叶子成功，阴性对照失败 | 正常 CA + hostname 校验，无额外 pinning |
| Hermes 叶子失败，api-prd 阳性对照成功 | Hermes 单独 pinning/独立 CA → **停止** |
| 全部失败 | 先排查车机时间、eMudhra 链信任、OCSP/CRL，不要归因于 pinning |
| 没有 ClientHello | 车机没尝试：检查 DNS 缓存、LTE、睡眠状态 |

**失败即停止。** 不尝试 root 车机、提取证书或绕过 pinning。

### Gate 2：WebSocket + 配置触发

`ws` / `config` / `full` 阶段：

- `ws`：只完成 101，不发帧；测“连上是否就拉配置”。
- `config`：101 + 一条 `STREAMING_CONFIG`；测“配置推送是否触发拉取”。
- `full`：再加 ACK/ping；测“能否维持在线”。

关键指标：`ws_open` 到第一次 `/feature_configuration` 命中的时间。与 Gate 0 的
基线对比。

如果 `config`/`full` 都不触发配置拉取，说明 Hermes 不是触发器；不要继续盲目扩展
桩，回到 `mothership`、`connman` 或车机自身定时器排查。

### Gate 3：持久性（24-72 小时）

- 本地桩持续运行，记录 WS 重连、ping、配置重复拉取、车机 UI 状态。
- 关键问题：**24 小时后 FSD 是否仍可用？** 如果服务端权益有离线宽限期，本地桩
  只能短期有效，零外网方案不成立。
- 观察车机是否在 Wi-Fi 无外网时切回 LTE；最彻底的做法是下 SIM/断 modem。
- 恢复网络前想清楚：车机可能回传离线期间的日志/配置状态。

### Gate 4：出口审计

在网关做默认 DROP，只允许本地探针端口；抓所有车机流量。验证没有 Tesla 公网 IP、
DoH、硬编码 IP、IPv6 泄漏。DNS 日志不能代替包级审计。

---

## 4. 探针实现

代码在 [`tools/hermes-probe/`](../tools/hermes-probe/)：

- `hermes_probe.py`：标准库实现的 TLS/WebSocket/Protobuf 最小服务；
- `make_lab_cert.py`：从 `include/fsd_tls_identity.h` 的 eMudhra 材料生成带 SAN 的
  实验室叶子证书；
- `proto/hermes.proto`：社区逆向的 Hermes 消息结构；
- `tests/`：不依赖网络/车辆的单元测试。

跑法见 `tools/hermes-probe/README.md`。核心命令：

```sh
python3 tools/hermes-probe/make_lab_cert.py \
  --san hermes-prd.vn.cloud.tesla.cn \
  --san hermes-stream-prd.vn.cloud.tesla.cn \
  --san signaling.vn.cloud.tesla.cn \
  --out tools/hermes-probe/certs

sudo python3 tools/hermes-probe/hermes_probe.py \
  --port 443 \
  --cert tools/hermes-probe/certs/lab-cert.pem \
  --key tools/hermes-probe/certs/lab-key.pem \
  --stage tls --log hermes-tls.jsonl
```

探针只监听，不建上游连接。日志会记录 SNI/ALPN/握手结果、HTTP Upgrade、`X-Jwt`
是否存在（只存哈希）、WebSocket 帧摘要；不会记录完整 token。

---

## 5. Gate 通过后的落地选择

### 推荐：伴生 Linux 设备

ESP32 继续做 AP/DNS/FSD mock；Hermes 桩跑在 Pi Zero 2 W、旧手机或小主机上。
需要给 ESP32 增加 **per-host DNS answer**：Hermes 主机名指向伴生设备，api-prd /
mothership 仍指向 ESP32。

优点：

- 不占 ESP32 的 443 单 socket 和紧张堆内存；
- Python/Go 实现 TLS/WS/Protobuf 更容易；
- 可同时跑 `tcpdump`、dnsmasq、日志；
- 出问题不影响 FSD 下发主链路。

### 不推荐先做：ESP32 原生

当前约束：

- `src/client_config.cpp` 的 443 现在是 `tlsSlots = PSRAM ? 4 : 2`；
- 长连接 WebSocket 占一个 socket 时，车机仍需要第二个 TLS socket 请求
  `/feature_configuration`，4-slot 策略已预留该余量；
- 没有 WebSocket handler，`esp_https_server` + `httpd_ws_*` 需要先验证；
- `kHostCapacity=96`，加 Hermes 域名要改容量/NVS 布局；
- 压测低水位约 12 KB，再加 WS/Protobuf 风险很高。

只有在 Gate 1/2 证明车端只用 HTTP/1.1 WebSocket 且流量很轻时，才考虑 ESP32 原生。

### 兜底：一次性限时放行

如果 pinning 存在，或不想做本地桩：管理页加“一次性 Hermes 窗口”，放行
DNS + NAT 120 秒，命中 `/feature_configuration` 或超时后立即撤销，并把 Hermes IP
加入临时阻断；不持久化，重启恢复黑名单。风险仍存在，但远小于长期白名单。

---

## 6. 需要在实车上确认的参数

1. 车型/年份/MCU/固件版本；是否已购买 FSD。
2. SIM 是实体卡还是 eSIM；能否下卡/断 modem；eCall/OTA 影响是否可接受。
3. 车机实际查询的 Hermes/签名主机名（DNS 抓包）。
4. Wi-Fi 与 LTE 的路径优先级；无外网 Wi-Fi 时是否切回 LTE。
5. 车机是否带客户端证书、是否需要 `X-Jwt`、是否使用 HTTP/2。
6. `/feature_configuration` 的触发器：连接、`STREAMING_CONFIG`、定时器还是其他。
7. 24-72 小时后 FSD 是否仍可用（服务端权益宽限期）。

---

## 7. 安全与合规

- 仅用于自有车辆、封闭场地、停车状态；不要公共道路测试。
- 不记录完整 JWT、VIN、客户端证书；日志脱敏。
- 不转发控车命令；本地桩没有 Tesla 上游。
- 不尝试绕过 pinning；不 root 车机；不安装自定义 CA。
- 可能违反 Tesla 服务条款，导致账号风控、FSD 权益回收、保修失效；风险自担。
- 下 SIM 可能影响 eCall/道路救援/OTA；是否可接受需自行判断。
