# Legacy Model S/X：直接进入 Service Mode 的公开资料核查

核查日期：2026-11-01。

研究范围扩大为 **2017–2020 Legacy Model S/X**，优先 MCU2/Intel；MCU1/Tegra 单独核对。目标实车仍为用户提供的 2019 Model X Raven、2026.2，当前连接为 X179 13/14 单路 CAN。扩大研究范围不等于扩大固件已验证兼容范围。

## 当前结论

已经找到 Model 3/Y 通过 CAN 激活车机 Service Mode 的厂商声明与开源发送代码；本轮没有找到可用于上述 Legacy S/X 连接的完整进入、退出和成功回执协议。因此，本轮没有新增固件控制代码，也没有开放进入/退出按钮。**目标功能尚未实现。**

这不是“CAN 在物理上一定做不到”的结论。准确缺口是：请求载荷、所在总线、车机/网关接收路径、版本适用范围与可验证的响应。知道一个服务相关信号或一个 UDS 会话编号，都不能补齐这些信息。

## 厂商资料的核对结果

| 来源 | 确认内容 | 适用边界 |
| --- | --- | --- |
| [LOKI 官方 2.7.0 更新](https://lokidiagnostics.com/loki-2-7-0/) | 2025-07-17 的更新明确声明通过 CAN 激活 Model 3/Y 的 Service Mode，并特别提及中国市场车辆 | 该项没有声明支持旧款 S/X；同一产品支持其他 S/X 功能不能代替逐功能兼容确认 |
| [元征官方诊断接头说明](https://www.cnlaunch.com/news-detail/i-1441.html) | 将进入维修模式归入 LAN 网口功能，并提供 S/X 2018–2021 的连接方案 | 不能当作当前两根 CAN 线的控制协议 |
| [Foxwell 官方功能查询](https://www.foxwelltech.us/support-fun-list.html) | 读取 Tesla + NT530 查询的全部 284 条记录；其中 Model S 的条目 79/81 和 Model X 的条目 216 将维护模式归在 TAS 空气悬架系统 | 经销商概述中的“Activate service mode”不足以证明是车机红框模式；本次官方列表没有提供 MCU/GTW 进入车机 Service Mode 的条目或报文字节 |
| [Tesla 旧款 Model X 维修手册](https://service.tesla.com/docs/ModelX/ServiceManual/en-us/GUID-0DC534D7-A4B8-417F-8E42-DF1560E970C2.html) | 车机入口及 Toolbox 进入/退出例程；Toolbox 路径包含网关解锁 | 例程名称相同不证明底层 CAN 事务在所有车型上相同；公开页面未给出请求字节 |

Foxwell 查询可复核的公开接口为 `/funSearch/getFunData2`，参数：`carId2=809`、`goodsId2=615`、`limit=100`、`otherParams=[]`，分别读取 `page=1,2,3`。这些编号是网站查询参数，**不是 CAN 地址、UDS DID 或车辆命令**。284 条记录的覆盖范围和版本标注，也不构成这辆 Raven 的实测结论。

## 本轮新增源码检查

逐项查看了下面 9 个仓库的说明与候选代码。提交号固定在本轮读取版本，避免后续版本变化导致结论无法复核。关键词扫描包含 Service Mode、进入/退出、中文维护/维修模式、`serviceDiagnostic`、`0x339` 等，并结合项目用途判断；**未检索到实现不能证明未检查的分支、历史或私有版本中不存在实现**。

| 仓库及本轮提交 | 结果 |
| --- | --- |
| [TSL6-Firmware · 374762d8d950](https://github.com/jfryton/TSL6-Firmware/tree/374762d8d95040765e02aff2a2e71e9593774ce3) | 检查固件研究文档、253 项动作分发表和 CAN 映射；没有定位到已验证的 Legacy S/X 车机进入/退出事务。文档中“Valet/Service”快捷键禁用功能不等于车辆 Service Mode |
| [TeslaDiagScripts · 7a537dfbd779](https://github.com/srccd-dev/TeslaDiagScripts/tree/7a537dfbd7794464e92d728f73b78231381a1508) | 面向旧款 S/X 的诊断与解码工具；没有定位到车机进入/退出实现 |
| [Pre-ap_diag · 71e802e13237](https://github.com/philipr144-create/Pre-ap_diag/tree/71e802e1323734f7ef20e4f860ae5d17edf9e191) | 目标为 2013 Model S；其中 EAS 的 Exit/Finish Service 例程属于空气悬架诊断上下文，不能当作车机 Service Mode 退出命令 |
| [UDS-TESLA · f872d73a6577](https://github.com/sampepin86/UDS-TESLA/tree/f872d73a6577eeb4577917193fc2b6d5030c1c36) | 通用 UDS 客户端与 ESP32 桥接；诊断会话能力不能证明可以进入车机模式 |
| [tesla-canserver-myremote · c2a32d5d231d](https://github.com/monah-studio/tesla-canserver-myremote/tree/c2a32d5d231d6223ae96bdbd0726230060786dd6) | 面向 pre-2021 Model S 的控制原型；没有定位到目标事务，不能把其其他控制功能当作 Service Mode 支持 |
| [Tesla-Open-CAN-Mod · ce475c30ee3b](https://github.com/JelloEa/Tesla-Open-CAN-Mod/tree/ce475c30ee3b97657be41ffce412c4f9985afabe) | 没有定位到目标进入/退出实现；文档中的 Legacy Model 3 接线说明不是 Legacy S/X 协议 |
| [TeslaCANModder · 2bc62a68dda3](https://github.com/babico/TeslaCANModder/tree/2bc62a68dda3544a80c481469b3ecea5294e653d) | 检查控制源码及随附 Legacy 资料；服务模式命中包含模拟器介绍和信号数据库，没有定位到目标实车事务 |
| [Saturn_Tesla_Can_Debugger · 41c8eaecc0b5](https://github.com/canhackers/Saturn_Tesla_Can_Debugger/tree/41c8eaecc0b5e3593e9b70ec545e3f0e82d9a912) | 没有定位到目标进入/退出实现 |
| [TeslapLX · 48b2cbc9abd3](https://github.com/Akisoft41/TeslapLX/tree/48b2cbc9abd3a784076d98f7650258b7fbfc74f5) | CAN 与 Scan My Tesla 桥接项目，没有定位到目标进入/退出实现 |

本轮还检索了 GitHub 的英文/中文命令关键词、相关问题记录，以及中文、英文和俄文网页。一般车机手动操作教程、诊断软件广告、UDS 通用示例均不能充当可执行协议。

## 报文候选与排除依据

| 候选 | 来源与性质 | 本轮处理 |
| --- | --- | --- |
| `0x339 / VCSEC_serviceDiagnosticRequest` | [Atlas 的具体发送代码](https://github.com/JordanzhaoD/Atlas-single-can-firmware/blob/75ac7e72c5fd1310c5926e5f14bbd09a1861a129/src/main.cpp#L255)在第二路 BODY CAN 发送；[Model 3 VEH 定义](https://github.com/talas9/tesla-can-signal-db/blob/4850104dca0dfabb123fd7afdfe31fc5e9182efb/docs/model3/2025.20.8/veh/vcsec/vcsec_authentication.md)给出 bit 47，但标注为 layout-only | 保留为 3/Y 研究证据；没有移植到当前 Raven CAN 发送路径 |
| S/X ETH `0x339` | [2026.2 S/X Intel 数据](https://github.com/bruvv/tesla-can-explorer/blob/cb07271ace7d482b7a41a4635c33e2b954c8c8ee/data/can_frames_decoded_all_values_modelsx_intel.json)中为 `DAS_bodyControls`；CH/BDY 同名消息编号为 `0x3E9` | 证明必须同时核对车型、通道和信号，不能只靠编号判断；不能把 ETH 编号当作 X179 上的编号 |
| CH `0x128 / GTW_diagDriveState=SERVICE` | 同一 S/X 数据中为网关行驶状态里的枚举 | 是状态候选；没有发现把该状态广播当作车机进入请求的依据 |
| `MCU_carInService` / `MCU_carInServiceModePlus` | 同一数据的 `MCU_bodyControl` 等消息中存在这些信号 | 名称不能证明外部发送可触发车机，也没有形成进入/退出回执协议 |
| ETH `0x666 / GTW_diagSession` | [旧款 S/X 信号元数据](https://github.com/talas9/tesla_can_signals/blob/main/ModelX/ModelS_ETH.compact.json.json)标记为 GTW 发给 MCU 的周期消息，包含诊断等级及剩余时间 | 不能把 `LEVEL_SERVICE=25` 直接当作进入命令，更不能假定可以从 CH CAN 注入 ETH 消息 |
| UDS `0x10`、悬架 Service、雨刷 Service、模块自身 Service 工作档 | 分别属于诊断会话、部件模式或工具运行方式 | 均未接入车机 Service Mode 的成功路径 |

## 实现边界与下一份有效证据

用户本轮已确认，暂时无法取得兼容模块或维修店配合的抓包。该条件没有被当作“已授权实车试发”，本轮未刷机、未连接诊断设备、未发送 CAN，也没有用推测报文试错。

在现有公开证据下，新增一个会发送的实验按钮只会把未解决的协议问题转移到实车。当前保留已实现的只读挡位/车速观察，以及拒绝未验证请求的 API；这部分不能声称完成一键进入。

能够解除当前实现缺口的证据至少需要满足其中一种：

1. Legacy S/X MCU2 的原始控制实现或协议资料，明确请求、传输通道、版本范围和回执；
2. 同架构车辆正常进入/退出时的完整诊断事务记录，区分请求与周期广播，并明确 CAN/以太网通道；
3. 可取得的对应版本车机/网关实现，能够追到外部请求接收路径，而非只有信号名称或枚举。

即使得到候选协议，也应先离线核对编码、校验和及事务边界，再实现一次性请求、超时和模式确认，最后验证 P 挡静止下的进入与退出。源码存在、CAN 发送成功或 HTTP 成功都不能代替实车模式确认。
