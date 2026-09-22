// Minimal DNS server for the captive portal: answers every A query with the
// soft-AP address so phones and laptops open the setup page on their own.
#pragma once

#include <stdint.h>

namespace dns_server {

// ip is in network byte order (as in esp_ip4_addr_t::addr)
void start(uint32_t ip);
void stop();

}  // namespace dns_server
