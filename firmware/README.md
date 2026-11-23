# ESP32 Firmware Credentials

1. Copy `secrets.example.h` to `secrets.h` in this folder.
2. Replace the placeholder with a newly rotated device token.
3. Keep `secrets.h` private. The repository `.gitignore` excludes it.
4. Compile and upload the `.ino` sketch.

The firmware currently keeps `INSECURE_TLS_FOR_TESTING = true` for prototype testing.
This disables TLS certificate verification. Do not use that setting on an untrusted
network or in production. Before production use, install the correct trusted CA
certificate in `ROOT_CA_CERT` and set `INSECURE_TLS_FOR_TESTING = false`.

The token previously embedded in firmware source must be rotated in the backend.
Removing it from the latest file does not remove it from Git history.

## Manual Wi-Fi

The primary firmware no longer uses WiFiManager. Copy secrets.example.h to secrets.h and set WIFI_SSID and WIFI_PASSWORD to a 2.4 GHz network. Keep secrets.h private and do not commit it. The ESP32 starts local control immediately and retries Wi-Fi in the background every 10 seconds.
