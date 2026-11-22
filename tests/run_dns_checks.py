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
constexpr size_t kStagingCapacity = 64;
struct CachedIpEntry { uint32_t ipHostOrder; char domain[128]; uint32_t lastSeenAtSeconds; };
CachedIpEntry gStagingIps[kStagingCapacity];
'''

STRICT_PREAMBLE = r'''
constexpr uint32_t kDnsTtlSeconds = 60;
struct AllowedAddress { uint32_t ip; uint32_t issuedAt; };
AllowedAddress gAllowedAddresses[128] = {};
uint32_t gDnsRulesEpoch = 0;
uint8_t gApNetifIndex = 0;
uint8_t gIngressNetifIndex = 0;
uint32_t gIpForwardDropCount = 0;
uint32_t nowMs = 0;
uint32_t millis() { return nowMs; }
#define portENTER_CRITICAL(mux) do {} while (0)
#define portEXIT_CRITICAL(mux) do {} while (0)
#define strlcpy(dest, src, size) std::snprintf(dest, size, "%s", src)
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
    nowMs = 59999;
    assert(dnsHookIp4CanForward(approved) == -1);
    nowMs = 60000;
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
#endif
    // Neither the full nor incremental IP refresh resolves a wildcard/apex
    // substitute; only literal rules and concrete blocked hosts are resolved.
    assert(buildStaging("*.qq.com qq.com *.tesla.cn api.example.com", 42) == 2);
    assert((WiFi.queries == std::vector<std::string>{"qq.com", "api.example.com"}));
    assert(std::strcmp(gStagingIps[0].domain, "qq.com") == 0);
    assert(gStagingIps[0].lastSeenAtSeconds == 42);
    WiFi.queries.clear();
    resolveAndRememberDomain("*.qq.com");
    resolveAndRememberDomain("");
    assert(WiFi.queries.empty() && remembered.empty());
    resolveAndRememberDomain("music.qq.com");
    assert((WiFi.queries == std::vector<std::string>{"music.qq.com"}));
    assert((remembered == std::vector<std::string>{"music.qq.com"}));
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


def main():
    signatures = [
        "bool isWildcardDomainRule(", "bool domainMatchesRule(",
        "String normalizeDomainCstr(", "bool hasRules(",
        "bool getRuleByIndex(", "size_t countRules(",
        "bool dnsFsdRedirectMatchesHost(const char* hosts, const char* domain) {",
        "static size_t longestRuleMatchLen(",
        "bool DNSWhitelistServer::hasDomainRules(",
        "bool DNSWhitelistServer::isAllowedDomain(",
        "void resolveAndRememberDomain(", "size_t buildStaging(",
    ]
    base_functions = "\n".join(function(s) for s in signatures)
    strict_functions = "\n".join(function(signature) for signature in (
        "void clearAllowedAddressesLocked(", "bool destinationAllowed(", "bool rememberAllowedAddress(",
        'extern "C" void dnsIpPolicyRecordIngress(', '\nvoid dnsStrictSetApInterface(int netifIndex) {',
        '\nvoid dnsStrictClearAddresses() {',
        '\nvoid dnsStrictUpdateRules(DNSFilterConfig& cfg, bool enabled, const char* allowlist, const char* blocklist) {',
        'extern "C" int dnsHookIp4CanForward(',
    ))
    with tempfile.TemporaryDirectory(prefix="dns-checks-") as directory:
        source = Path(directory) / "dns.cpp"
        binary = Path(directory) / "dns"
        for strict in (False, True):
            extra = STRICT_PREAMBLE + strict_functions if strict else ""
            flags = ["-DWIFI_BRIDGE_STRICT_ALLOWLIST"] if strict else []
            source.write_text(PREAMBLE + base_functions + extra + TESTS)
            subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", *flags, str(source),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
