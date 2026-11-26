#pragma once
// Copy this file to secrets.h in the same firmware folder.
// Put a newly rotated device token here.
// secrets.h is ignored by Git; never commit it.
#define DEVICE_TOKEN "PASTE_NEW_ROTATED_DEVICE_TOKEN_HERE"

// Manual Wi-Fi credentials. Replace these locally; do not commit secrets.h.
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Optional: set this to the Vercel Preview URL when testing a branch deployment.
// Leave commented to use the production API URL defined in the firmware.
// #define API_BASE_URL "https://your-vercel-preview-url.vercel.app"
