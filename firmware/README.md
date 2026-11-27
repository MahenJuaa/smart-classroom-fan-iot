# ESP32 Firmware Setup

## Private credentials

1. Copy `secrets.example.h` to `secrets.h` in this folder.
2. Put the rotated `DEVICE_TOKEN` from Vercel in `secrets.h`.
3. Keep `secrets.h` private. The repository `.gitignore` excludes it.
4. The home Wi-Fi SSID/password are **not** stored in `secrets.h`; WiFiManager collects them through the ESP32 setup access point and stores them in ESP32 NVS.

The token previously embedded in firmware source must be rotated in the backend. Removing it from the latest file does not remove it from Git history.

## Arduino libraries

Install these through Arduino IDE → Library Manager:
- WiFiManager by tzapu
- DHT sensor library by Adafruit
- Adafruit Unified Sensor
- ArduinoJson

`WiFi.h`, `WiFiClientSecure.h`, and `HTTPClient.h` are supplied by the ESP32 board package (esp32 by Espressif Systems).

## Wi-Fi captive portal

On first boot, or when saved Wi-Fi credentials cannot connect, the ESP32 creates:
- Setup Wi-Fi name: `SmartClassroomFan-Setup`
- Setup Wi-Fi password: `SCFsetup26`
- Configuration page: `http://192.168.4.1` (many phones open it automatically)

Connect a phone/laptop to that setup Wi-Fi, select the target **2.4 GHz** router/hotspot in the portal, enter that network's password, and save. WiFiManager stores the credentials in ESP32 NVS; the ESP32 then joins that network and uses it on later boots. The setup AP is only for provisioning, not the normal internet connection.

To reopen the setup portal after credentials have been saved, hold both classroom buttons (GPIO14 and GPIO27) simultaneously for 5 seconds. This long-press is reserved for Wi-Fi setup and does not toggle either fan.

The portal runs in non-blocking mode so the local sensor/control loop remains active while the user configures Wi-Fi. The portal automatically times out after 10 minutes. The ESP32 retries saved network credentials every 10 seconds after a later disconnection.

## Which sketch to upload

Use **Smart_Classroom_Fan_Optimized.ino** as the active firmware. Do not upload the older `Smart_Classroom_Fan_WiFiManager_Fix.ino`; it is retained as a historical reference and has an outdated button mapping.

## HTTPS / TLS

The firmware currently keeps `INSECURE_TLS_FOR_TESTING = true` for prototype testing. This disables TLS certificate verification. Do not use that setting on an untrusted network or in production. Before production use, install the correct trusted CA certificate in `ROOT_CA_CERT` and set `INSECURE_TLS_FOR_TESTING = false`.

## Targeting a Vercel Preview deployment

By default, the firmware sends requests to the production domain. To test a branch without sending device data to production, add an `API_BASE_URL` definition to your private `secrets.h`:

```cpp
#define API_BASE_URL "https://your-branch-preview.vercel.app"
```

Use the actual Preview deployment URL shown by Vercel, without a trailing slash. Keep this definition in the ignored private `secrets.h` file; do not commit it. If `API_BASE_URL` is omitted, the firmware uses the production domain.
