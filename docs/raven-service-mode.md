# Model X Raven 维护模式：实现状态

目标车辆由用户提供：**2019 Model X Raven，车机版本 2026.2**。这不是从车辆自动识别的结果，也不是兼容性验证结论。当前仅为此目标保留开发入口，不宣称覆盖其他 Raven 年款或软件系列。

用户已确认接线为 **X179 的 13/14，只有 CAN，暂无诊断网线或 Toolbox**。Tesla 的 [Model X SOP9 连接器资料](https://service.tesla.com/docs/ModelX/ElectricalReference2015/x-lhd-sop9/x179/)记录该针脚通向 X730；该版本覆盖 2019-10-21 起生产车辆，不能据此推断这辆车的准确线束版本。

## 已实现

- 手机主页「车辆信息」与车机页面「设置 → 操作」可打开 `/service`。
- 页面显示设备连接状态、未知的维护模式状态，以及只读挡位、带方向的车速与 P 挡/静止观察结果。
- 独立观察器按公开旧款 S/X 的 `DI_torque2` 定义解析 `0x118`，要求 DLC 为 6、加和校验匹配，挡位仅接受 1～4，车速拒绝 `4095/SNA`。无信号、超过 1000 ms、CAN 不可用或驱动会话改变时不提供有效观察值。此逻辑不等于 Raven 实车兼容性验证，不能授权车辆操作。
- CAN 与 Web 核心通过锁读写完整快照；驱动会话代数使用原子读取。页面每秒刷新，显示报文格式、年龄与原始字节，失败时清除旧 P 挡显示。
- 页面保留 Tesla 官方的手动进入、退出步骤，并提供官方 Toolbox 例程链接；打开链接不执行例程。
- 自动进入、退出按钮保持禁用。二次确认交互已预留，当前设备能力不会开启它。
- `GET /api/service-mode` 返回能力、真实观察值及其有效性，不把设备联网、CAN 收到数据或 HTTP 成功当作车辆已进入维护模式。`vehicleVerified`、`telemetryVerified` 与 `protocolVerified` 仍为 `false`；失效观察值为 `null`。
- `POST /api/service-mode` 校验现有 PIN 会话、浏览器 Origin、操作、确认字段与声明的目标范围；即使全部匹配，也返回 `501 protocol_unverified`、`transmitted:false`。本功能没有 CAN 发送路径。
- 不写入 NVS，不在启动、刷新、重试或 OTA 后自动执行车辆操作。

构建平台固定为 `espressif32@6.12.0`，匹配项目现有的 Arduino 2.x / ESP-IDF 4.4 API。原配置自动拉取的平台 8.0.0 使用 Arduino 3.x，现有 `esp_task_wdt_init(30, true)` 无法编译；本次没有进行全项目框架迁移。

项目版本头文件从 `version.h` 更名为 `fsd_version.h`，避免 Wi-Fi 混合构建时遮蔽 libsodium 自己的同名头文件。固件版本号未改变。

## 尚未实现：实际进入与退出

[Tesla 2015–2020 Model X 官方维修手册](https://service.tesla.com/docs/ModelX/ServiceManual/en-us/GUID-0DC534D7-A4B8-417F-8E42-DF1560E970C2.html)给出了车机入口，以及 Toolbox 3 例程 `PROC_ICE_X_ENTER-SERVICE-MODE` / `PROC_ICE_X_EXIT-SERVICE-MODE`。Toolbox 流程包含网关解锁。该页面没有提供可直接移植的 CAN 请求、响应或成功判据。

[Toolbox 3 连接说明](https://service.tesla.com/docs/ModelX/ServiceManual/en-us/GUID-291C383D-E64F-4A2F-B673-2532F0B3181A.html)说明了旧款 Model X 的本地诊断连接；它不能证明本项目当前连接的一路 CAN 足以完成同样的操作。

本项目的 `mod_telemetry.h` 使用 Model 3 Party CAN 定义，且没有为 Raven 建立独立的数据有效期校验。因此，不能将现有 `gearRaw` / `speedD` 直接用作 Raven 的 P 挡、静止安全条件。UDS 诊断会话也不能被当作车辆屏幕上的 Service Mode。

## 2026-11-01 公开资料核查

用户要求的是**设备实际触发车机进入 Service Mode**，不是新增入口、显示 P 挡或进入 UDS 扩展诊断会话。当前这一目标尚未完成。

| 来源 | 确认的内容 | 不足以证明的内容 |
| --- | --- | --- |
| [Tesla Model X Service Mode](https://service.tesla.com/docs/ModelX/ServiceManual/en-us/GUID-0DC534D7-A4B8-417F-8E42-DF1560E970C2.html) | 官方例程名称为 `PROC_ICE_X_ENTER-SERVICE-MODE` 与 `PROC_ICE_X_EXIT-SERVICE-MODE`，Toolbox 流程包含网关解锁 | 没有公布可用于 X179 13/14 的 CAN 请求或响应 |
| [Tesla Toolbox 3 连接](https://service.tesla.com/docs/ModelX/ServiceManual/en-us/GUID-291C383D-E64F-4A2F-B673-2532F0B3181A.html) | 需要车辆诊断连接及 Toolbox 本地连接；例程集成在车辆固件中 | ESP32 的 Wi-Fi 热点不等于该车辆诊断连接 |
| [opendbc 旧款 Tesla DBC](https://github.com/commaai/opendbc/blob/178c81ab5c052ac92a849f6400703d082d268c02/opendbc/dbc/tesla_can.dbc) | `0x118/DLC6` 中挡位为 `12|3`，车速为 `16|12`，`raw × 0.05 − 25 MPH`，`4095` 表示 SNA | DBC 没有提供进入车机 Service Mode 的控制例程，也不验证该车当前总线数据 |
| [BogGyver/panda 校验实现](https://github.com/BogGyver/panda/blob/tesla_unity_dev/board/safety/safety_tesla.h) | `tesla_compute_checksum` 将地址低/高字节与除末字节外的载荷求和后取低 8 位 | 本机没有这辆 Raven 的已知正确报文，仍需与真实流量核对 |
| [Model X 信号资料](https://github.com/talas9/tesla_can_signals/tree/main/ModelX) | S/X CH 与 ETH 数据分属不同通道；ETH 的 `GTW_diagSession` 是网关到 MCU 的周期状态报告 | `LEVEL_SERVICE` 枚举不等于进入命令；不能凭名称伪造该广播来声称执行成功 |
| [Tristan Rice 的车机研究](https://fn.lc/post/tesla-model-3-modes/) | 2020 年 Model 3 车机中存在 `GUI_serviceMode` 数据值 | 它属于车机内部访问，不是 X179 CAN API，不能外推到 Raven 2026.2 |

进一步阅读了公开的 2019 年 ODIN 客户端研究与 Tesla 车机安全研究。它们展示的是内部网络、权限或车机进程访问；没有找到适用于本项目接线与目标固件的完整进入/退出事务。**“没有找到公开实现”不等于“物理上不可能经 CAN 实现”**，但目前不足以编写真实控制命令。

当前已明确只有双线 CAN，后续仍需该总线上的可复现进入/退出协议证据，或适用于目标车型与固件的控制协议资料。若以后增加已有授权的诊断网络/Toolbox，可核实正式例程的请求与回执。不能将旧 Model 3 内部调用、UDS `0x10` 会话或网关 `LEVEL_SERVICE` 状态当作替代命令；不引入未连通的以太网后端来声称功能完成。

### 补充调查：第三方 CAN 激活及 0x339 线索

用户补充：中国市场的手动入口存在服务中心附近的限制，第三方模块可进入，品牌未知。本次检索找到了支持继续研究 CAN 路线的一手资料。因此，**不能把“只有 CAN”判定为一定做不到；也不能把第三方对 Model 3/Y 的支持外推成 Raven 已可用。** 地域限制本身不是本项目已经测出的车辆状态。

| 一手来源 | 实际证据 | 对本车的适用边界 |
| --- | --- | --- |
| [LOKI 官方 2.7.0 更新，2025-07-17](https://lokidiagnostics.com/loki-2-7-0/) | 厂商明确声明通过 CAN 激活 **Model 3/Y** 的 Service Mode，特别提及中国市场车辆 | 该项声明没有覆盖旧款 S/X 或 Raven；同页其他 S/X 功能不代表这项功能也兼容 |
| [Atlas 固件发送代码](https://github.com/JordanzhaoD/Atlas-single-can-firmware/blob/75ac7e72c5fd1310c5926e5f14bbd09a1861a129/src/main.cpp#L255) | 双 CAN 编译分支在第二路 BODY 总线上发送 `0x339/DLC8`；代码将 byte 5 的 bit 7 作为请求位，4 帧、间隔 10 ms | 这是可读到的源码实现，不是 Raven 实车验证；本项目目前仅接一路 X179 13/14，不能直接移植其第二路发送路径 |
| [talas9 Model 3 VEH 信号定义](https://github.com/talas9/tesla-can-signal-db/blob/4850104dca0dfabb123fd7afdfe31fc5e9182efb/docs/model3/2025.20.8/veh/vcsec/vcsec_authentication.md) | `VCSEC_authentication` 为 VEH CAN `0x339`；`VCSEC_serviceDiagnosticRequest` 布局为 little-endian `47\|1` | 页面将该位标为 `layout-only`，并说明该帧尚未实车观测；布局不能单独证明触发、退出或回执语义 |
| [Tesla CAN Explorer 的 S/X MCU2 数据](https://github.com/bruvv/tesla-can-explorer/blob/cb07271ace7d482b7a41a4635c33e2b954c8c8ee/data/can_frames_decoded_all_values_modelsx_intel.json) | 2026.2 S/X Intel 数据中，**ETH 的 `0x339` 是 `DAS_bodyControls`**；同名消息在实际 CH/BDY 通道为 `0x3E9`，包含灯光、雨刷等控制字段 | ETH 是车内转发通道，其编号不能直接当作 X179 上的 CAN 编号；此数据没有建立上述 3/Y 请求位在 Raven 上的对应关系 |
| [Tesla CAN Explorer 的 Model 3 MCU3 数据](https://github.com/bruvv/tesla-can-explorer/blob/cb07271ace7d482b7a41a4635c33e2b954c8c8ee/data/can_frames_decoded_all_values_mcu3.json) | 同为 2026.2，Model 3 ETH 的 `0x339` 则是 `VCSEC_authentication`，包含 `VCSEC_serviceDiagnosticRequest` | 进一步说明车型和通道必须一起匹配，不能只按编号查找 |
| [元征官方接头说明](https://www.cnlaunch.com/news-detail/i-1441.html) | 厂商把进入维修模式归入 **LAN 网口**功能，并列出 S/X 2018–2021 的接线方案 | 这类第三方诊断仪能够进入，不等于目前双线 CAN 连接具备相同通道 |

另外检索到[“用 CAN 卡进特斯拉维修模式”的中文演示文稿](https://www.douyin.com/shipin/7361071368156416010)。文稿提到 500 kbit/s、发送 ID“339”后屏幕进入维护模式，但未读出完整载荷和演示车辆版本。链接是动态聚合页，未取得可逐帧核对的原始视频；第三方自动摘要所谓“尾号 1000”不能作为完整协议，更不能据此补零生成发送报文。

本次逐项检查了上述两个 Explorer 数据集（Model 3 577 帧、S/X 1728 帧），以及 Atlas 的真实发送实现。下载内容已与 Git 对象 SHA-1 对照一致。找到的结果是 **3/Y 的具体 CAN 候选路径**，尚未找到 **2019 Raven / 2026.2 / X179 13/14** 的可复现进入/退出事务。S/X 的 `MCU_carInService`、`MCU_carInServiceModePlus` 与 `GTW_diagLevel` 仍只是信号候选，不能凭名称确认其控制方向。

因此，本轮只更新研究结论，不新增 `0x339` 注入、不开放现有按钮。余下缺口已缩小为：Raven 对应的真实请求及所在总线、进入/退出的回执与软件适用范围。已有 P 挡观察只能解决操作前置条件，不能替代这个协议缺口。

### 扩大到 Legacy S/X 2017–2020 的后续核查

按用户要求将研究范围扩大到 Legacy S/X 2017–2020，优先 MCU2/Intel。新增检查 9 个开源仓库，并核对 Foxwell 官方全部 284 条 Tesla/NT530 功能记录；相关维护条目属于 TAS 空气悬架，不能据此确认车机 Service Mode 支持。仍未取得可用于当前接线的进入/退出事务。用户已确认暂时无法取得兼容模块或维修店抓包，本轮没有新增控制代码或执行实车试发。

完整来源、固定提交号、报文候选和排除依据见 [Legacy Service Mode 研究记录](legacy-service-mode-research.md)。这次扩大的是检索范围，不是固件已验证车型范围，实际一键进入仍未完成。

## 打通实车所需证据与验收

1. 已知只有 X179 13/14 CAN；继续确认 ESP32 板型与实际线束/总线。
2. 从适用于该车的软件协议资料或正常诊断操作记录中确认进入、退出的实际传输方式、请求与响应。手动车机操作时采到的 CAN 状态变化本身不能证明存在可重放的控制命令。
3. 验证 Raven 车型识别、P 挡与车速信号，包含无信号、失效值、超时与跨核读取一致性。任何未知状态都阻止车辆操作。
4. 验证一次确认仅触发一次请求；校验会话、网关要求、超时、重复点击、取消和断线。不能通过自动重试重复发送状态改变请求。
5. 以经验证的车辆回执和车机真实模式为成功依据；HTTP 200 或 CAN 发送成功均不够。完成进入与退出的停驻实车验证后，才能开放按钮。

在以上条件完成前，**本功能仍不能一键进入维护模式，不应作为已支持功能发布或刷入车辆验收**。

## 本地检查

```sh
clang++ -std=c++11 -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -Iinclude tests/service_mode_test.cpp -o /tmp/service-mode-test
/tmp/service-mode-test
node --test tests/service_mode_ui.test.mjs
pio run -e esp32 -e esp32s3-waveshare
```

固件构建还会执行 `compress_html.py`，将独立页面压缩到 `SERVICE_HTML_GZ`。本地测试不能替代实车协议验证。

原生检查覆盖旧款 S/X P/R/N/D 与无效挡位、正负车速、SNA、校验失败、DLC 冲突、信号失效、时间回绕、驱动恢复、并发快照和 JSON 容量；页面检查覆盖实时观察、失效清理、PIN、确认、重复提交与不自动重试。

本机 Wi-Fi 混合构建使用隔离的 Python 3.9.6 环境验证。Python 3.15 下 ESP-IDF 4.4 的 `pyparsing` 依赖因缺少 `sre_constants` 而失败；切换到旧版 ESP-IDF 兼容的 Python 环境后再构建，不要修改业务代码来绕过该依赖错误。

2026-11-01 本轮检查通过：Service Mode 原生检查（AddressSanitizer/UndefinedBehaviorSanitizer）、9 项维护页测试、4 项原有页面测试、14 组原有 CAN 回归，以及原有 API/NVS/状态检查。ESP32、ESP32-S3、ESP32 Wi-Fi 和 ESP32-S3 Wi-Fi 四个发布环境构建通过。混合构建还需将 `PLATFORMIO_PACKAGES_DIR` 与 `PLATFORMIO_PLATFORMS_DIR` 指向实际目录；通过隔离 core 目录下的符号链接访问会触发 ESP-IDF 重复构建同名源文件错误。
