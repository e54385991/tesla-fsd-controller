#pragma once
// LWIP IP-layer hook — only active when WIFI_BRIDGE_ENABLED
// Used to gate forwarded IPv4 packets by destination and ingress interface.
// See mod_dns.h for dnsHookIp4CanForward() implementation.

#ifdef WIFI_BRIDGE_ENABLED
#include "mod_dns.h"
#ifdef WIFI_BRIDGE_STRICT_ALLOWLIST
#include "lwip/netif.h"
#if LWIP_IPV6_FORWARD
#error "Strict IPv4 allowlist requires IPv6 forwarding to stay disabled"
#endif
// ip4_input calls this before its synchronous call to ip4_canforward.
static inline int dnsRecordIp4Ingress(const struct netif* netif) {
    dnsIpPolicyRecordIngress(netif_get_index(netif));
    return 0;
}
#define LWIP_HOOK_IP4_INPUT(p, inp) dnsRecordIp4Ingress(inp)
#endif
#define LWIP_HOOK_IP4_CANFORWARD(p, dest) dnsHookIp4CanForward(dest)
#endif
