#!/usr/bin/env python3
"""Run DNS policy regression tests without hardware or PlatformIO.

Compile the production functions unchanged with small host adapters; generated
sources and binaries stay in a temporary directory. Run: python3 tests/run_dns_checks.py
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "include/mod_dns.h").read_text()


def function(signature):
    start = SOURCE.index(signature)
    brace = SOURCE.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


PREAMBLE = r'''
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>
struct String : std::string {
    using std::string::string;
    String(const std::string& value) : std::string(value) {}
    bool isEmpty() const { return empty(); }
    bool startsWith(const char* prefix) const { return rfind(prefix, 0) == 0; }
    bool endsWith(const char* suffix) const {
        size_t n = std::strlen(suffix);
        return size() >= n && compare(size() - n, n, suffix) == 0;
    }
    void trim() {
        while (!empty() && std::isspace(static_cast<unsigned char>(back()))) pop_back();
        erase(begin(), std::find_if_not(begin(), end(), [](unsigned char c) { return std::isspace(c); }));
    }
    void toLowerCase() { for (char& c : *this) c = std::tolower(static_cast<unsigned char>(c)); }
    void remove(size_t index) { erase(index); }
    void toCharArray(char* dest, size_t capacity) const {
        size_t n = std::min(size(), capacity - 1);
        std::memcpy(dest, c_str(), n); dest[n] = '\0';
    }
};
struct DNSFilterConfig {
    bool enabled;
    char allowlist[3072] = {}, blocklist[3072] = {};
    DNSFilterConfig(bool enabled, const char* allow, const char* block) : enabled(enabled) {
        if (allow) std::snprintf(allowlist, sizeof(allowlist), "%s", allow);
        if (block) std::snprintf(blocklist, sizeof(blocklist), "%s", block);
    }
};
class DNSWhitelistServer {
  public:
    bool hasDomainRules(const char* rules) const;
    bool isAllowedDomainCstr(const DNSFilterConfig& cfg, const char* domain, size_t domainLen) const;
    bool isAllowedDomain(const DNSFilterConfig& cfg, const String& domain) const;
};
struct IPAddress { uint32_t value = 0; };
struct FakeWiFi {
    std::vector<std::string> queries;
    int hostByName(const char* domain, IPAddress& ip) {
        queries.emplace_back(domain); ip.value = queries.size(); return 1;
    }
} WiFi;
uint32_t ipAddressToHostOrder(const IPAddress& ip) { return ip.value; }
std::vector<std::string> remembered;
void rememberResolvedIp(const String& domain, uint32_t) { remembered.push_back(domain); }
constexpr uint16_t kDnsHeaderSize = 12;
constexpr uint16_t kDnsTypeOPT = 41;
constexpr size_t kStagingCapacity = 64;
constexpr size_t kBlockedIpCapacity = 256;
constexpr uint32_t kIpEntryTtlSeconds = 3600;
constexpr size_t kAllowedIpCapacity = 256;
constexpr uint32_t kAllowedIpIdleTtlSeconds = 1800;
constexpr uint32_t kAllowedIpTouchIntervalSeconds = 30;
enum class DnsIpPolicyMode : uint8_t { Off = 0, BlocklistIps = 1, StrictAllowIps = 2 };
struct CachedIpEntry { uint32_t ipHostOrder = 0; uint32_t lastSeenAtSeconds = 0; };
CachedIpEntry gStagingIps[kStagingCapacity];
CachedIpEntry gBlockedIps[kBlockedIpCapacity];
size_t gBlockedIpCount = 0;
CachedIpEntry gAllowedIps[kAllowedIpCapacity];
size_t gAllowedIpCount = 0;
volatile uint32_t gDnsApNetHost = 0x09090900u;
volatile uint32_t gDnsApMaskHost = 0xFFFFFF00u;
volatile uint8_t gDnsFilterActiveFlag = 0;
volatile uint8_t gDnsIpPolicyMode = 0;
volatile bool gBlockedTableDirty = false;
volatile uint32_t gIpForwardDropCount = 0;
uint32_t gTestNowMs = 0;
uint32_t millis() { return gTestNowMs; }
constexpr size_t kDnsCacheCapacity = 32;
constexpr uint32_t kDnsCacheMinTtlSeconds = 30;
constexpr uint32_t kDnsCacheMaxTtlSeconds = 300;
struct DnsCacheEntry { char domain[96] = {}; uint32_t ipv4Nbo = 0; uint32_t expiresAtSec = 0; uint32_t lastUsedSec = 0; };
DnsCacheEntry gDnsCache[kDnsCacheCapacity];
#ifndef portENTER_CRITICAL
#define portENTER_CRITICAL(mux) do {} while (0)
#define portEXIT_CRITICAL(mux) do {} while (0)
#endif
#ifndef strlcpy
#define strlcpy(dest, src, size) std::snprintf(dest, size, "%s", src)
#endif
constexpr size_t kPendingBlockResolveCap = 8;
char gPendingBlockResolve[kPendingBlockResolveCap][128] = {};
size_t gPendingBlockHead = 0;
size_t gPendingBlockCount = 0;
int gDnsCacheMux = 0;
int gDnsIpPolicyMux = 0;
int gDnsAllowedIpMux = 0;
'''

STRICT_PREAMBLE = r'''
constexpr uint32_t kDnsTtlSeconds = 60;
struct AllowedAddress { uint32_t ip; uint32_t issuedAt; };
int gDnsAllowMux = 0;
AllowedAddress gAllowedAddresses[128] = {};
uint32_t gDnsRulesEpoch = 0;
uint8_t gApNetifIndex = 0;
uint8_t gIngressNetifIndex = 0;
bool destinationAllowed(const AllowedAddress* addresses, size_t count, uint32_t ip, uint32_t now);
'''

TESTS = r'''
int main() {
    DNSWhitelistServer dns;
    unsigned checks = 0;
#ifdef WIFI_BRIDGE_STRICT_ALLOWLIST
    constexpr bool allowUnlisted = false;
#else
    constexpr bool allowUnlisted = true;
#endif
    auto expect = [&](const char* allow, const char* block, const char* domain, bool allowed) {
        DNSFilterConfig config{true, allow, block};
        bool actual = dns.isAllowedDomain(config, normalizeDomainCstr(domain));
        if (actual != allowed) {
            std::fprintf(stderr, "allow=[%s] block=[%s] domain=%s: expected %d, got %d\n",
                         allow, block, domain, allowed, actual);
            std::abort();
        }
        ++checks;
    };
    // Bare domains retain apex + arbitrary-depth subdomain matching.
    for (const char* name : {"qq.com", "music.qq.com", "a.b.qq.com"})
        expect("qq.com", "", name, true);
    // Doubao / Apple Music defaults: bare domains cover every subdomain.
    for (const char* name : {"doubao.com", "www.doubao.com", "p3-sign.doubao.com",
                             "butterflyai.cn", "lf-cdn-tos.bytescm.com",
                             "music.apple.com", "aod-ssl.itunes.apple.com",
                             "is1-ssl.mzstatic.com"})
        expect("doubao.com butterflyai.cn bytescm.com *.apple.com mzstatic.com", "", name, true);
    expect("doubao.com", "", "fakedoubao.com", false);
    expect("*.apple.com", "", "fakeapple.com", false);
    expect("*.apple.com", "", "apple.com", false);
    // QQ Music / NetEase Cloud Music parent-domain coverage.
    for (const char* name : {"dl.stream.qqmusic.qq.com", "isure.stream.qqmusic.qq.com"})
        expect("qqmusic.qq.com", "", name, true);
    expect("iplay.163.com", "", "api.iplay.163.com", true);
    expect("music.163.com", "", "interface.music.163.com", true);
    expect("qlogo.cn", "", "thirdqq.qlogo.cn", true);
    expect("qqmusic.qq.com", "", "fakeqqmusic.qq.com", false);
    // Consolidated parent domains (v5): Baidu, QQ, NetEase.
    for (const char* name : {"api.map.baidu.com", "itsmap3.baidu.com"})
        expect("baidu.com", "", name, true);
    expect("bdimg.com", "", "mapapip0.bdimg.com", true);
    expect("bcebos.com", "", "lbsnavi.cdn.bcebos.com", true);
    expect("qq.com", "", "aqqmusic.tc.qq.com", true);
    expect("163.com", "", "interface.music.163.com", true);
    expect("126.net", "", "m701.music.126.net", true);
    expect("netease.com", "", "mam.netease.com", true);
    // Wildcards include all child depths, while respecting domain boundaries.
    for (const char* name : {"music.qq.com", "a.b.qq.com", "a.b.c.qq.com"})
        expect("*.qq.com", "", name, true);
    for (const char* name : {"qq.com", "fakeqq.com", "a.fakeqq.com", "qq.com.evil", "com", ""})
        expect("*.qq.com", "", name, false);
    // Both rule forms support the existing separators and normalization.
    expect("\t*.QQ.COM.\n", "", "A.B.QQ.COM.", true);
    expect("example.com,*.qq.com; qpic.cn\n*.gtimg.com", "", "a.b.qq.com", true);
    expect("example.com,*.qq.com; qpic.cn\n*.gtimg.com", "", "img.gtimg.com", true);
    expect("qq.com *.qq.com", "", "qq.com", true);
    expect("qq.com *.qq.com", "", "a.b.qq.com", true);
    // Nonempty allowlist = default deny: every unmatched name is blocked.
    expect("example.com", "", "example.com", true);
    expect("example.com", "", "music.example.com", true);
    expect("example.com", "", "other.com", false);
    expect("example.com", "", "example.com.evil", false);
    expect("*.example.com", "", "example.com", false);
    expect("*.example.com", "", "a.example.com", true);
    // Empty allowlist = blacklist-only mode; unmatched names stay allowed.
    expect("", "blocked.com", "other.com", allowUnlisted);
    expect("", "blocked.com", "sub.blocked.com", false);
    // Disabling the filter bypasses both lists (normal mode). Strict allowlist
    // builds are intentionally fail-closed even with filtering disabled.
#ifndef WIFI_BRIDGE_STRICT_ALLOWLIST
    assert(dns.isAllowedDomain({false, "example.com", "blocked.com"}, "other.com"));
#endif
    // Wildcard blacklists exclude the apex and allow unrelated domains.
    expect("", "*.qq.com", "qq.com", allowUnlisted);
    expect("", "*.qq.com", "music.qq.com", false);
    expect("", "*.qq.com", "a.b.qq.com", false);
    expect("", "*.qq.com", "fakeqq.com", allowUnlisted);
    // The wildcard's two syntax characters must not inflate specificity.
    expect("*.qq.com", "a.qq.com", "a.qq.com", false);
    expect("*.qq.com", "a.qq.com", "child.a.qq.com", false);
    expect("a.qq.com", "*.qq.com", "a.qq.com", true);
    expect("a.qq.com", "*.qq.com", "child.a.qq.com", true);
    expect("*.qq.com", "*.a.qq.com", "child.a.qq.com", false);
    expect("*.a.qq.com", "*.qq.com", "child.a.qq.com", true);
    expect("*.qq.com", "qq.com", "music.qq.com", true);
    expect("qq.com", "*.qq.com", "music.qq.com", true);
    expect("*.qq.com", "*.qq.com", "music.qq.com", true);
    expect("qq.com", "qq.com", "qq.com", true);
    // No unrestricted wildcard or malformed empty suffix is introduced.
    expect("*", "", "music.qq.com", false);
    expect("*.", "", "music.qq.com", false);
    expect("*.qq.com", "", "example.com", false);
    assert(!dns.isAllowedDomain({true, nullptr, nullptr}, ""));
#ifdef WIFI_BRIDGE_STRICT_ALLOWLIST
    expect("", "", "example.com", false);
    assert(!dns.isAllowedDomain({false, "example.com", ""}, "example.com"));
    constexpr uint32_t approved = 0xcb00712a;
    constexpr uint32_t unknown = 0xc633642a;
    dnsIpPolicyRecordIngress(2);
    assert(dnsHookIp4CanForward(unknown) == 0);
    assert(!rememberAllowedAddress(approved, gDnsRulesEpoch));
    dnsStrictSetApInterface(2);
    assert(dnsHookIp4CanForward(approved) == 0);
    assert(rememberAllowedAddress(approved, gDnsRulesEpoch));
    assert(dnsHookIp4CanForward(approved) == -1);
    assert(dnsHookIp4CanForward(unknown) == 0);
    assert(dnsHookIp4CanForward(0x2a7100cb) == 0);
    dnsIpPolicyRecordIngress(3);
    assert(dnsHookIp4CanForward(unknown) == -1);
    dnsIpPolicyRecordIngress(2);
    gTestNowMs = 59999;
    assert(dnsHookIp4CanForward(approved) == -1);
    gTestNowMs = 60000;
    assert(dnsHookIp4CanForward(approved) == 0);
    assert(rememberAllowedAddress(approved, gDnsRulesEpoch));
    uint32_t staleEpoch = gDnsRulesEpoch;
    DNSFilterConfig rules{true, "example.com", ""};
    dnsStrictUpdateRules(rules, true, "example.com", "");
    assert(gDnsRulesEpoch == staleEpoch);
    dnsStrictUpdateRules(rules, true, "other.example", nullptr);
    assert(std::strcmp(rules.allowlist, "other.example") == 0);
    assert(!rememberAllowedAddress(approved, staleEpoch));
    assert(dnsHookIp4CanForward(approved) == 0);
    staleEpoch = gDnsRulesEpoch;
    dnsStrictClearAddresses();
    assert(!rememberAllowedAddress(approved, staleEpoch));
    assert(dnsHookIp4CanForward(approved) == 0);
    std::puts("Strict IPv4 policy: ingress, unknown IP, TTL and invalidation checks passed");
#else
    expect("", "", "example.com", true);
    assert(dns.isAllowedDomain({false, "*.qq.com", "qq.com"}, "music.qq.com"));

    // Runtime L3 security checks: external DNS:53 / DoT:853 blocking + 3-mode IP policy.
    auto makeIpUdpTcp = [](uint32_t srcHost, uint32_t dstHost, uint8_t proto, uint16_t dport) {
        std::vector<uint8_t> pkt(28, 0);
        pkt[0] = 0x45; // IPv4, IHL=20
        pkt[9] = proto;
        pkt[12] = (srcHost >> 24) & 0xFF; pkt[13] = (srcHost >> 16) & 0xFF;
        pkt[14] = (srcHost >> 8) & 0xFF;  pkt[15] = srcHost & 0xFF;
        pkt[16] = (dstHost >> 24) & 0xFF; pkt[17] = (dstHost >> 16) & 0xFF;
        pkt[18] = (dstHost >> 8) & 0xFF;  pkt[19] = dstHost & 0xFF;
        pkt[22] = (dport >> 8) & 0xFF;    pkt[23] = dport & 0xFF;
        return pkt;
    };
    dnsIpPolicySetApSubnet(0x09090909u, 0xFFFFFF00u);
    dnsIpPolicySetFilterEnabled(1);
    dnsIpPolicySetMode(0);
    auto apToDns53 = makeIpUdpTcp(0x0909090Au, 0x08080808u, 17, 53);
    auto apToTcp53 = makeIpUdpTcp(0x0909090Au, 0x01010101u, 6, 53);
    auto apToDot853 = makeIpUdpTcp(0x0909090Au, 0x01010101u, 6, 853);
    auto apToHttps443 = makeIpUdpTcp(0x0909090Au, 0x01010101u, 6, 443);
    auto staReturn = makeIpUdpTcp(0x08080808u, 0x0909090Au, 17, 53);
    assert(dnsHookIp4CanForwardPacket(apToDns53.data(), apToDns53.size(), 0x08080808u) == 0);
    assert(dnsHookIp4CanForwardPacket(apToTcp53.data(), apToTcp53.size(), 0x01010101u) == 0);
    assert(dnsHookIp4CanForwardPacket(apToDot853.data(), apToDot853.size(), 0x01010101u) == 0);
    assert(dnsHookIp4CanForwardPacket(apToHttps443.data(), apToHttps443.size(), 0x01010101u) == -1);
    // Inbound STA->AP return packet is bypassed lock-free.
    assert(dnsHookIp4CanForwardPacket(staReturn.data(), staReturn.size(), 0x0909090Au) == -1);
    // Disabling DNS filter stops dropping 53/853.
    dnsIpPolicySetFilterEnabled(0);
    assert(dnsHookIp4CanForwardPacket(apToDns53.data(), apToDns53.size(), 0x08080808u) == -1);

    // Runtime Mode 2 (StrictAllowIps): drops unknown/hardcoded IPs, allows resolved IPs with 30m sliding TTL.
    dnsIpPolicySetFilterEnabled(1);
    dnsIpPolicySetMode(2);
    dnsAllowedIpClearInternal();
    gTestNowMs = 10000; // t = 10s
    assert(dnsHookIp4CanForwardPacket(apToHttps443.data(), apToHttps443.size(), 0x01010101u) == 0);
    dnsAllowedIpRemember(0x01010101u, 10);
    assert(dnsHookIp4CanForwardPacket(apToHttps443.data(), apToHttps443.size(), 0x01010101u) == -1);
    // Active traffic refreshes sliding TTL (>30s later keeps it alive past original 1800s window).
    gTestNowMs = 1000000; // t = 1000s
    assert(dnsHookIp4CanForwardPacket(apToHttps443.data(), apToHttps443.size(), 0x01010101u) == -1);
    gTestNowMs = 2500000; // t = 2500s (1500s since refresh < 1800s -> still alive!)
    assert(dnsHookIp4CanForwardPacket(apToHttps443.data(), apToHttps443.size(), 0x01010101u) == -1);
    gTestNowMs = 4400000; // t = 4400s (>1800s idle -> expired)
    assert(dnsHookIp4CanForwardPacket(apToHttps443.data(), apToHttps443.size(), 0x01010101u) == 0);
    dnsIpPolicySetMode(0);
    gTestNowMs = 0;
#endif
    // Neither the full nor incremental IP refresh resolves a wildcard/apex
    // substitute; only literal rules and concrete blocked hosts are resolved.
    assert(buildStagingFiltered(nullptr, "*.qq.com qq.com *.tesla.cn api.example.com", 42) == 2);
    assert((WiFi.queries == std::vector<std::string>{"qq.com", "api.example.com"}));
    assert(gStagingIps[0].ipHostOrder == 1);
    assert(gStagingIps[0].lastSeenAtSeconds == 42);
    assert(gStagingIps[1].ipHostOrder == 2);
    // Allowlist-over-blocklist precedence: a blocklist host overridden by an equal/longer
    // allowlist entry must NOT be added to L3 blocked IPs.
    WiFi.queries.clear();
    assert(buildStagingFiltered("api-prd.vn.cloud.tesla.cn",
                                "api-prd.vn.cloud.tesla.cn mothership.vn.teslamotors.com", 50) == 1);
    assert((WiFi.queries == std::vector<std::string>{"mothership.vn.teslamotors.com"}));
    static_assert(sizeof(CachedIpEntry) == 8, "CachedIpEntry must stay compact (8 B)");
    WiFi.queries.clear();
    resolveAndRememberDomain("*.qq.com");
    resolveAndRememberDomain("");
    assert(WiFi.queries.empty() && remembered.empty());
    resolveAndRememberDomain("music.qq.com");
    assert((WiFi.queries == std::vector<std::string>{"music.qq.com"}));
    assert((remembered == std::vector<std::string>{"music.qq.com"}));

    // Strict zero-heap DNS QNAME parser rejects embedded NUL, whitespace, dots, and compression pointers.
    {
        char out[256];
        size_t off = 0, len = 0;
        const uint8_t okName[] = {2, 'Y', 'y', 2, 'q', 'q', 3, 'C', 'O', 'M', 0};
        assert(parseDomainNameToBuf(okName, sizeof(okName), off, out, sizeof(out), len));
        assert(len == 9 && std::strcmp(out, "yy.qq.com") == 0 && off == sizeof(okName));

        off = 0;
        const uint8_t badSpace[] = {7, 'q', 'q', '.', 'c', 'o', 'm', ' ', 0};
        assert(!parseDomainNameToBuf(badSpace, sizeof(badSpace), off, out, sizeof(out), len));
        off = 0;
        const uint8_t badDot[] = {6, 'q', 'q', '.', 'c', 'o', 'm', 0};
        assert(!parseDomainNameToBuf(badDot, sizeof(badDot), off, out, sizeof(out), len));
        off = 0;
        const uint8_t badNul[] = {3, 'a', '\0', 'b', 0};
        assert(!parseDomainNameToBuf(badNul, sizeof(badNul), off, out, sizeof(out), len));
        off = 0;
        const uint8_t badPtr[] = {0xC0, 0x0C};
        assert(!parseDomainNameToBuf(badPtr, sizeof(badPtr), off, out, sizeof(out), len));
    }

    // EDNS0 UDP payload clamp + Question echo verification.
    {
        // Header (12) + QNAME "a.b" (5) + QTYPE/QCLASS (4) + OPT RR (11)
        uint8_t pkt[32] = {
            0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
            1, 'A', 1, 'b', 0, 0x00, 0x01, 0x00, 0x01,
            0x00, 0x00, 41, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00  // OPT UDP=4096
        };
        clampEdnsUdpPayload(pkt, sizeof(pkt), 21, 1024);
        assert(readU16(pkt + 24) == 1024);
        // Response with lowercased QNAME still matches; mismatched domain fails.
        uint8_t resp[21];
        std::memcpy(resp, pkt, 21);
        resp[13] = 'a';
        assert(questionMatchesQuery(pkt, 21, resp, sizeof(resp)));
        resp[15] = 'c';
        assert(!questionMatchesQuery(pkt, 21, resp, sizeof(resp)));
    }

    // IP blocker disabled (default mode 0) & StrictAllowIps (mode 2): dnsIpBlockerRememberDomain must NOT enqueue.
    WiFi.queries.clear();
    gDnsIpPolicyMode = 0;
    gPendingBlockHead = 0;
    gPendingBlockCount = 0;
    dnsIpBlockerRememberDomain("blocked.example.com", 1);
    assert(WiFi.queries.empty() && gPendingBlockCount == 0);
    gDnsIpPolicyMode = 2;
    dnsIpBlockerRememberDomain("blocked.example.com", 1);
    assert(WiFi.queries.empty() && gPendingBlockCount == 0);

    // IP blocker mode 1 (BlocklistIps): enqueues asynchronously (deduped) without calling hostByName().
    gDnsIpPolicyMode = 1;
    dnsIpBlockerRememberDomain("  BLOCKED.Example.COM. ", 1);
    dnsIpBlockerRememberDomain("blocked.example.com", 1);
    assert(WiFi.queries.empty() && gPendingBlockCount == 1);
    char popped[128] = {};
    assert(popPendingBlockResolve(popped, sizeof(popped)));
    assert(std::strcmp(popped, "blocked.example.com") == 0);
    assert(gPendingBlockCount == 0);
    gDnsIpPolicyMode = 0;

    // DNS A-record LRU cache: store, TTL clamp [30..300], hit, expire, clear.
    dnsCacheClear();
    uint32_t hitIp = 0, remTtl = 0;
    assert(!dnsCacheLookup("api.map.baidu.com", 100, hitIp, remTtl));
    dnsCacheStore("api.map.baidu.com", 0x01020304u, 5, 100);  // clamped up to 30s
    assert(dnsCacheLookup("api.map.baidu.com", 110, hitIp, remTtl));
    assert(hitIp == 0x01020304u && remTtl == 20);
    assert(!dnsCacheLookup("api.map.baidu.com", 130, hitIp, remTtl));  // expired
    dnsCacheStore("y.qq.com", 0x05060708u, 9999, 200);  // clamped down to 300s
    assert(dnsCacheLookup("y.qq.com", 250, hitIp, remTtl));
    assert(hitIp == 0x05060708u && remTtl == 250);
    dnsCacheClear();
    assert(!dnsCacheLookup("y.qq.com", 250, hitIp, remTtl));
    // FSD hijack set: exact configured hosts plus PC arp_mitm's mothership.*
    // prefix, regardless of whether the list names a concrete mothership host.
    assert(dnsFsdRedirectMatchesHost("api-prd.vn.cloud.tesla.cn", "api-prd.vn.cloud.tesla.cn"));
    assert(dnsFsdRedirectMatchesHost("api-prd.vn.cloud.tesla.cn", "API-PRD.VN.CLOUD.TESLA.CN"));
    assert(dnsFsdRedirectMatchesHost("api-prd.vn.cloud.tesla.cn", "mothership.vn.teslamotors.com"));
    assert(dnsFsdRedirectMatchesHost("api-prd.vn.cloud.tesla.cn", "mothership.vn.cloud.tesla.cn"));
    assert(dnsFsdRedirectMatchesHost("api-prd.vn.cloud.tesla.cn", "mothership.anything.example"));
    assert(!dnsFsdRedirectMatchesHost("api-prd.vn.cloud.tesla.cn", "xmothership.example"));
    assert(!dnsFsdRedirectMatchesHost("api-prd.vn.cloud.tesla.cn", "api-prd.vn.cloud.tesla.cn.evil"));
    assert(dnsFsdRedirectMatchesHost("api-prd.vn.cloud.tesla.cn,mothership.vn.teslamotors.com; foo.example", "foo.example"));
    assert(!dnsFsdRedirectMatchesHost("", "foo.example"));
    // Default external blocklist also covers both mothership hosts; the FSD
    // hijack decision above runs before this filter.
    for (const char* name : {"mothership.vn.teslamotors.com", "mothership.vn.cloud.tesla.cn"})
        expect("", "api-prd.vn.cloud.tesla.cn mothership.vn.teslamotors.com mothership.vn.cloud.tesla.cn", name, false);
    std::printf("DNS policy: %u cases passed; disabled filter and IP refresh checks passed\n", checks);
}
'''


def sanitizer_flags(cxx: str) -> list[str]:
    with tempfile.TemporaryDirectory(prefix="san-probe-") as tmp:
        probe = Path(tmp) / "p.cpp"
        out = Path(tmp) / "p"
        probe.write_text("int main(){return 0;}\n")
        r = subprocess.run([cxx, "-fsanitize=address,undefined", str(probe), "-o", str(out)],
                           capture_output=True)
        return ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if r.returncode == 0 else []


def main():
    signatures = [
        "uint16_t readU16(", "void writeU16(",
        "void dnsCacheClear(", "bool dnsCacheLookup(", "void dnsCacheStore(",
        "size_t longestRuleMatchLenCstr(", "bool isRuleOverriddenByAllowlist(",
        "void dnsAllowedIpClearInternal(", "void dnsAllowedIpRemember(",
        "bool containsAndTouchAllowedIpLocked(",
        "bool parseDomainNameToBuf(", "void clampEdnsUdpPayload(",
        "bool questionMatchesQuery(",
        "bool enqueuePendingBlockResolve(", "bool popPendingBlockResolve(",
        "bool isWildcardDomainRule(", "bool domainMatchesRule(",
        "String normalizeDomainCstr(", "bool hasRules(",
        "bool getRuleByIndex(", "size_t countRules(",
        "bool containsFreshIpLocked(",
        "bool dnsFsdRedirectMatchesHost(const char* hosts, const char* domain) {",
        "bool DNSWhitelistServer::hasDomainRules(",
        "bool DNSWhitelistServer::isAllowedDomainCstr(",
        "bool DNSWhitelistServer::isAllowedDomain(",
        "void resolveAndRememberDomain(", "size_t buildStagingFiltered(",
        'extern "C" void dnsIpPolicySetApSubnet(',
        'extern "C" void dnsIpPolicySetFilterEnabled(',
        'extern "C" void dnsIpPolicySetMode(',
        'extern "C" void dnsIpBlockerRememberDomain(',
        'extern "C" int dnsHookIp4CanForwardPacket(',
        'extern "C" int dnsHookIp4CanForward(',
    ]
    base_functions = "\n".join(function(s) for s in signatures)
    strict_functions = "\n".join(function(signature) for signature in (
        "void clearAllowedAddressesLocked(", "bool destinationAllowed(", "bool rememberAllowedAddress(",
        'extern "C" void dnsIpPolicyRecordIngress(', '\nvoid dnsStrictSetApInterface(int netifIndex) {',
        '\nvoid dnsStrictClearAddresses() {',
        '\nvoid dnsStrictUpdateRules(DNSFilterConfig& cfg, bool enabled, const char* allowlist, const char* blocklist) {',
    ))
    cxx = os.environ.get("CXX", "c++")
    san = sanitizer_flags(cxx)
    with tempfile.TemporaryDirectory(prefix="dns-checks-") as directory:
        source = Path(directory) / "dns.cpp"
        binary = Path(directory) / "dns"
        for strict in (False, True):
            flags = ["-DWIFI_BRIDGE_STRICT_ALLOWLIST"] if strict else []
            source.write_text(PREAMBLE + (STRICT_PREAMBLE if strict else "") + base_functions + (strict_functions if strict else "") + TESTS)
            subprocess.run([cxx, "-std=gnu++26", "-Wall", "-Wextra", "-Werror",
                            *san, *flags, str(source),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
