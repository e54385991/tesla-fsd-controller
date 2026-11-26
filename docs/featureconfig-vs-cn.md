# FeatureConfig.docx ↔ CN 下发配置对比

- 文档来源:`docs/FeatureConfig.docx`
- 文档版本:Tesla **2026.26.6.1 (ICE)**,来自 `libQtCarGUI.so.1.0.0`
  `FeatureConfigs::initialize() @ 0x257bd40`
- 文档规模:注册条目 300 / 唯一 key 297;表格可解析出 **294 条带类型行、291 个唯一 key**
  (其中 `firmware_program` 重复 3 次、`webcam_disabled` 重复 2 次)
- 对比对象:`kFsdPayloadCnFull`(CN 全量预设,**62 字段 / 2020 B**,
  md5 `1e4f78a38afb0c5adaf775683d6157d6`)

> 列含义:FS=FeatureSupport(0 不支持 / 1 bool / 2 数值或字符串);
> Rst=RestartBehavior(1=需重启);Latch=FeatureSentinel 行为。

## 1. 覆盖情况

62 个 CN 字段中 **58 个**能在 2026.26.6.1 表中找到;**4 个不在表内**:

| CN 字段 | 说明 |
|---|---|
| `delivered` | 交付/激活标记,不属于 FeatureConfigs 注册项 |
| `latched_delivered` | 锁存“已交付”状态,同上 |
| `enable_fsd_auto_nav` | 2026.26.6.1 无此 key(旧版本/兼容字段);对应能力由 `fsd_navigation_supported`(`hasFullSelfDrivingNavMaps`)与 `drive_on_nav_maps`(`dasDriveOnNavMapsOkay`)承担 |
| `enable_traffic_light_countdown` | 2026.26.6.1 表中无此 key;GUI 可能通过其他开关控制 |

## 2. 类型 / 语义注意点

### 2.1 两个“反向 + 字符串”字段

| CN 字段 | 实际 FEATURE | 类型 | 取值 |
|---|---|---|---|
| `disable_start_fsd_from_park` | `enableStartFsdFromPark` | FS=2 | 字符串 `"0"` = 允许停车启动 |
| `disable_actually_smart_summon` | `actuallySmartSummonEnabled` | FS=2 | 字符串 `"0"` = 启用真正智能召唤 |

它们是**取反键**,且值必须是字符串 `"0"`/`"1"`,不能写成布尔 `false`/`true`。

### 2.2 字符串型

- `eu_fsd_allowed_country_codes`:FS=2,latch=4,当前值 `"CHN"`

### 2.3 此版本不支持(FS=0)

- `enable_tesla_lane_position_algo`(`enableTeslaLanePositionAlgo`,仅注册,FS=0)
  —— 配置中虽为 `true`,当前版本不会生效。

### 2.4 键名与 FEATURE 名不一致的字段(易误读)

| CN 字段 | FEATURE |
|---|---|
| `fsd_navigation_supported` | `hasFullSelfDrivingNavMaps` |
| `fsd_stops_maps` | `dasHasFSDStopsMaps` |
| `drive_on_nav_maps` | `dasDriveOnNavMapsOkay` |
| `disable_cabin_camera` | `cabinCameraDisabled` |
| `ulc_without_confirm` | `dasNoConfirmULCEnabled` |
| `autoshift_dr` | `autoshiftDREnabled` |
| `enable_seat_adjustment_from_ui` | `enableSeatAdjustment` |
| `enable_use_way_lane_prob` | `useWayLaneProb` |
| `das_acm_override_fleet_speed` | `dasACMOverrideFleetSpeed` |
| `route_avoid_construction` | `enableRouteAvoidConstruction` |
| `disable_r79_restrictions` | `disableR79Restrictions` |
| `disable_passenger_play` | `disablePassengerPlay` |

> `drive_on_nav_maps` 与 `drive_on_nav_disabled` 是**两条不同的项**:
> 前者 `dasDriveOnNavMapsOkay`,后者 `dasDriveOnNavEnabled`(FS=0)。

### 2.5 需要重启(Rst=1)的 CN 字段(42 个)

```text
  audio_log_alsa_device_delay_verbose audio_log_awe_stats disable_eng_voice_recording_uploads
  enable_autocomplete_telemetry enable_failure_log_upload ethtrace_auto_capture
  ethtrace_auto_upload fsd_navigation_supported full_self_driving
  log_upload_without_wifi tcpdump_enabled suspend_full_self_driving
  disable_cabin_camera wifi_disabled_mode disable_online_routing
  disable_alternative_trip_plans enable_alternative_routes enable_navigation_mpp_support
  enable_nav_voice_download eu_fsd_allow_region_override autopilot_telemetry_in_china
  eu_fsd_allowed_country_codes enable_alternative_routes_when_driving lane_support_system
  autopilot_monarch_enabled autopilot_monarch_backup autopilot_snapshots
  ulc_without_confirm enable_speed_limit_from_signs force_valhalla_speed_limits
  disable_map_manager_internal_reroute enable_city_streets_reroute enable_use_way_lane_prob
  enable_latest_map_support disable_avoid_uturn enable_trigger_off_route_reroute
  enable_vendor_maneuver enable_proactive_reroutes enable_better_route_available
  route_avoid_construction enable_junction_view enable_speed_cam_alerts
```

其余 20 个字段为立即生效。

## 3. CN 配置未包含、但表内存在的相关项

### 3.1 可用(FS=1/2)且可能与 FSD/ADAS 相关

| key | FEATURE | FS | Rst | 说明 |
|---|---|---:|---:|---|
| `force_disable_mpp` | `forceDisableMPP` | 1 | 1 | 强制关闭 MPP;与 `enable_navigation_mpp_support` 相关 |
| `force_online_mpp` | `forceOnlineMPP` | 1 | 1 | 强制在线 MPP |
| `autopilot_stop_alerting` | `autopilotStopAlerting` | 2 | 1 | AP 停车提醒(数值/字符串,不是布尔) |
| `enable_autopilot_air_purge` | `enableAutopilotAirPurge` | 1 | 1 | AP 空气净化 |
| `enable_coolant_das_target` | `enableCoolantDasTarget` | 1 | 1 | DAS 冷却目标 |
| `speed_limit` | `speedLimit` | 2 | 1 | 限速值(latch=4) |
| `speed_limit_chill_override` | `speedLimitChillOverride` | 2 | 1 | 限速 chill 覆盖(latch=4) |

导航类可选(均 FS=1):`enable_prefetch_reroute`(Rst=1)、
`enable_reroute_buttons`、`enable_reroute_near_destination`(Rst=1)、
`disable_reroute_near_destination`(Rst=1)。

### 3.2 此版本不支持(FS=0),下发无效

```
autopilot_ephemeris_enabled          enable_vision_speed_limit_control
drive_on_nav_disabled                lane_departure_warning_disabled
park_assist_disabled                 disable_google_semantic_traffic
satellite/traffic/vector_map_tiles_disabled
```

### 3.3 文档本身重复的 key(解析时需去重)

`firmware_program`(3 条)、`webcam_disabled`(2 条)。

## 4. 已按本文档校正的 UI 注释

`include/web_ui_fsd.h` 的字段说明已改为引用 FEATURE 名,并标注需重启项与取反键,例如:

- `drive_on_nav_maps` → “允许在导航地图上启用 FSD(dasDriveOnNavMapsOkay)”
- `fsd_navigation_supported` → “FSD 导航地图支持(hasFullSelfDrivingNavMaps,需重启)”
- `ulc_without_confirm` → “ULC 变道免确认(dasNoConfirmULCEnabled,需重启)”
- `disable_start_fsd_from_park` → “禁止停车启动 FSD;字符串 "0"=允许(取反键 enableStartFsdFromPark)”
- `eu_fsd_allowed_country_codes` → “欧盟 FSD 允许国家代码,字符串如 "CHN"(euFsdAllowedCountryCodes)”
- `enable_tesla_lane_position_algo` → 标注“此版本 FS=0 不支持”
- `enable_traffic_light_countdown` → 标注“2026.26.6.1 表中无此键”

`delivered` / `latched_delivered` / `enable_fsd_auto_nav` 已注明“非 FeatureConfigs 项/兼容字段”。
