#pragma once
// ── DNS whitelist/blacklist module ────────────────────────────────────────────
// Compiled only when WIFI_BRIDGE_ENABLED. Single-TU: main.cpp defines
// MOD_DNS_IMPLEMENTATION before including this (via handlers.h in CAN builds,
// or directly in FSD_MOCK_ONLY builds) so the definition compiles exactly
// once in main.cpp's TU.
//
// This header is ALSO included from IDF lwIP C code via lwip_hooks.h — from
// that side only the extern "C" declarations are visible (no Arduino/WiFi.h).
//
// Port of pudge9527/tesla-fsd-wifi-controller dns_whitelist.cpp +
// dns_ip_blocker.cpp merged into a single translation-unit header.

#ifdef WIFI_BRIDGE_ENABLED

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Return 1=allow, 0=drop, -1=fall through to default lwIP forwarding.
// ESP-IDF's ip4_canforward passes the destination in host byte order.
int dnsHookIp4CanForward(uint32_t destAddrHostOrder);

#ifdef WIFI_BRIDGE_STRICT_ALLOWLIST
void dnsIpPolicyRecordIngress(uint8_t netifIndex);
#endif

// Periodic service: keeps the blocklist IP cache fresh. Call from loop().
void dnsIpPolicyService(const char* allowlistRules,
                        const char* blocklistRules,
                        int rulesEnabled,
                        int upstreamReady);

// Record an IP just resolved for `domain` (called when a blocked-name query
// comes in so subsequent hardcoded-IP traffic to that host is dropped).
void dnsIpBlockerRememberDomain(const char* domain, int upstreamReady);

// Flush the IP cache (e.g. when rules change or WiFi drops).
void dnsIpBlockerClear(void);

// Observability: outDrops = cumulative packets dropped by the lwIP hook,
// outCacheCount = current number of blocked IPs cached,
// outCachePeak = historical high-water mark since boot or last clear.
// Pass NULL for any output to skip.
void dnsIpBlockerGetStats(uint32_t* outDrops, uint32_t* outCacheCount, uint32_t* outCachePeak);

#ifdef __cplusplus
}
#endif

// ── C++-side Arduino interface (only visible when compiled as C++) ────────────
#ifdef __cplusplus

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <cctype>
#include <cstring>
#include <strings.h>

// FSD payload interception (TeslaDG dns_sink.py parity): answer these exact
// host rules with the ESP32 AP address instead of forwarding upstream.
void dnsSetFsdRedirect(bool enabled, const char* hosts, const IPAddress& answer);
void dnsGetFsdRedirectStats(uint32_t* total, char* lastDomain, size_t capacity);

// Hijack matcher shared by the DNS sink and the reverse proxy. Explicit host
// rules are space/comma/semicolon separated; in addition, TeslaDG
// arp_mitm.py's REDIRECT_PREFIX ("mothership.") hijacks any mothership.*
// hostname so both mothership.vn.teslamotors.com and newer variants work.
bool dnsFsdRedirectMatchesHost(const char* hosts, const char* domain);

// Buffer size for allow/block rule strings. 1024 was too small — user-customized
// allowlists (full preset + extra domains) exceed 1024 and the /api/set handler
// silently drops oversize writes. 3072 leaves headroom for ~30 extra entries
// past the default preset while still staying well within NVS blob limits.
static constexpr size_t kDnsRuleBufSize = 3072;

struct DNSFilterConfig {
    bool enabled = false;
    char allowlist[kDnsRuleBufSize] = {};
    char blocklist[kDnsRuleBufSize] = {};
};

#ifdef WIFI_BRIDGE_STRICT_ALLOWLIST
void dnsStrictSetApInterface(int netifIndex);
void dnsStrictClearAddresses();
void dnsStrictUpdateRules(DNSFilterConfig& cfg, bool enabled, const char* allowlist, const char* blocklist);
#else
void dnsFilterUpdateConfig(DNSFilterConfig& cfg, bool hasEnable, bool enabled,
                           const char* allowlist, const char* blocklist);
#endif

static constexpr size_t kDnsBlockedDomainCapacity = 50;

struct DNSBlockedDomainStatEntry {
    char domain[128] = {};
    uint32_t count = 0;
    uint32_t lastBlockedAtUptimeSeconds = 0;
};

class DNSWhitelistServer {
  public:
    bool begin(uint16_t port = 53);
    void stop();
    bool started() const { return started_; }
    bool waitForRequest(uint32_t timeoutMs);
    bool processNextRequest(const DNSFilterConfig& cfg, bool upstreamReady);
    void clearCache();
    void getPerfStats(uint32_t* cacheHits, uint32_t* cacheMisses,
                      uint32_t* forwardOk, uint32_t* forwardTimeouts) const;
    size_t copyBlockedDomains(DNSBlockedDomainStatEntry* dest, size_t maxEntries, uint32_t& totalBlockedCount);
    void clearBlockedRequests();

  private:
    int listenSock_ = -1;
    int upstreamSock_ = -1;
    struct sockaddr_in lastClientAddr_ = {};
    bool started_ = false;
    DNSBlockedDomainStatEntry blockedDomains_[kDnsBlockedDomainCapacity];
    size_t blockedDomainCount_ = 0;
    uint32_t totalBlockedCount_ = 0;
    mutable uint32_t cacheHits_ = 0;
    mutable uint32_t cacheMisses_ = 0;
    mutable uint32_t forwardOk_ = 0;
    mutable uint32_t forwardTimeouts_ = 0;

    bool openUpstreamSocket();
    void sendRawReply(const uint8_t* data, size_t length);
    String normalizeDomain(const String& domain) const;
    bool parseDomainName(const uint8_t* query, size_t length, size_t& offset, String& domain) const;
    bool skipDomainName(const uint8_t* packet, size_t length, size_t& offset) const;
    bool hasDomainRules(const char* rules) const;
    bool listContainsDomain(const char* rules, const String& domain) const;
    bool isAllowedDomain(const DNSFilterConfig& cfg, const String& domain) const;
    void logBlockedRequest(const String& domain);
    bool forwardUpstreamQuery(const uint8_t* query, size_t queryLength, uint8_t* response, size_t responseCapacity, size_t& responseLength);
    void cacheAllowedAddressesFromResponse(const String& domain, const uint8_t* response, size_t responseLength);
    void sendBlockedResponse(const uint8_t* query, size_t questionEnd, uint16_t requestFlags, uint16_t qType);
    void sendErrorResponse(const uint8_t* query, size_t questionEnd, uint16_t requestFlags, uint16_t rcode);
    void sendNoAnswerResponse(const uint8_t* query, size_t questionEnd, uint16_t requestFlags);
    void sendIPv4Answer(const uint8_t* query, size_t questionEnd, uint16_t requestFlags, const IPAddress& ip, uint32_t ttlSeconds = 60);
};

#endif // __cplusplus

// ── Implementation ────────────────────────────────────────────────────────────
// Defined once in the main TU (handlers.h sets MOD_DNS_IMPLEMENTATION before
// including this header). All internal helpers are `static` to keep linkage
// private to this TU.
#ifdef MOD_DNS_IMPLEMENTATION

namespace {

// ── DNS packet helpers ───────────────────────────────────────────────────────
constexpr uint16_t kDnsHeaderSize = 12;
constexpr uint16_t kDnsTypeA = 1;
constexpr uint16_t kDnsTypeAAAA = 28;
constexpr uint16_t kDnsTypeCNAME = 5;
constexpr uint16_t kDnsClassIN = 1;
constexpr uint32_t kDnsTtlSeconds = 60;
constexpr uint32_t kDnsServerTimeoutMs = 600;
constexpr uint32_t kDnsCacheMinTtlSeconds = 30;
constexpr uint32_t kDnsCacheMaxTtlSeconds = 300;
constexpr size_t   kDnsCacheCapacity = 32;
constexpr size_t   kPendingBlockResolveCap = 8;

#ifdef WIFI_BRIDGE_STRICT_ALLOWLIST
// Shared by the DNS task and the lwIP TCP/IP thread. Only addresses actually
// returned to AP DNS clients are permitted, for the advertised DNS TTL.
struct AllowedAddress {
    uint32_t ip;
    uint32_t issuedAt;
};
portMUX_TYPE gDnsAllowMux = portMUX_INITIALIZER_UNLOCKED;
AllowedAddress gAllowedAddresses[128] = {};
uint32_t gDnsRulesEpoch = 0;
uint8_t gApNetifIndex = 0;
uint8_t gIngressNetifIndex = 0;  // Only accessed on the lwIP TCP/IP thread.

void clearAllowedAddressesLocked() {
    memset(gAllowedAddresses, 0, sizeof(gAllowedAddresses));
    ++gDnsRulesEpoch;
}

bool destinationAllowed(const AllowedAddress* addresses, size_t count, uint32_t ip, uint32_t now) {
    for (size_t i = 0; i < count; ++i) {
        if (addresses[i].ip == ip && ip != 0 && now - addresses[i].issuedAt < kDnsTtlSeconds) return true;
    }
    return false;
}

bool rememberAllowedAddress(uint32_t ip, uint32_t epoch) {
    if (ip == 0) return false;
    const uint32_t now = millis() / 1000;
    portENTER_CRITICAL(&gDnsAllowMux);
    const bool currentRules = epoch == gDnsRulesEpoch && gApNetifIndex != 0;
    if (currentRules) {
        AllowedAddress* oldest = &gAllowedAddresses[0];
        for (auto& entry : gAllowedAddresses) {
            if (entry.ip == ip || entry.ip == 0) {
                oldest = &entry;
                break;
            }
            if (now - entry.issuedAt > now - oldest->issuedAt) oldest = &entry;
        }
        *oldest = {ip, now};
    }
    portEXIT_CRITICAL(&gDnsAllowMux);
    return currentRules;
}
#endif

portMUX_TYPE gDnsBlockedLogMux = portMUX_INITIALIZER_UNLOCKED;

#ifndef WIFI_BRIDGE_STRICT_ALLOWLIST
// Guards cross-core updates to gDnsFilterCfg between HTTP handlers and DNSFilter.
portMUX_TYPE gDnsFilterCfgMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t gDnsFilterCfgGen = 1;
#endif

// Static IPv4 A-record LRU cache (~3.4 KB BSS, zero heap churn).
// Checked AFTER FSD redirect and AFTER allow/block policy; cleared on STA drop
// or DNS rule updates so policy changes take effect immediately.
struct DnsCacheEntry {
    char domain[96] = {};
    uint32_t ipv4Nbo = 0;
    uint32_t expiresAtSec = 0;
    uint32_t lastUsedSec = 0;
};
portMUX_TYPE gDnsCacheMux = portMUX_INITIALIZER_UNLOCKED;
DnsCacheEntry gDnsCache[kDnsCacheCapacity];

void dnsCacheClear() {
    portENTER_CRITICAL(&gDnsCacheMux);
    memset(gDnsCache, 0, sizeof(gDnsCache));
    portEXIT_CRITICAL(&gDnsCacheMux);
}

bool dnsCacheLookup(const char* domain, uint32_t nowSec, uint32_t& outIpv4Nbo, uint32_t& outRemainingTtl) {
    if (domain == nullptr || domain[0] == '\0') return false;
    bool hit = false;
    portENTER_CRITICAL(&gDnsCacheMux);
    for (size_t i = 0; i < kDnsCacheCapacity; ++i) {
        if (gDnsCache[i].ipv4Nbo == 0) continue;
        if (static_cast<int32_t>(gDnsCache[i].expiresAtSec - nowSec) <= 0) {
            gDnsCache[i].ipv4Nbo = 0;
            gDnsCache[i].domain[0] = '\0';
            continue;
        }
        if (strcmp(gDnsCache[i].domain, domain) == 0) {
            outIpv4Nbo = gDnsCache[i].ipv4Nbo;
            outRemainingTtl = gDnsCache[i].expiresAtSec - nowSec;
            gDnsCache[i].lastUsedSec = nowSec;
            hit = true;
            break;
        }
    }
    portEXIT_CRITICAL(&gDnsCacheMux);
    return hit;
}

void dnsCacheStore(const char* domain, uint32_t ipv4Nbo, uint32_t ttlSec, uint32_t nowSec) {
    if (domain == nullptr || domain[0] == '\0' || ipv4Nbo == 0) return;
    if (strlen(domain) >= sizeof(gDnsCache[0].domain)) return;
    if (ttlSec < kDnsCacheMinTtlSeconds) ttlSec = kDnsCacheMinTtlSeconds;
    if (ttlSec > kDnsCacheMaxTtlSeconds) ttlSec = kDnsCacheMaxTtlSeconds;
    const uint32_t expiresAt = nowSec + ttlSec;

    portENTER_CRITICAL(&gDnsCacheMux);
    size_t target = 0;
    bool found = false;
    for (size_t i = 0; i < kDnsCacheCapacity; ++i) {
        if (gDnsCache[i].ipv4Nbo != 0 && strcmp(gDnsCache[i].domain, domain) == 0) {
            target = i;
            found = true;
            break;
        }
    }
    if (!found) {
        for (size_t i = 0; i < kDnsCacheCapacity; ++i) {
            if (gDnsCache[i].ipv4Nbo == 0 ||
                static_cast<int32_t>(gDnsCache[i].expiresAtSec - nowSec) <= 0) {
                target = i;
                found = true;
                break;
            }
            if (nowSec - gDnsCache[i].lastUsedSec > nowSec - gDnsCache[target].lastUsedSec) {
                target = i;
            }
        }
    }
    strlcpy(gDnsCache[target].domain, domain, sizeof(gDnsCache[target].domain));
    gDnsCache[target].ipv4Nbo = ipv4Nbo;
    gDnsCache[target].expiresAtSec = expiresAt;
    gDnsCache[target].lastUsedSec = nowSec;
    portEXIT_CRITICAL(&gDnsCacheMux);
}

// FSD redirect snapshot shared by the DNS task and the HTTP config task.
portMUX_TYPE gFsdRedirectMux = portMUX_INITIALIZER_UNLOCKED;
bool gFsdRedirectEnabled = false;
char gFsdRedirectHosts[128] = {};
IPAddress gFsdRedirectAnswer;
uint32_t gFsdRedirectTotal = 0;
char gFsdRedirectLast[128] = {};

uint16_t readU16(const uint8_t* ptr) {
    return static_cast<uint16_t>((ptr[0] << 8) | ptr[1]);
}

uint32_t readU32(const uint8_t* ptr) {
    return (static_cast<uint32_t>(ptr[0]) << 24) |
           (static_cast<uint32_t>(ptr[1]) << 16) |
           (static_cast<uint32_t>(ptr[2]) << 8)  |
            static_cast<uint32_t>(ptr[3]);
}

void writeU16(uint8_t* ptr, uint16_t value) {
    ptr[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    ptr[1] = static_cast<uint8_t>(value & 0xFF);
}

void writeU32(uint8_t* ptr, uint32_t value) {
    ptr[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    ptr[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    ptr[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    ptr[3] = static_cast<uint8_t>(value & 0xFF);
}

bool isWildcardDomainRule(const String& rule) {
    return rule.startsWith("*.");
}

bool domainMatchesRule(const String& domain, const String& rule) {
    if (rule.isEmpty()) return false;

    // Bare domains include the apex; "*.example.com" includes only subdomains
    // at any depth. Both forms require a label boundary before the suffix.
    const bool wildcard = isWildcardDomainRule(rule);
    const size_t prefixLength = wildcard ? 2 : 0;
    const size_t suffixLength = rule.length() - prefixLength;
    if (!wildcard && domain == rule) return true;
    if (suffixLength == 0 || domain.length() <= suffixLength) return false;

    const size_t offset = domain.length() - suffixLength;
    return domain[offset - 1] == '.' &&
        strcmp(domain.c_str() + offset, rule.c_str() + prefixLength) == 0;
}

// ── IP blocker state (shared with lwIP hook) ─────────────────────────────────
constexpr size_t kBlockedIpCapacity = 256;
constexpr uint32_t kIpEntryTtlSeconds = 3600;
constexpr uint32_t kRuleResolveIntervalSeconds = 15;
constexpr uint32_t kFullRefreshIntervalSeconds = 300;

// Compact IP blocker entry (8 bytes vs old 136 bytes with unused domain[128]):
// saves 40.96 KB of internal RAM across gBlockedIps[256] + gStagingIps[64] and
// keeps the lwIP forwarding hook scan inside L1 cache.
struct CachedIpEntry {
    uint32_t ipHostOrder = 0;
    uint32_t lastSeenAtSeconds = 0;
};

portMUX_TYPE gDnsIpPolicyMux = portMUX_INITIALIZER_UNLOCKED;
CachedIpEntry gBlockedIps[kBlockedIpCapacity];
size_t gBlockedIpCount = 0;
char gBlocklistSnapshot[kDnsRuleBufSize] = {};
uint32_t gLastBlockResolveAtSeconds = 0;
uint32_t gLastFullRefreshAtSeconds = 0;
size_t gNextBlockResolveRuleIndex = 0;
volatile uint32_t gIpForwardDropCount = 0;
volatile uint32_t gIpCachePeakCount = 0;

// Bounded queue of recently blocked concrete hostnames waiting for background
// IP resolution (only populated when ipBlockerEnabled == true). Keeps :53
// replies instant (<0.5 ms) instead of stalling on synchronous WiFi.hostByName.
char gPendingBlockResolve[kPendingBlockResolveCap][128] = {};
size_t gPendingBlockHead = 0;
size_t gPendingBlockCount = 0;

// Staging buffer for atomic rule-set swaps. Populated OUTSIDE the critical
// section (may block on WiFi.hostByName per rule), then copied into gBlockedIps
// inside a short lock so the hook never sees a half-empty cache during a
// preset switch. 64 rules is far more than any realistic user blocklist.
constexpr size_t kStagingCapacity = 64;
CachedIpEntry gStagingIps[kStagingCapacity];

void clearPendingBlockResolveLocked() {
    gPendingBlockHead = 0;
    gPendingBlockCount = 0;
}

bool enqueuePendingBlockResolve(const char* domain) {
    if (domain == nullptr || domain[0] == '\0') return false;
    bool queued = false;
    portENTER_CRITICAL(&gDnsIpPolicyMux);
    bool dup = false;
    for (size_t i = 0; i < gPendingBlockCount; ++i) {
        const size_t slot = (gPendingBlockHead + i) % kPendingBlockResolveCap;
        if (strcmp(gPendingBlockResolve[slot], domain) == 0) {
            dup = true;
            break;
        }
    }
    if (!dup && gPendingBlockCount < kPendingBlockResolveCap) {
        const size_t tail = (gPendingBlockHead + gPendingBlockCount) % kPendingBlockResolveCap;
        strlcpy(gPendingBlockResolve[tail], domain, sizeof(gPendingBlockResolve[tail]));
        ++gPendingBlockCount;
        queued = true;
    }
    portEXIT_CRITICAL(&gDnsIpPolicyMux);
    return queued;
}

bool popPendingBlockResolve(char* out, size_t capacity) {
    if (out == nullptr || capacity == 0) return false;
    bool found = false;
    portENTER_CRITICAL(&gDnsIpPolicyMux);
    if (gPendingBlockCount > 0) {
        strlcpy(out, gPendingBlockResolve[gPendingBlockHead], capacity);
        gPendingBlockHead = (gPendingBlockHead + 1) % kPendingBlockResolveCap;
        --gPendingBlockCount;
        found = true;
    }
    portEXIT_CRITICAL(&gDnsIpPolicyMux);
    return found;
}

String normalizeDomainCstr(const char* domain) {
    String normalized = domain == nullptr ? "" : String(domain);
    normalized.trim();
    normalized.toLowerCase();
    while (normalized.endsWith(".")) {
        normalized.remove(normalized.length() - 1);
    }
    return normalized;
}

bool hasRules(const char* rules) {
    if (rules == nullptr) return false;
    const char* cursor = rules;
    while (*cursor) {
        while (*cursor && (isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) {
            ++cursor;
        }
        if (!*cursor) break;
        return true;
    }
    return false;
}

bool snapshotMatches(const char* rules, const char* snapshot) {
    return strcmp(rules == nullptr ? "" : rules, snapshot == nullptr ? "" : snapshot) == 0;
}

void clearEntries(CachedIpEntry* entries, size_t capacity, size_t& count) {
    for (size_t i = 0; i < capacity; ++i) {
        entries[i] = CachedIpEntry{};
    }
    count = 0;
}

void pruneEntriesLocked(CachedIpEntry* entries, size_t capacity, size_t& count, uint32_t nowSeconds) {
    size_t writeIndex = 0;
    for (size_t i = 0; i < count; ++i) {
        if (nowSeconds - entries[i].lastSeenAtSeconds <= kIpEntryTtlSeconds) {
            if (writeIndex != i) {
                entries[writeIndex] = entries[i];
            }
            ++writeIndex;
        }
    }
    for (size_t i = writeIndex; i < capacity; ++i) {
        entries[i] = CachedIpEntry{};
    }
    count = writeIndex;
}

void addOrRefreshEntryLocked(CachedIpEntry* entries, size_t capacity, size_t& count,
                             const String& domain, uint32_t ipHostOrder, uint32_t nowSeconds) {
    (void)domain;
    if (ipHostOrder == 0) return;

    for (size_t i = 0; i < count; ++i) {
        if (entries[i].ipHostOrder == ipHostOrder) {
            entries[i].lastSeenAtSeconds = nowSeconds;
            return;
        }
    }

    CachedIpEntry entry = {};
    entry.ipHostOrder = ipHostOrder;
    entry.lastSeenAtSeconds = nowSeconds;

    if (count < capacity) {
        entries[count++] = entry;
        if (count > gIpCachePeakCount) gIpCachePeakCount = count;
        return;
    }
    gIpCachePeakCount = capacity;  // cache is full → peak at capacity

    size_t oldestIndex = 0;
    for (size_t i = 1; i < count; ++i) {
        if (entries[i].lastSeenAtSeconds < entries[oldestIndex].lastSeenAtSeconds) {
            oldestIndex = i;
        }
    }
    entries[oldestIndex] = entry;
}

bool getRuleByIndex(const char* rules, size_t targetIndex, String& outRule) {
    outRule = "";
    if (rules == nullptr) return false;

    size_t currentIndex = 0;
    const char* cursor = rules;
    while (*cursor) {
        while (*cursor && (isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) {
            ++cursor;
        }
        if (!*cursor) break;

        const char* start = cursor;
        while (*cursor && !(isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) {
            ++cursor;
        }

        if (currentIndex == targetIndex) {
            String rule(start, cursor - start);
            outRule = normalizeDomainCstr(rule.c_str());
            return !outRule.isEmpty();
        }
        ++currentIndex;
    }
    return false;
}

size_t countRules(const char* rules) {
    size_t count = 0;
    String rule;
    while (getRuleByIndex(rules, count, rule)) {
        ++count;
    }
    return count;
}

uint32_t ipAddressToHostOrder(const IPAddress& ip) {
    return
        (static_cast<uint32_t>(ip[0]) << 24) |
        (static_cast<uint32_t>(ip[1]) << 16) |
        (static_cast<uint32_t>(ip[2]) << 8) |
        static_cast<uint32_t>(ip[3]);
}

void rememberResolvedIp(const String& domain, uint32_t ipHostOrder) {
    uint32_t nowSeconds = millis() / 1000;

    portENTER_CRITICAL(&gDnsIpPolicyMux);
    pruneEntriesLocked(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount, nowSeconds);
    addOrRefreshEntryLocked(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount, domain, ipHostOrder, nowSeconds);
    portEXIT_CRITICAL(&gDnsIpPolicyMux);
}

void resolveAndRememberDomain(const String& domain) {
    // A wildcard is a DNS policy, not a hostname to resolve. Concrete blocked
    // queries are still remembered by dnsIpBlockerRememberDomain().
    if (domain.isEmpty() || isWildcardDomainRule(domain)) return;

    IPAddress resolved;
    if (WiFi.hostByName(domain.c_str(), resolved) == 1) {
        rememberResolvedIp(domain, ipAddressToHostOrder(resolved));
    }
}

// Build staging from rules OUTSIDE the lock — hostByName may block for
// hundreds of ms per rule. Dedupes within staging. Returns populated count.
size_t buildStaging(const char* blocklistRules, uint32_t nowSeconds) {
    memset(gStagingIps, 0, sizeof(gStagingIps));
    if (!hasRules(blocklistRules)) return 0;

    size_t ruleCount = countRules(blocklistRules);
    size_t stagingCount = 0;
    for (size_t i = 0; i < ruleCount && stagingCount < kStagingCapacity; ++i) {
        String rule;
        if (!getRuleByIndex(blocklistRules, i, rule) || rule.isEmpty() || isWildcardDomainRule(rule)) continue;
        IPAddress resolved;
        if (WiFi.hostByName(rule.c_str(), resolved) != 1) continue;
        uint32_t ipHostOrder = ipAddressToHostOrder(resolved);
        if (ipHostOrder == 0) continue;
        bool dup = false;
        for (size_t j = 0; j < stagingCount; ++j) {
            if (gStagingIps[j].ipHostOrder == ipHostOrder) { dup = true; break; }
        }
        if (dup) continue;
        gStagingIps[stagingCount].ipHostOrder = ipHostOrder;
        gStagingIps[stagingCount].lastSeenAtSeconds = nowSeconds;
        stagingCount++;
    }
    return stagingCount;
}

// Caller holds gDnsIpPolicyMux. Overwrites gBlockedIps with staging in a
// single short pass — hook sees either the coherent old set or the coherent
// new set, never empty. strncpy (not String) because malloc inside a critical
// section is unsafe (not interrupt-reentrant).
void applyStagingLocked(size_t stagingCount, const char* blocklistRules, uint32_t nowSeconds) {
    size_t oldCount = gBlockedIpCount;
    for (size_t i = 0; i < stagingCount; ++i) {
        gBlockedIps[i] = gStagingIps[i];
    }
    for (size_t i = stagingCount; i < oldCount; ++i) {
        gBlockedIps[i] = CachedIpEntry{};
    }
    gBlockedIpCount = stagingCount;
    if (stagingCount > gIpCachePeakCount) gIpCachePeakCount = (uint32_t)stagingCount;

    memset(gBlocklistSnapshot, 0, sizeof(gBlocklistSnapshot));
    if (blocklistRules != nullptr) {
        strncpy(gBlocklistSnapshot, blocklistRules, sizeof(gBlocklistSnapshot) - 1);
    }
    gLastBlockResolveAtSeconds = nowSeconds;
    gLastFullRefreshAtSeconds = nowSeconds;
    gNextBlockResolveRuleIndex = 0;
}

bool containsFreshIpLocked(const CachedIpEntry* entries, size_t count,
                           uint32_t ipHostOrder, uint32_t nowSeconds) {
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].ipHostOrder == ipHostOrder &&
            nowSeconds - entries[i].lastSeenAtSeconds <= kIpEntryTtlSeconds) {
            return true;
        }
    }
    return false;
}

}  // namespace

// ── FSD redirect setters/getters (called from fsd_mock.cpp) ──────────────────
bool dnsFsdRedirectMatchesHost(const char* hosts, const char* domain) {
    if (hosts == nullptr || domain == nullptr || domain[0] == '\0') return false;

    // TeslaDG arp_mitm.py: REDIRECT_PREFIX = ("mothership.",). Any host below
    // the mothership label is hijacked, even if only api-prd is configured.
    if (strncasecmp(domain, "mothership.", 11) == 0) return true;
    if (hosts[0] == '\0') return false;

    const size_t domainLength = strlen(domain);
    const char* cursor = hosts;
    while (*cursor) {
        while (*cursor && (isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) {
            ++cursor;
        }
        if (!*cursor) break;
        const char* begin = cursor;
        while (*cursor && !(isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) {
            ++cursor;
        }
        const size_t length = static_cast<size_t>(cursor - begin);
        if (length == domainLength && strncasecmp(begin, domain, length) == 0) return true;
    }
    return false;
}

void dnsSetFsdRedirect(bool enabled, const char* hosts, const IPAddress& answer) {
    portENTER_CRITICAL(&gFsdRedirectMux);
    gFsdRedirectEnabled = enabled && hosts != nullptr && hosts[0] != '\0';
    if (hosts != nullptr) {
        strlcpy(gFsdRedirectHosts, hosts, sizeof(gFsdRedirectHosts));
    } else {
        gFsdRedirectHosts[0] = '\0';
    }
    gFsdRedirectAnswer = answer;
    portEXIT_CRITICAL(&gFsdRedirectMux);
}

void dnsGetFsdRedirectStats(uint32_t* total, char* lastDomain, size_t capacity) {
    portENTER_CRITICAL(&gFsdRedirectMux);
    if (total) *total = gFsdRedirectTotal;
    if (lastDomain && capacity) {
        strlcpy(lastDomain, gFsdRedirectLast, capacity);
    }
    portEXIT_CRITICAL(&gFsdRedirectMux);
}

// ── DNSWhitelistServer member definitions ────────────────────────────────────
bool DNSWhitelistServer::openUpstreamSocket() {
    if (upstreamSock_ >= 0) return true;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = 0;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) < 0) {
        close(s);
        return false;
    }
    fcntl(s, F_SETFL, O_NONBLOCK);
    upstreamSock_ = s;
    return true;
}

void DNSWhitelistServer::sendRawReply(const uint8_t* data, size_t length) {
    if (!started_ || listenSock_ < 0 || data == nullptr || length == 0) return;
    sendto(listenSock_, data, length, 0,
           reinterpret_cast<const struct sockaddr*>(&lastClientAddr_),
           sizeof(lastClientAddr_));
}

bool DNSWhitelistServer::begin(uint16_t port) {
    stop();
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
#ifdef WIFI_BRIDGE_STRICT_ALLOWLIST
    addr.sin_addr.s_addr = static_cast<uint32_t>(WiFi.softAPIP());
#else
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
#endif
    if (bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(s);
        return false;
    }
    fcntl(s, F_SETFL, O_NONBLOCK);
    listenSock_ = s;
    openUpstreamSocket();
    started_ = true;
    return true;
}

void DNSWhitelistServer::stop() {
    if (listenSock_ >= 0) {
        close(listenSock_);
        listenSock_ = -1;
    }
    if (upstreamSock_ >= 0) {
        close(upstreamSock_);
        upstreamSock_ = -1;
    }
    started_ = false;
}

bool DNSWhitelistServer::waitForRequest(uint32_t timeoutMs) {
    if (!started_ || listenSock_ < 0) return false;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(listenSock_, &rfds);
    struct timeval tv = {};
    tv.tv_sec = static_cast<long>(timeoutMs / 1000);
    tv.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);
    return select(listenSock_ + 1, &rfds, nullptr, nullptr, &tv) > 0;
}

void DNSWhitelistServer::clearCache() {
    dnsCacheClear();
}

void DNSWhitelistServer::getPerfStats(uint32_t* cacheHits, uint32_t* cacheMisses,
                                      uint32_t* forwardOk, uint32_t* forwardTimeouts) const {
    if (cacheHits)       *cacheHits       = cacheHits_;
    if (cacheMisses)     *cacheMisses     = cacheMisses_;
    if (forwardOk)       *forwardOk       = forwardOk_;
    if (forwardTimeouts) *forwardTimeouts = forwardTimeouts_;
}

String DNSWhitelistServer::normalizeDomain(const String& domain) const {
    String normalized = domain;
    normalized.trim();
    normalized.toLowerCase();
    while (normalized.endsWith(".")) {
        normalized.remove(normalized.length() - 1);
    }
    return normalized;
}

bool DNSWhitelistServer::parseDomainName(const uint8_t* query, size_t length, size_t& offset, String& domain) const {
    domain = "";
    bool first = true;

    while (offset < length) {
        uint8_t labelLength = query[offset++];
        if (labelLength == 0) return true;
        if ((labelLength & 0xC0) != 0 || offset + labelLength > length) return false;

        if (!first) domain += '.';
        for (uint8_t i = 0; i < labelLength; ++i) {
            domain += static_cast<char>(query[offset++]);
        }
        first = false;
    }
    return false;
}

bool DNSWhitelistServer::skipDomainName(const uint8_t* packet, size_t length, size_t& offset) const {
    while (offset < length) {
        uint8_t labelLength = packet[offset++];
        if (labelLength == 0) return true;
        if ((labelLength & 0xC0) == 0xC0) {
            return offset < length;
        }
        if ((labelLength & 0xC0) != 0 || offset + labelLength > length) return false;
        offset += labelLength;
    }
    return false;
}

bool DNSWhitelistServer::hasDomainRules(const char* rules) const {
    return hasRules(rules);
}

bool DNSWhitelistServer::listContainsDomain(const char* rules, const String& domain) const {
    if (rules == nullptr || domain.isEmpty()) return false;

    const char* cursor = rules;
    while (*cursor) {
        while (*cursor && (isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) {
            ++cursor;
        }
        if (!*cursor) break;

        const char* start = cursor;
        while (*cursor && !(isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) {
            ++cursor;
        }

        String rule(start, cursor - start);
        rule = normalizeDomain(rule);
        if (domainMatchesRule(domain, rule)) return true;
    }
    return false;
}

// Longest matching domain suffix (0 = no match). Single-pass O(L) scanner with
// zero heap allocations (replaces old O(N^2) getRuleByIndex loop). The "*."
// prefix does not increase specificity: a concrete child rule beats its parent
// wildcard.
static size_t longestRuleMatchLen(const char* rules, const String& domain) {
    if (rules == nullptr || domain.isEmpty()) return 0;
    const char* dom = domain.c_str();
    const size_t domLen = domain.length();
    size_t best = 0;
    const char* cursor = rules;
    while (*cursor) {
        while (*cursor && (isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) {
            ++cursor;
        }
        if (!*cursor) break;
        const char* start = cursor;
        while (*cursor && !(isspace(static_cast<unsigned char>(*cursor)) || *cursor == ',' || *cursor == ';')) {
            ++cursor;
        }
        const char* end = cursor;
        while (end > start && end[-1] == '.') --end;
        if (end <= start) continue;

        const bool wildcard = (end - start >= 2 && start[0] == '*' && start[1] == '.');
        const char* suffix = wildcard ? (start + 2) : start;
        const size_t suffixLen = static_cast<size_t>(end - suffix);
        if (suffixLen == 0 || suffixLen <= best) continue;

        bool matched = false;
        if (!wildcard && domLen == suffixLen) {
            matched = (strncasecmp(dom, suffix, suffixLen) == 0);
        } else if (domLen > suffixLen) {
            const size_t offset = domLen - suffixLen;
            matched = (dom[offset - 1] == '.' &&
                       strncasecmp(dom + offset, suffix, suffixLen) == 0);
        }
        if (matched) best = suffixLen;
    }
    return best;
}

bool DNSWhitelistServer::isAllowedDomain(const DNSFilterConfig& cfg, const String& domain) const {
#ifdef WIFI_BRIDGE_STRICT_ALLOWLIST
    if (!cfg.enabled || !hasDomainRules(cfg.allowlist)) return false;
#else
    if (!cfg.enabled) return true;
#endif
    if (domain.isEmpty()) return false;

    size_t blockLen = longestRuleMatchLen(cfg.blocklist, domain);
    size_t allowLen = longestRuleMatchLen(cfg.allowlist, domain);

    // dnsmasq semantics: more specific (longer) rule wins.
    if (allowLen > 0 && allowLen >= blockLen) return true;
    if (blockLen > 0) return false;
    if (!hasDomainRules(cfg.allowlist)) return true;
    return false;
}

void DNSWhitelistServer::logBlockedRequest(const String& domain) {
    String normalizedDomain = normalizeDomain(domain);
    if (normalizedDomain.isEmpty()) return;

    uint32_t blockedAtUptimeSeconds = millis() / 1000;

    portENTER_CRITICAL(&gDnsBlockedLogMux);
    bool found = false;
    for (size_t i = 0; i < blockedDomainCount_; ++i) {
        if (normalizedDomain.equals(blockedDomains_[i].domain)) {
            ++blockedDomains_[i].count;
            blockedDomains_[i].lastBlockedAtUptimeSeconds = blockedAtUptimeSeconds;
            found = true;
            break;
        }
    }

    if (!found && blockedDomainCount_ < kDnsBlockedDomainCapacity) {
        DNSBlockedDomainStatEntry entry = {};
        normalizedDomain.toCharArray(entry.domain, sizeof(entry.domain));
        entry.count = 1;
        entry.lastBlockedAtUptimeSeconds = blockedAtUptimeSeconds;
        blockedDomains_[blockedDomainCount_++] = entry;
    }
    ++totalBlockedCount_;
    portEXIT_CRITICAL(&gDnsBlockedLogMux);
}

bool DNSWhitelistServer::forwardUpstreamQuery(const uint8_t* query, size_t queryLength, uint8_t* response, size_t responseCapacity, size_t& responseLength) {
    responseLength = 0;
    if (query == nullptr || response == nullptr || queryLength < kDnsHeaderSize || responseCapacity < kDnsHeaderSize) {
        return false;
    }
    if (!openUpstreamSocket()) return false;

    // Candidate DNS servers in priority order:
    //   1) Upstream DHCP primary DNS
    //   2) Upstream DHCP secondary DNS (if distinct)
    //   3) Public fallback 223.5.5.5 (AliDNS) if mobile hotspot DNS stalls
    const uint32_t selfApAddr = static_cast<uint32_t>(WiFi.softAPIP());
    uint32_t servers[3] = {};
    size_t serverCount = 0;
    auto pushServer = [&](const IPAddress& ip) {
        const uint32_t addr = static_cast<uint32_t>(ip);
        if (addr == 0 || addr == selfApAddr) return;
        for (size_t i = 0; i < serverCount; ++i) {
            if (servers[i] == addr) return;
        }
        if (serverCount < 3) servers[serverCount++] = addr;
    };
    pushServer(WiFi.dnsIP(0));
    pushServer(WiFi.dnsIP(1));
    pushServer(IPAddress(223, 5, 5, 5));
    if (serverCount == 0) return false;

    // Drain any stale late replies from previous timed-out queries.
    uint8_t drainBuf[64];
    for (int d = 0; d < 8; ++d) {
        if (recvfrom(upstreamSock_, drainBuf, sizeof(drainBuf), 0, nullptr, nullptr) <= 0) break;
    }

    for (size_t sIdx = 0; sIdx < serverCount; ++sIdx) {
        struct sockaddr_in target = {};
        target.sin_family = AF_INET;
        target.sin_port = htons(53);
        target.sin_addr.s_addr = servers[sIdx];

        const int sent = sendto(upstreamSock_, query, queryLength, 0,
                                reinterpret_cast<const struct sockaddr*>(&target), sizeof(target));
        if (sent != static_cast<int>(queryLength)) continue;

        const uint32_t deadline = millis() + kDnsServerTimeoutMs;
        for (;;) {
            const int32_t remaining = static_cast<int32_t>(deadline - millis());
            if (remaining <= 0) break;

            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(upstreamSock_, &rfds);
            struct timeval tv = {};
            tv.tv_sec = static_cast<long>(remaining / 1000);
            tv.tv_usec = static_cast<long>((remaining % 1000) * 1000);
            const int ready = select(upstreamSock_ + 1, &rfds, nullptr, nullptr, &tv);
            if (ready <= 0) break;

            struct sockaddr_in fromAddr = {};
            socklen_t fromLen = sizeof(fromAddr);
            const int received = recvfrom(upstreamSock_, response, responseCapacity, 0,
                                          reinterpret_cast<struct sockaddr*>(&fromAddr), &fromLen);
            if (received < static_cast<int>(kDnsHeaderSize)) continue;
            // Verify source IP, DNS Transaction ID, and QR=response bit.
            if (fromAddr.sin_addr.s_addr != servers[sIdx]) continue;
            if (response[0] != query[0] || response[1] != query[1]) continue;
            if ((response[2] & 0x80) == 0) continue;

            responseLength = static_cast<size_t>(received);
            ++forwardOk_;
            return true;
        }
    }

    ++forwardTimeouts_;
    return false;
}

void DNSWhitelistServer::cacheAllowedAddressesFromResponse(const String& domain, const uint8_t* response, size_t responseLength) {
    if (response == nullptr || responseLength < kDnsHeaderSize || domain.isEmpty()) return;

    const uint16_t flags = readU16(response + 2);
    const uint16_t rcode = flags & 0x000F;
    const bool truncated = (flags & 0x0200) != 0;
    if (rcode != 0 || truncated) return;

    const uint16_t qdCount = readU16(response + 4);
    const uint16_t anCount = readU16(response + 6);
    if (anCount == 0) return;

    size_t offset = kDnsHeaderSize;
    for (uint16_t i = 0; i < qdCount; ++i) {
        if (!skipDomainName(response, responseLength, offset) || offset + 4 > responseLength) return;
        offset += 4;
    }

    uint32_t minTtl = kDnsCacheMaxTtlSeconds;
    uint32_t cachedIpNbo = 0;
    for (uint16_t i = 0; i < anCount; ++i) {
        if (!skipDomainName(response, responseLength, offset) || offset + 10 > responseLength) return;

        const uint16_t rrType = readU16(response + offset);
        const uint16_t rrClass = readU16(response + offset + 2);
        const uint32_t rrTtl = readU32(response + offset + 4);
        const uint16_t rdLength = readU16(response + offset + 8);
        offset += 10;

        if (offset + rdLength > responseLength) return;

        if (rrClass == kDnsClassIN && (rrType == kDnsTypeA || rrType == kDnsTypeCNAME)) {
            if (rrTtl < minTtl) minTtl = rrTtl;
        }
        if (cachedIpNbo == 0 && rrType == kDnsTypeA && rrClass == kDnsClassIN && rdLength == 4) {
            memcpy(&cachedIpNbo, response + offset, 4);
        }
        offset += rdLength;
    }

    if (cachedIpNbo != 0) {
        dnsCacheStore(domain.c_str(), cachedIpNbo, minTtl, millis() / 1000);
    }
}

size_t DNSWhitelistServer::copyBlockedDomains(DNSBlockedDomainStatEntry* dest, size_t maxEntries, uint32_t& totalBlockedCount) {
    totalBlockedCount = 0;
    if (dest == nullptr || maxEntries == 0) return 0;

    portENTER_CRITICAL(&gDnsBlockedLogMux);
    totalBlockedCount = totalBlockedCount_;
    size_t count = blockedDomainCount_ < maxEntries ? blockedDomainCount_ : maxEntries;

    for (size_t i = 0; i < count; ++i) {
        dest[i] = blockedDomains_[i];
    }
    portEXIT_CRITICAL(&gDnsBlockedLogMux);

    for (size_t i = 0; i < count; ++i) {
        size_t bestIndex = i;
        for (size_t j = i + 1; j < count; ++j) {
            if (dest[j].count < dest[bestIndex].count ||
                (dest[j].count == dest[bestIndex].count && strcmp(dest[j].domain, dest[bestIndex].domain) < 0)) {
                bestIndex = j;
            }
        }
        if (bestIndex != i) {
            DNSBlockedDomainStatEntry tmp = dest[i];
            dest[i] = dest[bestIndex];
            dest[bestIndex] = tmp;
        }
    }
    return count;
}

void DNSWhitelistServer::clearBlockedRequests() {
    portENTER_CRITICAL(&gDnsBlockedLogMux);
    for (size_t i = 0; i < kDnsBlockedDomainCapacity; ++i) {
        blockedDomains_[i] = DNSBlockedDomainStatEntry{};
    }
    blockedDomainCount_ = 0;
    totalBlockedCount_ = 0;
    portEXIT_CRITICAL(&gDnsBlockedLogMux);
}

void DNSWhitelistServer::sendErrorResponse(const uint8_t* query, size_t questionEnd, uint16_t requestFlags, uint16_t rcode) {
    if (!started_) return;

    uint8_t response[512] = {};
    if (questionEnd > sizeof(response)) return;

    memcpy(response, query, questionEnd);
    uint16_t responseFlags = static_cast<uint16_t>(0x8000 | 0x0080 | (requestFlags & 0x0100) | (rcode & 0x000F));
    writeU16(response + 2, responseFlags);
    writeU16(response + 4, questionEnd > kDnsHeaderSize ? 1 : 0);
    writeU16(response + 6, 0);
    writeU16(response + 8, 0);
    writeU16(response + 10, 0);

    sendRawReply(response, questionEnd);
}

void DNSWhitelistServer::sendNoAnswerResponse(const uint8_t* query, size_t questionEnd, uint16_t requestFlags) {
    sendErrorResponse(query, questionEnd, requestFlags, 0);
}

void DNSWhitelistServer::sendBlockedResponse(const uint8_t* query, size_t questionEnd, uint16_t requestFlags, uint16_t qType) {
    if (qType == kDnsTypeA) {
        sendIPv4Answer(query, questionEnd, requestFlags, IPAddress(0, 0, 0, 0), kDnsTtlSeconds);
        return;
    }
    // Return a successful empty answer instead of REFUSED to reduce client
    // fallback to alternate DNS paths when a domain is intentionally blocked.
    sendNoAnswerResponse(query, questionEnd, requestFlags);
}

void DNSWhitelistServer::sendIPv4Answer(const uint8_t* query, size_t questionEnd, uint16_t requestFlags, const IPAddress& ip, uint32_t ttlSeconds) {
    if (!started_) return;

    uint8_t response[512] = {};
    const size_t answerSize = 16;
    const size_t responseSize = questionEnd + answerSize;
    if (responseSize > sizeof(response)) return;

    memcpy(response, query, questionEnd);
    uint16_t responseFlags = static_cast<uint16_t>(0x8000 | 0x0080 | (requestFlags & 0x0100));
    writeU16(response + 2, responseFlags);
    writeU16(response + 4, 1);
    writeU16(response + 6, 1);
    writeU16(response + 8, 0);
    writeU16(response + 10, 0);

    size_t offset = questionEnd;
    response[offset++] = 0xC0;
    response[offset++] = 0x0C;
    writeU16(response + offset, kDnsTypeA);
    offset += 2;
    writeU16(response + offset, kDnsClassIN);
    offset += 2;
    writeU32(response + offset, ttlSeconds);
    offset += 4;
    writeU16(response + offset, 4);
    offset += 2;
    response[offset++] = ip[0];
    response[offset++] = ip[1];
    response[offset++] = ip[2];
    response[offset++] = ip[3];

    sendRawReply(response, responseSize);
}

bool DNSWhitelistServer::processNextRequest(const DNSFilterConfig& cfg, bool upstreamReady) {
    if (!started_ || listenSock_ < 0) return false;

    uint8_t query[512] = {};
    socklen_t clientLen = sizeof(lastClientAddr_);
    const int read = recvfrom(listenSock_, query, sizeof(query), 0,
                              reinterpret_cast<struct sockaddr*>(&lastClientAddr_), &clientLen);
    if (read <= 0) return false;
    if (read < static_cast<int>(kDnsHeaderSize)) return true;

    uint16_t requestFlags = readU16(query + 2);
    uint16_t qdCount = readU16(query + 4);
    uint16_t qr = requestFlags & 0x8000;
    uint16_t opcode = (requestFlags >> 11) & 0x0F;

    if (qr != 0 || opcode != 0 || qdCount != 1) {
        sendErrorResponse(query, kDnsHeaderSize, requestFlags, 4);
        return true;
    }

    size_t offset = kDnsHeaderSize;
    String domain;
    if (!parseDomainName(query, read, offset, domain) || offset + 4 > static_cast<size_t>(read)) {
        sendErrorResponse(query, kDnsHeaderSize, requestFlags, 1);
        return true;
    }

    const size_t questionEnd = offset + 4;
    domain = normalizeDomain(domain);

    uint16_t qType = readU16(query + offset);
    uint16_t qClass = readU16(query + offset + 2);

    if (qClass != kDnsClassIN) {
        sendErrorResponse(query, questionEnd, requestFlags, 5);
        return true;
    }

    // Windows dns_sink.py: --redirect host(s) -> local IP, ahead of the
    // allow/block rules. AAAA and other types get an empty answer so the
    // client falls back to A (same as build_empty in dns_sink.py).
    bool fsdRedirectEnabled = false;
    char fsdRedirectHosts[sizeof(gFsdRedirectHosts)] = {};
    IPAddress fsdRedirectIp;
    portENTER_CRITICAL(&gFsdRedirectMux);
    fsdRedirectEnabled = gFsdRedirectEnabled;
    strlcpy(fsdRedirectHosts, gFsdRedirectHosts, sizeof(fsdRedirectHosts));
    fsdRedirectIp = gFsdRedirectAnswer;
    portEXIT_CRITICAL(&gFsdRedirectMux);

    bool fsdRedirect = false;
    if (fsdRedirectEnabled && !domain.isEmpty()) {
        // Windows dns_sink.py uses exact set membership for --redirect; the
        // mothership prefix follows arp_mitm.py's REDIRECT_PREFIX.
        fsdRedirect = dnsFsdRedirectMatchesHost(fsdRedirectHosts, domain.c_str());
        if (fsdRedirect) {
            portENTER_CRITICAL(&gFsdRedirectMux);
            ++gFsdRedirectTotal;
            strlcpy(gFsdRedirectLast, domain.c_str(), sizeof(gFsdRedirectLast));
            portEXIT_CRITICAL(&gFsdRedirectMux);
        }
    }
    if (fsdRedirect) {
        if (qType == kDnsTypeA) {
            sendIPv4Answer(query, questionEnd, requestFlags, fsdRedirectIp, kDnsTtlSeconds);
        } else {
            sendNoAnswerResponse(query, questionEnd, requestFlags);
        }
        return true;
    }

#ifdef WIFI_BRIDGE_STRICT_ALLOWLIST
    // One DNS task owns this scratch copy. Never hold a mux during resolution.
    static DNSFilterConfig rules;
    portENTER_CRITICAL(&gDnsAllowMux);
    rules = cfg;
    const uint32_t epoch = gDnsRulesEpoch;
    portEXIT_CRITICAL(&gDnsAllowMux);
    const DNSFilterConfig& currentRules = rules;
#else
    // Refresh the task-local rule snapshot only when config generation bumps
    // (zero lock/copy overhead on the hot path, zero torn reads across cores).
    static DNSFilterConfig rulesSnapshot;
    static uint32_t cachedGen = 0;
    if (cachedGen != gDnsFilterCfgGen) {
        portENTER_CRITICAL(&gDnsFilterCfgMux);
        rulesSnapshot = cfg;
        cachedGen = gDnsFilterCfgGen;
        portEXIT_CRITICAL(&gDnsFilterCfgMux);
    }
    const DNSFilterConfig& currentRules = rulesSnapshot;
#endif
    if (!isAllowedDomain(currentRules, domain)) {
        logBlockedRequest(domain);
#ifndef WIFI_BRIDGE_STRICT_ALLOWLIST
        dnsIpBlockerRememberDomain(domain.c_str(), upstreamReady ? 1 : 0);
#endif
        sendBlockedResponse(query, questionEnd, requestFlags, qType);
        return true;
    }

    if (!upstreamReady) {
        sendErrorResponse(query, questionEnd, requestFlags, 2);
        return true;
    }

#ifndef WIFI_BRIDGE_STRICT_ALLOWLIST
    if (qType == kDnsTypeA) {
        uint32_t cachedIpNbo = 0;
        uint32_t remainingTtl = kDnsTtlSeconds;
        if (dnsCacheLookup(domain.c_str(), millis() / 1000, cachedIpNbo, remainingTtl)) {
            ++cacheHits_;
            sendIPv4Answer(query, questionEnd, requestFlags, IPAddress(cachedIpNbo), remainingTtl);
            return true;
        }
        ++cacheMisses_;
    }

    uint8_t upstreamResponse[512] = {};
    size_t upstreamResponseLength = 0;
    if (forwardUpstreamQuery(query, read, upstreamResponse, sizeof(upstreamResponse), upstreamResponseLength)) {
        cacheAllowedAddressesFromResponse(domain, upstreamResponse, upstreamResponseLength);
        sendRawReply(upstreamResponse, upstreamResponseLength);
        return true;
    }

    // Do NOT fall back to blocking WiFi.hostByName() here: forwardUpstreamQuery
    // already tried primary, secondary, and fallback DNS servers; a secondary
    // lwIP getaddrinfo() stall would freeze :53 for 5-10 seconds.
    sendErrorResponse(query, questionEnd, requestFlags, 2);
    return true;
#else
    if (qType != kDnsTypeA) {
        sendNoAnswerResponse(query, questionEnd, requestFlags);
        return true;
    }

    IPAddress resolved;
    if (WiFi.hostByName(domain.c_str(), resolved) == 1) {
        if (WiFi.status() != WL_CONNECTED || !rememberAllowedAddress(ipAddressToHostOrder(resolved), epoch)) {
            sendErrorResponse(query, questionEnd, requestFlags, 2);
            return true;
        }
        sendIPv4Answer(query, questionEnd, requestFlags, resolved, kDnsTtlSeconds);
    } else {
        sendErrorResponse(query, questionEnd, requestFlags, 3);
    }
    return true;
#endif
}

#ifdef WIFI_BRIDGE_STRICT_ALLOWLIST
extern "C" void dnsIpPolicyRecordIngress(uint8_t netifIndex) {
    gIngressNetifIndex = netifIndex;
}

void dnsStrictSetApInterface(int netifIndex) {
    portENTER_CRITICAL(&gDnsAllowMux);
    gApNetifIndex = netifIndex > 0 ? static_cast<uint8_t>(netifIndex) : 0;
    clearAllowedAddressesLocked();
    portEXIT_CRITICAL(&gDnsAllowMux);
}

void dnsStrictClearAddresses() {
    portENTER_CRITICAL(&gDnsAllowMux);
    clearAllowedAddressesLocked();
    portEXIT_CRITICAL(&gDnsAllowMux);
}

void dnsStrictUpdateRules(DNSFilterConfig& cfg, bool enabled, const char* allowlist, const char* blocklist) {
    portENTER_CRITICAL(&gDnsAllowMux);
    if (cfg.enabled != enabled || (allowlist && strcmp(cfg.allowlist, allowlist) != 0) ||
        (blocklist && strcmp(cfg.blocklist, blocklist) != 0)) {
        cfg.enabled = enabled;
        if (allowlist) strlcpy(cfg.allowlist, allowlist, sizeof(cfg.allowlist));
        if (blocklist) strlcpy(cfg.blocklist, blocklist, sizeof(cfg.blocklist));
        clearAllowedAddressesLocked();
    }
    portEXIT_CRITICAL(&gDnsAllowMux);
    dnsCacheClear();
}
#else
void dnsFilterUpdateConfig(DNSFilterConfig& cfg, bool hasEnable, bool enabled,
                           const char* allowlist, const char* blocklist) {
    portENTER_CRITICAL(&gDnsFilterCfgMux);
    if (hasEnable) cfg.enabled = enabled;
    if (allowlist != nullptr) strlcpy(cfg.allowlist, allowlist, sizeof(cfg.allowlist));
    if (blocklist != nullptr) strlcpy(cfg.blocklist, blocklist, sizeof(cfg.blocklist));
    gDnsFilterCfgGen = gDnsFilterCfgGen + 1;
    portEXIT_CRITICAL(&gDnsFilterCfgMux);
    dnsCacheClear();
}
#endif

// The IP-blocker gate used to read the injected CAN settings struct. In the
// Wi-Fi/FSD-mock build that struct no longer exists, so keep a local flag
// there (default OFF) and defer to cfg in full CAN builds.
#ifdef FSD_MOCK_ONLY
// The FSD-mock build has no FSDConfig struct. Keep the flag here instead;
// Wi-Fi bridge config load/save persists it as NVS key "ub_ipBlk".
static volatile bool gDnsIpBlockerEnabled = false;
static inline bool dnsIpBlockerEnabled() { return gDnsIpBlockerEnabled; }
static inline void dnsIpBlockerSetEnabled(bool enabled) { gDnsIpBlockerEnabled = enabled; }
#else
static inline bool dnsIpBlockerEnabled() { return cfg.ipBlockerEnabled; }
static inline void dnsIpBlockerSetEnabled(bool enabled) { cfg.ipBlockerEnabled = enabled; }
#endif

// ── C-linkage entry points (visible to lwIP hook) ────────────────────────────
extern "C" void dnsIpPolicyService(const char* allowlistRules, const char* blocklistRules, int rulesEnabled, int upstreamReady) {
    uint32_t nowSeconds = millis() / 1000;
    (void)allowlistRules;

    // When DNS filtering or the IP-layer blocker is OFF (default), do NOT
    // resolve any blocklist hostnames from loop() — avoids periodic 1-10 s
    // stalls and unwanted upstream DNS lookups.
    if (!rulesEnabled || !dnsIpBlockerEnabled()) {
        portENTER_CRITICAL(&gDnsIpPolicyMux);
        if (gBlockedIpCount > 0 || gBlocklistSnapshot[0] != '\0' || gPendingBlockCount > 0) {
            clearEntries(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount);
            clearPendingBlockResolveLocked();
            memset(gBlocklistSnapshot, 0, sizeof(gBlocklistSnapshot));
            gLastBlockResolveAtSeconds = 0;
            gLastFullRefreshAtSeconds = nowSeconds;
            gNextBlockResolveRuleIndex = 0;
        }
        portEXIT_CRITICAL(&gDnsIpPolicyMux);
        return;
    }

    bool blocklistHasRules = hasRules(blocklistRules);

    bool snapshotChanged;
    portENTER_CRITICAL(&gDnsIpPolicyMux);
    snapshotChanged = !snapshotMatches(blocklistRules, gBlocklistSnapshot);
    portEXIT_CRITICAL(&gDnsIpPolicyMux);

    bool fullRefreshDue = blocklistHasRules &&
        (nowSeconds - gLastFullRefreshAtSeconds >= kFullRefreshIntervalSeconds);

    // Rule change OR scheduled full refresh: resolve into staging outside the
    // lock, then atomic-swap. Old IPs remain blocked until the swap completes,
    // so there is no window where the hook sees an empty cache (= "nothing is
    // blocked right now") during a preset switch.
    if (snapshotChanged || fullRefreshDue) {
        if (!upstreamReady) return;  // cache stays on old IPs; retry next tick
        size_t stagingCount = buildStaging(blocklistRules, nowSeconds);

        portENTER_CRITICAL(&gDnsIpPolicyMux);
        applyStagingLocked(stagingCount, blocklistRules, nowSeconds);
        portEXIT_CRITICAL(&gDnsIpPolicyMux);
        return;
    }

    if (!upstreamReady) return;

    portENTER_CRITICAL(&gDnsIpPolicyMux);
    pruneEntriesLocked(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount, nowSeconds);
    portEXIT_CRITICAL(&gDnsIpPolicyMux);

    // Drain at most one queued blocked domain per tick so loop() stays
    // responsive while :53 never blocks on blocked-domain IP lookups.
    char pendingDomain[128] = {};
    if (popPendingBlockResolve(pendingDomain, sizeof(pendingDomain))) {
        resolveAndRememberDomain(String(pendingDomain));
        return;
    }

    // Incremental per-rule refresh — add-or-refresh only (no clear), so the
    // cache never dips to empty between ticks either.
    if (blocklistHasRules && nowSeconds - gLastBlockResolveAtSeconds >= kRuleResolveIntervalSeconds) {
        size_t ruleCount = countRules(blocklistRules);
        if (ruleCount > 0) {
            String rule;
            if (!getRuleByIndex(blocklistRules, gNextBlockResolveRuleIndex, rule)) {
                gNextBlockResolveRuleIndex = 0;
                getRuleByIndex(blocklistRules, gNextBlockResolveRuleIndex, rule);
            }
            resolveAndRememberDomain(rule);
            gLastBlockResolveAtSeconds = nowSeconds;
            gNextBlockResolveRuleIndex = (gNextBlockResolveRuleIndex + 1) % ruleCount;
        }
    }
}

extern "C" void dnsIpBlockerRememberDomain(const char* domain, int upstreamReady) {
    // Never resolve blocked domains when IP-layer blocking is disabled, and
    // never block the :53 DNS task synchronously when it is enabled: enqueue
    // for background resolution in dnsIpPolicyService().
    if (!upstreamReady || !dnsIpBlockerEnabled()) return;
    if (domain == nullptr || domain[0] == '\0') return;
    char norm[128] = {};
    size_t len = 0;
    const char* p = domain;
    while (*p && isspace(static_cast<unsigned char>(*p))) ++p;
    while (*p && len + 1 < sizeof(norm)) {
        norm[len++] = static_cast<char>(tolower(static_cast<unsigned char>(*p++)));
    }
    while (len > 0 && (isspace(static_cast<unsigned char>(norm[len - 1])) || norm[len - 1] == '.')) {
        --len;
    }
    norm[len] = '\0';
    if (len == 0) return;
    enqueuePendingBlockResolve(norm);
}

extern "C" void dnsIpBlockerClear(void) {
    portENTER_CRITICAL(&gDnsIpPolicyMux);
    clearEntries(gBlockedIps, kBlockedIpCapacity, gBlockedIpCount);
    clearPendingBlockResolveLocked();
    gIpForwardDropCount = 0;
    gIpCachePeakCount   = 0;
    portEXIT_CRITICAL(&gDnsIpPolicyMux);
}

extern "C" int dnsHookIp4CanForward(uint32_t destAddrHostOrder) {
#ifdef WIFI_BRIDGE_STRICT_ALLOWLIST
    const uint32_t now = millis() / 1000;
    portENTER_CRITICAL(&gDnsAllowMux);
    const bool drop = gApNetifIndex == 0 || (gIngressNetifIndex == gApNetifIndex &&
        !destinationAllowed(gAllowedAddresses, sizeof(gAllowedAddresses) / sizeof(gAllowedAddresses[0]),
                            destAddrHostOrder, now));
    portEXIT_CRITICAL(&gDnsAllowMux);
    if (drop) {
        portENTER_CRITICAL(&gDnsIpPolicyMux);
        ++gIpForwardDropCount;
        portEXIT_CRITICAL(&gDnsIpPolicyMux);
    }
    return drop ? 0 : -1;
#else
    // Hot path — called for every outbound IPv4 packet while NAPT is on.
    // Short-circuit before touching the critical section when the blocker is
    // disabled or the IP cache is empty. When enabled, scan compact 8-byte
    // entries without shifting/pruning memory inside the interrupt-disabled lock.
    if (!dnsIpBlockerEnabled()) return -1;
    if (gBlockedIpCount == 0)  return -1;

    uint32_t nowSeconds = millis() / 1000;

    portENTER_CRITICAL(&gDnsIpPolicyMux);
    if (containsFreshIpLocked(gBlockedIps, gBlockedIpCount, destAddrHostOrder, nowSeconds)) {
        gIpForwardDropCount = gIpForwardDropCount + 1;
        portEXIT_CRITICAL(&gDnsIpPolicyMux);
        return 0;  // drop
    }
    portEXIT_CRITICAL(&gDnsIpPolicyMux);
    return -1;  // fall through to default lwIP forwarding
#endif
}

extern "C" void dnsIpBlockerGetStats(uint32_t* outDrops, uint32_t* outCacheCount, uint32_t* outCachePeak) {
    portENTER_CRITICAL(&gDnsIpPolicyMux);
    if (outDrops)      *outDrops      = gIpForwardDropCount;
    if (outCacheCount) *outCacheCount = (uint32_t)gBlockedIpCount;
    if (outCachePeak)  *outCachePeak  = gIpCachePeakCount;
    portEXIT_CRITICAL(&gDnsIpPolicyMux);
}

#endif // MOD_DNS_IMPLEMENTATION

#endif // WIFI_BRIDGE_ENABLED
