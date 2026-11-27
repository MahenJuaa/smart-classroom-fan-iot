# Audit — Smart Classroom Fan v4 (Vercel + Supabase + AUTO/MANUAL)

## Baseline issues found in v3

1. Repository documentation described Render/Gunicorn deployment even though the device URL and user deployment target are Vercel.
2. Backend opened a new PostgreSQL connection for every API request with an 8-second connection timeout. Firmware used a 2.5-second HTTP timeout, so a normal serverless cold start or database connection could exceed the ESP32 read timeout.
3. Backend command responses omitted `control_mode`, while firmware attempted to read it and defaulted a missing value to `MANUAL`.
4. Backend and dashboard explicitly documented sensor-only monitoring, while the currently uploaded firmware implemented AUTO from the DHT11 and PIR.
5. There was no persistent per-room control revision, so a device reboot or remote mode transition with no pending command could leave local control state unsynchronized.
6. Dashboard pin labels and README pin map did not match the current firmware. Dashboard identified PIR as GPIO32/33, although those pins are relay outputs in the current firmware.
7. README/dashboard referred to DHT22, but the current firmware compiles the sensor driver as DHT11.

## v4 changes in this branch

- Vercel Flask deployment is documented using the root `app.py` WSGI app entrypoint and Python runtime. `vercel.json` sets function max duration to 30 seconds. Gunicorn is removed from the Vercel requirements.
- Added a small Psycopg connection pool for warm serverless function instances. Pool size is one; SSL is required; prepared statements are disabled for Supabase shared transaction pooler compatibility; connection and pool wait timeouts are bounded.
- `.env.example` now instructs the operator to use the Supabase Transaction Pooler connection URI (port 6543) for Vercel.
- Added per-room `control_mode` and `control_version` to the schema. The schema is additive/re-runnable for an existing v3 database and does not drop sensor/fan history.
- Device command responses include mode and revision even when no command is pending.
- Dashboard supports per-room AUTO and MANUAL control. ON/OFF changes to MANUAL; AUTO resumes the local ESP32 rule.
- Physical button synchronization sets that room to MANUAL and increments its control revision.
- Telemetry only mirrors actual relay state into desired state while a room is in AUTO. It does not overwrite desired state in MANUAL.
- Acknowledgements do not increment control revision, preventing a redundant repeat synchronization.
- Firmware syncs cloud state on initial connection and whenever the room control revision changes, including when command_id is zero. Cloud requests use a 15-second timeout in the separate network task, with local PIR/button/relay logic still running in the main loop.
- Firmware command polling and telemetry intervals are 5 and 15 seconds respectively.
- Dashboard pin labels and docs now match the current firmware: DHT11 A/B GPIO16/17, PIR A/B GPIO26/25, relay A/B GPIO32/33, buttons A/B GPIO14/27.

## Expected control behavior

- AUTO, temperature >=30°C + motion -> fan ON.
- AUTO, temperature below 30°C -> fan OFF.
- AUTO, no motion for 5 minutes since last detected motion -> fan OFF.
- A physical button moves only its own classroom into MANUAL.
- Dashboard ON/OFF moves only the selected classroom into MANUAL.
- Dashboard AUTO releases manual override and resumes local control.
- Internet/cloud failure does not stop local sensor sampling, button input, timer, or relay decisions.

## Validation still required

The following have not been proven by a live deployment in this audit:
- Vercel environment values are correctly set, particularly the Supabase Transaction Pooler DATABASE_URL.
- Current SQL schema has been executed successfully in the user's Supabase project.
- Deployed `/api/health` returns a healthy database response.
- The Git branch preview deployment works with the project's Vercel settings.
- ESP32 firmware compiles against the user's installed ESP32 board package and Arduino libraries.
- ESP32 returns successful HTTP 2xx responses from Vercel.
- TLS certificate validation is configured for production. The prototype firmware still has `INSECURE_TLS_FOR_TESTING=true`; this bypasses certificate verification and must be disabled before production use.
- GPIO, relay active level, 24V fan load, and startup current are physically tested.

Do not claim the system is fully validated until these checks pass. This branch is a prepared patch for review/deployment testing, not a claim that the live hardware has already been proven.


## Wi-Fi captive portal update

- The active optimized firmware uses WiFiManager with an ESP32 SoftAP/captive portal; home Wi-Fi SSID/password are not compiled into the sketch or private secrets file.
- First boot (or failed saved-network connection) exposes a board-specific `SCF-Setup-XXXX` AP with a random password printed in Serial Monitor at 115200 baud. The user selects a 2.4 GHz network and enters its password in the portal at `http://192.168.4.1`.
- WiFiManager stores the selected network credentials in ESP32 NVS. On later boots, the ESP32 tries the saved network automatically.
- WiFiManager runs in non-blocking mode and is serviced from the main loop; the sensor/control loop continues while the setup portal is active.
- Holding both classroom buttons simultaneously for 5 seconds reopens the setup portal. GPIO14 remains Button A and GPIO27 remains Button B; the long-press is consumed by Wi-Fi setup, not fan toggling.
- The network task pauses cloud HTTP requests while the portal is active.
- Validation required: install WiFiManager by tzapu in the local Arduino IDE, compile the active optimized sketch, verify initial AP provisioning and saved-NVS reconnection, then verify the two-button portal shortcut and continued local sensor operation.


## Sensor responsiveness tuning

- DHT11 polling interval in the optimized firmware is 2.5 seconds, with the first sample taken immediately after startup. This remains above the Adafruit library's 2-second minimum interval.
- PIR inputs continue to be sampled in the main loop. Firmware now detects HIGH/LOW transitions and prints timestamp-adjacent diagnostic messages using the `[PIR]` prefix in Serial Monitor.
- A change in temperature, humidity, PIR state, or fan state marks telemetry as dirty. The network task posts changed telemetry after a short 750 ms debounce instead of waiting for the 15-second heartbeat.
- A 15-second telemetry heartbeat remains as a fallback. The backend still updates live classroom state on heartbeats but inserts sensor history only when temperature, humidity, or motion actually changes.
- Dashboard live status polling is reduced from 3 seconds to 1 second. History panels refresh every 30 seconds.
- DHT11's physical temperature response is still slow (approximately 10 seconds to reach 63% of a temperature step under the datasheet condition). Software changes reduce transport/display delay, not the sensor element's physical response. Do not test by placing an open flame against or very close to the sensor; use a controlled warm-air source at a safe distance.
- For HC-SR501 validation, allow the module to stabilize after power-up, use repeatable trigger mode (H), reduce TIME to its minimum practical setting, and test movement across its field of view. Verify the output pin independently before attributing a delay to firmware.

## Responsiveness validation still required

- Compile the optimized sketch against the exact ESP32 board package and installed DHT/ArduinoJson libraries.
- Open Serial Monitor at 115200 baud. Confirm `[DHT]` lines appear when the reported temperature or humidity changes and `[PIR]` lines appear promptly for both HIGH and LOW transitions.
- Compare the timestamp of each Serial Monitor transition with the dashboard update. A quick PIR transition in Serial Monitor but a late dashboard update indicates a network/API/dashboard path issue; a late or missing Serial Monitor transition points to sensor placement, wiring, supply, or module adjustment.
- Repeat the test with each classroom independently and verify that one room's sensor changes do not alter the other room's telemetry or fan state.
- These changes have been committed to the working branch for deployment testing; they have not yet been proven on the physical ESP32 or live Vercel/Supabase deployment.
