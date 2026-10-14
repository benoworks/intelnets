#pragma once

// Wi-Fi Credentials for S.H.A.I.N. On-Chip Telemetry & Logging
#define WIFI_SSID       "atwelve"
#define WIFI_PASS       "12121212"

// Onboard single-color indicator LED GPIO (e.g., GPIO 2 or GPIO 21 depending on ESP32-S3 board)
#define LED_GPIO        2

// Normal operation (0 = use standard DHCP DNS)
#define SIMULATE_DNS_OUTAGE 0
