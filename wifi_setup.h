// wifi_setup.h — WiFiManager AP-mode onboarding.
//
// Standard captive portal: if no credentials are saved, the device opens
// an AP named "<device_hostname>-setup"; the user joins it from a phone,
// the portal auto-launches, they enter their LAN credentials, the device
// saves them and reboots into client mode.
//
// The web console's "Reset WiFi" wipes creds and reboots into AP mode.

#pragma once

void wifi_setup_begin();
void wifi_setup_reset_and_reboot();
bool wifi_setup_in_ap_mode();
