#pragma once
// LWIP IP-layer hook — only active when WIFI_BRIDGE_ENABLED
// Used to gate forwarded IPv4 packets by destination, transport port (53/853),
// and ingress subnet/interface.
// See mod_dns.h for dnsHookIp4CanForwardPacket() implementation.

#ifdef WIFI_BRIDGE_ENABLED
#include "mod_dns.h"
#include "lwip/pbuf.h"
#if (defined(LWIP_IPV6) && LWIP_IPV6) || (defined(LWIP_IPV6_FORWARD) && LWIP_IPV6_FORWARD)
#error "Wi-Fi bridge requires IPv6 and IPv6 forwarding to stay disabled"
#endif
#ifdef WIFI_BRIDGE_STRICT_ALLOWLIST
#include "lwip/netif.h"
// ip4_input calls this before its synchronous call to ip4_canforward.
static inline int dnsRecordIp4Ingress(const struct netif* netif) {
    dnsIpPolicyRecordIngress(netif_get_index(netif));
    return 0;
}
#define LWIP_HOOK_IP4_INPUT(p, inp) dnsRecordIp4Ingress(inp)
#endif
#define LWIP_HOOK_IP4_CANFORWARD(p, dest) \
    dnsHookIp4CanForwardPacket((p) ? (p)->payload : NULL, (p) ? (size_t)(p)->len : 0u, (dest))
#endif
