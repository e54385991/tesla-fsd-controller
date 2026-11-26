#pragma once

// Payloads copied byte-for-byte from TeslaDG/app/*.json (Windows package).
// Keep compact JSON (no spaces/newlines) so X-Checksum matches Windows.
//
// kFsdPayloadDefault is the ESP32 default: the Windows 20-field payload plus
// "disable_cabin_camera":false, "enable_eu_fsd_behavior":false, and
// "autoshift_dr":true as the LAST key (654 B).
// kFsdPayloadPc is the untouched TeslaDG/app/payload.json (602 B, where
// disable_cabin_camera is true and is available as an explicit preset).
// kFsdPayloadCnFull is the user-supplied CN payload (62 fields, 2020 B) exposed
// as an explicit preset; field order is kept verbatim for X-Checksum parity.

static constexpr char kFsdPayloadPc[] = R"JSON({"audio_log_alsa_device_delay_verbose":false,"audio_log_awe_stats":false,"delivered":true,"disable_eng_voice_recording_uploads":true,"drive_on_nav_maps":true,"enable_autocomplete_telemetry":false,"enable_failure_log_upload":false,"enable_fsd_auto_nav":true,"ethtrace_auto_capture":false,"ethtrace_auto_upload":false,"fsd_navigation_supported":true,"fsd_stops_maps":true,"full_self_driving":true,"latched_delivered":true,"log_upload_without_wifi":false,"tcpdump_enabled":false,"suspend_full_self_driving":false,"enable_seat_adjustment_from_ui":true,"disable_cabin_camera":true,"wifi_disabled_mode":true})JSON";
static constexpr char kFsdPayloadDefault[] = R"JSON({"audio_log_alsa_device_delay_verbose":false,"audio_log_awe_stats":false,"delivered":true,"disable_eng_voice_recording_uploads":true,"drive_on_nav_maps":true,"enable_autocomplete_telemetry":false,"enable_failure_log_upload":false,"enable_fsd_auto_nav":true,"ethtrace_auto_capture":false,"ethtrace_auto_upload":false,"fsd_navigation_supported":true,"fsd_stops_maps":true,"full_self_driving":true,"latched_delivered":true,"log_upload_without_wifi":false,"tcpdump_enabled":false,"suspend_full_self_driving":false,"enable_seat_adjustment_from_ui":true,"disable_cabin_camera":false,"wifi_disabled_mode":true,"enable_eu_fsd_behavior":false,"autoshift_dr":true})JSON";
static constexpr char kFsdPayload16f[] = R"JSON({"audio_log_alsa_device_delay_verbose":false,"audio_log_awe_stats":false,"delivered":true,"disable_eng_voice_recording_uploads":true,"drive_on_nav_maps":true,"enable_autocomplete_telemetry":false,"enable_failure_log_upload":false,"enable_fsd_auto_nav":true,"ethtrace_auto_capture":false,"ethtrace_auto_upload":false,"fsd_navigation_supported":true,"fsd_stops_maps":true,"full_self_driving":true,"latched_delivered":true,"log_upload_without_wifi":false,"tcpdump_enabled":false})JSON";
static constexpr char kFsdPayload282b[] = R"JSON({"lab_diagnostic_marker":"cn-region-http-probe","eu_fsd_allowed_country_codes":"CHN","full_self_driving":true,"fsd_navigation_supported":true,"fsd_stops_maps":true,"drive_on_nav_maps":true,"lane_support_system":true,"eu_fsd_allow_region_override":true,"enable_eu_fsd_behavior":true})JSON";

static constexpr char kFsdPayloadPcMd5[] = "241cb01b81a2b9c8b9f389a471bcbc94";
static constexpr char kFsdPayloadDefaultMd5[] = "bd262907ff9fb2709bb54f9d3e78d07d";
static constexpr char kFsdPayload16fMd5[] = "b61035d9dea38e737fbf8195ccb39f2c";
static constexpr char kFsdPayloadCnFull[] = R"JSON({"audio_log_alsa_device_delay_verbose":false,"audio_log_awe_stats":false,"delivered":true,"disable_eng_voice_recording_uploads":true,"drive_on_nav_maps":true,"enable_autocomplete_telemetry":false,"enable_failure_log_upload":false,"enable_fsd_auto_nav":true,"ethtrace_auto_capture":false,"ethtrace_auto_upload":false,"fsd_navigation_supported":true,"fsd_stops_maps":true,"full_self_driving":true,"latched_delivered":true,"log_upload_without_wifi":false,"tcpdump_enabled":false,"suspend_full_self_driving":false,"enable_seat_adjustment_from_ui":true,"disable_cabin_camera":false,"wifi_disabled_mode":true,"enable_eu_fsd_behavior":false,"enable_tesla_tts_service":true,"enable_active_traffic_on_route":true,"disable_online_routing":false,"disable_alternative_trip_plans":false,"enable_alternative_routes":true,"enable_navigation_mpp_support":true,"enable_nav_voice_download":true,"autoshift_dr":true,"tesla_autopark_disabled":false,"eu_fsd_allow_region_override":true,"disable_r79_restrictions":true,"autopilot_telemetry_in_china":true,"eu_fsd_allowed_country_codes":"CHN","disable_actually_smart_summon":"0","disable_start_fsd_from_park":"0","enable_alternative_routes_when_driving":true,"lane_support_system":true,"autopilot_monarch_enabled":true,"autopilot_monarch_backup":true,"autopilot_snapshots":true,"ulc_without_confirm":true,"das_acm_override_fleet_speed":true,"enable_speed_limit_from_signs":true,"force_valhalla_speed_limits":true,"enable_average_speed_zone":true,"use_map_matched_car_location":true,"disable_map_manager_internal_reroute":false,"enable_city_streets_reroute":true,"enable_use_way_lane_prob":true,"enable_latest_map_support":true,"disable_avoid_uturn":true,"enable_traffic_light_countdown":true,"enable_tesla_lane_position_algo":true,"enable_trigger_off_route_reroute":true,"enable_vendor_maneuver":true,"enable_proactive_reroutes":true,"enable_better_route_available":true,"route_avoid_construction":true,"enable_junction_view":true,"enable_speed_cam_alerts":true,"disable_passenger_play":false})JSON";
// Previous CN preset (46 fields, 1466 B) kept alongside the 62-field CN payload
// so users can roll back to the older field set.
static constexpr char kFsdPayloadCn46[] = R"JSON({"audio_log_alsa_device_delay_verbose":false,"audio_log_awe_stats":false,"delivered":true,"disable_eng_voice_recording_uploads":true,"drive_on_nav_maps":true,"enable_autocomplete_telemetry":false,"enable_failure_log_upload":false,"enable_fsd_auto_nav":true,"ethtrace_auto_capture":false,"ethtrace_auto_upload":false,"fsd_navigation_supported":true,"fsd_stops_maps":true,"full_self_driving":true,"latched_delivered":true,"log_upload_without_wifi":false,"tcpdump_enabled":false,"suspend_full_self_driving":false,"enable_seat_adjustment_from_ui":true,"disable_cabin_camera":false,"wifi_disabled_mode":true,"lane_support_system":true,"enable_eu_fsd_behavior":false,"enable_active_traffic_on_route":true,"disable_online_routing":false,"enable_tesla_tts_service":true,"autoshift_dr":true,"autopilot_monarch_enabled":true,"autopilot_monarch_backup":true,"autopilot_snapshots":true,"ulc_without_confirm":true,"das_acm_override_fleet_speed":true,"enable_speed_limit_from_signs":true,"force_valhalla_speed_limits":true,"enable_average_speed_zone":true,"use_map_matched_car_location":true,"autopilot_telemetry_in_china":true,"disable_map_manager_internal_reroute":false,"enable_city_streets_reroute":true,"enable_use_way_lane_prob":true,"enable_latest_map_support":true,"disable_start_fsd_from_park":"0","disable_actually_smart_summon":"0","disable_avoid_uturn":true,"enable_navigation_mpp_support":true,"eu_fsd_allow_region_override":true,"eu_fsd_allowed_country_codes":"CHN"})JSON";
static constexpr char kFsdPayloadCnFullMd5[] = "1e4f78a38afb0c5adaf775683d6157d6";
static constexpr char kFsdPayloadCn46Md5[] = "0ea421e01be6b6e8f66a8fb752970bb3";
static constexpr unsigned kFsdPayloadPcBytes = 602;
static constexpr unsigned kFsdPayloadDefaultBytes = 654;
static constexpr unsigned kFsdPayload16fBytes = 476;
static constexpr unsigned kFsdPayload282bBytes = 282;
static constexpr unsigned kFsdPayloadCnFullBytes = 2020;
static constexpr unsigned kFsdPayloadCn46Bytes = 1466;

// Windows: the dashboard canonical sample is payload-282b.json (282 B / c212...).
// fsd_mock.DEFAULT_PAYLOAD has the same 9 fields in a different key order
// (282 B / 104ccde7...) and is only used when FSD_MOCK_JSON is unset.
// The 602 B payload.json is the packaged Windows default; 476 B / 282 B are rollback samples.
static constexpr unsigned kFsdCanonicalBytes = 282;
static constexpr char kFsdCanonicalMd5[] = "c212a75ca858750ac302093c138991f2";

static_assert(sizeof(kFsdPayloadPc) - 1 == kFsdPayloadPcBytes, "payload.json drifted");
static_assert(sizeof(kFsdPayloadDefault) - 1 == kFsdPayloadDefaultBytes, "default payload drifted");
static_assert(sizeof(kFsdPayload16f) - 1 == kFsdPayload16fBytes, "payload-16f.json drifted");
static_assert(sizeof(kFsdPayload282b) - 1 == kFsdPayload282bBytes, "payload-282b.json drifted");
static_assert(sizeof(kFsdPayloadCnFull) - 1 == kFsdPayloadCnFullBytes, "CN full payload drifted");
static_assert(sizeof(kFsdPayloadCn46) - 1 == kFsdPayloadCn46Bytes, "CN 46-field payload drifted");
