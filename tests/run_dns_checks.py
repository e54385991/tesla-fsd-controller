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
struct DNSFilterConfig { bool enabled = true; const char* allowlist; const char* blocklist; };
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

TESTS = r'''
int main() {
    DNSWhitelistServer dns;
    unsigned checks = 0;
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
    // Wildcard blacklists exclude the apex and allow unrelated domains.
    expect("", "*.qq.com", "qq.com", true);
    expect("", "*.qq.com", "music.qq.com", false);
    expect("", "*.qq.com", "a.b.qq.com", false);
    expect("", "*.qq.com", "fakeqq.com", true);
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
    expect("", "", "example.com", true);
    assert(!dns.isAllowedDomain({true, nullptr, nullptr}, ""));
    assert(dns.isAllowedDomain({false, "*.qq.com", "qq.com"}, "music.qq.com"));
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
    std::printf("DNS policy: %u cases passed; disabled filter and IP refresh checks passed\n", checks);
}
'''


def main():
    signatures = [
        "bool isWildcardDomainRule(", "bool domainMatchesRule(",
        "String normalizeDomainCstr(", "bool hasRules(",
        "bool getRuleByIndex(", "size_t countRules(",
        "static size_t longestRuleMatchLen(",
        "bool DNSWhitelistServer::hasDomainRules(",
        "bool DNSWhitelistServer::isAllowedDomain(",
        "void resolveAndRememberDomain(", "size_t buildStaging(",
    ]
    code = PREAMBLE + "\n".join(function(s) for s in signatures) + TESTS
    with tempfile.TemporaryDirectory(prefix="dns-checks-") as directory:
        source = Path(directory) / "dns.cpp"
        binary = Path(directory) / "dns"
        source.write_text(code)
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(source),
                        "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
