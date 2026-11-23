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


## Manual Wi-Fi configuration update

- The primary optimized firmware no longer includes WiFiManager or starts an SCF-Setup portal.
- Wi-Fi SSID/password are supplied from the private, ignored firmware/secrets.h file.
- Initial Wi-Fi connection is non-blocking; local sensor sampling, AUTO control, buttons, timers, and relays start even if Wi-Fi is unavailable.
- The firmware retries the configured SSID/password every 10 seconds.
- GPIO14 remains Button A and GPIO27 remains Button B.
- Validation still required: configure local secrets.h, compile with the installed ESP32 board package/libraries, and verify successful association/DHCP plus API HTTP 2xx responses.
