#pragma once
// Test network for the WPA2 handshake step. The passphrase is compiled into the kext in plain text and is
// committed with the source, so use a throwaway guest network or phone hotspot, not your real password.
// The SSID must match the router the driver associates with (it is hard-coded to "TT_4FF8" for the tests).
#define RTL_TEST_PASSPHRASE "lduk4703"
#define RTL_TEST_SSID "Galaxy S22 Ultra93F4"
