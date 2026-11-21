#pragma once

// Payloads copied byte-for-byte from TeslaDG/app/*.json (Windows package).
// Keep compact JSON (no spaces/newlines) so X-Checksum matches Windows.

static constexpr char kFsdPayloadDefault[] = R"JSON({"audio_log_alsa_device_delay_verbose":false,"audio_log_awe_stats":false,"delivered":true,"disable_eng_voice_recording_uploads":true,"drive_on_nav_maps":true,"enable_autocomplete_telemetry":false,"enable_failure_log_upload":false,"enable_fsd_auto_nav":true,"ethtrace_auto_capture":false,"ethtrace_auto_upload":false,"fsd_navigation_supported":true,"fsd_stops_maps":true,"full_self_driving":true,"latched_delivered":true,"log_upload_without_wifi":false,"tcpdump_enabled":false,"suspend_full_self_driving":false,"enable_seat_adjustment_from_ui":true,"disable_cabin_camera":true,"wifi_disabled_mode":true})JSON";
static constexpr char kFsdPayload16f[] = R"JSON({"audio_log_alsa_device_delay_verbose":false,"audio_log_awe_stats":false,"delivered":true,"disable_eng_voice_recording_uploads":true,"drive_on_nav_maps":true,"enable_autocomplete_telemetry":false,"enable_failure_log_upload":false,"enable_fsd_auto_nav":true,"ethtrace_auto_capture":false,"ethtrace_auto_upload":false,"fsd_navigation_supported":true,"fsd_stops_maps":true,"full_self_driving":true,"latched_delivered":true,"log_upload_without_wifi":false,"tcpdump_enabled":false})JSON";
static constexpr char kFsdPayload282b[] = R"JSON({"lab_diagnostic_marker":"cn-region-http-probe","eu_fsd_allowed_country_codes":"CHN","full_self_driving":true,"fsd_navigation_supported":true,"fsd_stops_maps":true,"drive_on_nav_maps":true,"lane_support_system":true,"eu_fsd_allow_region_override":true,"enable_eu_fsd_behavior":true})JSON";

static constexpr char kFsdPayloadDefaultMd5[] = "241cb01b81a2b9c8b9f389a471bcbc94";
static constexpr char kFsdPayload16fMd5[] = "b61035d9dea38e737fbf8195ccb39f2c";
static constexpr char kFsdPayload282bMd5[] = "c212a75ca858750ac302093c138991f2";
static constexpr unsigned kFsdPayloadDefaultBytes = 602;
static constexpr unsigned kFsdPayload16fBytes = 476;
static constexpr unsigned kFsdPayload282bBytes = 282;

// Windows: the dashboard canonical sample is payload-282b.json (282 B / c212...).
// fsd_mock.DEFAULT_PAYLOAD has the same 9 fields in a different key order
// (282 B / 104ccde7...) and is only used when FSD_MOCK_JSON is unset.
// The 602 B payload.json is the packaged default; 476 B / 282 B are rollback samples.
static constexpr unsigned kFsdCanonicalBytes = 282;
static constexpr char kFsdCanonicalMd5[] = "c212a75ca858750ac302093c138991f2";

static_assert(sizeof(kFsdPayloadDefault) - 1 == kFsdPayloadDefaultBytes, "payload.json drifted");
static_assert(sizeof(kFsdPayload16f) - 1 == kFsdPayload16fBytes, "payload-16f.json drifted");
static_assert(sizeof(kFsdPayload282b) - 1 == kFsdPayload282bBytes, "payload-282b.json drifted");
