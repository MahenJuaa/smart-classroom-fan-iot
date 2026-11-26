# Smart Classroom Fan — Vercel + Supabase + ESP32

Firmware and dashboard for two classrooms controlled by one ESP32.

## 1. Operating logic

Each class has an independent control mode.

### AUTO
- Temperature at or above 30°C AND PIR motion detected: fan ON.
- Temperature below 30°C: fan OFF.
- Temperature at or above 30°C but no motion: fan remains ON only until 5 minutes have elapsed since the last detected motion, then turns OFF.
- The 5-minute timer runs locally on the ESP32 and does not depend on Vercel or the internet.
- If the DHT11 reading is invalid, the firmware uses a fail-safe and does not start the fan automatically.

### MANUAL
- A physical button immediately toggles that room's relay and changes that room to MANUAL.
- Dashboard ON/OFF commands change that room to MANUAL.
- The dashboard AUTO button returns only that room to automatic control.
- Kelas A and Kelas B remain independent.

The relay state shown on the dashboard is the state reported by the ESP32 GPIO logic. There is no RPM or current sensor, so the dashboard cannot prove that the fan motor is physically rotating.

## 2. Architecture

~~~text
DHT11 + PIR + Button + Relay + Fan (Kelas A) ─┐
                                               ├─ ESP32 DevKit
DHT11 + PIR + Button + Relay + Fan (Kelas B) ─┘
                 │
           local control loop
       AUTO / MANUAL / 5-minute timer
                 │
            Wi-Fi / HTTPS
                 │
        Vercel Flask Function
          dashboard + REST API
                 │
       Supabase PostgreSQL
       status / command queue / history
~~~

Sensor reading, relay control, physical buttons, and the 5-minute timer run locally. Vercel is for the dashboard, cloud state, remote commands, and history. A Vercel/database timeout must not stop local fan control.

## 3. Repository layout

~~~text
app.py
templates/
  index.html
  login.html
firmware/
  Smart_Classroom_Fan_Optimized.ino
schema.sql
API.md
requirements.txt
.env.example
.python-version
vercel.json
AUDIT.md
~~~

## 4. Deploy the Flask application to Vercel

Vercel detects the Flask application from the top-level Flask app instance in app.py; no Gunicorn start command or custom build command is required. Procfile is not used by Vercel.

1. In Vercel, import this GitHub repository.
2. Select the repository root as the Root Directory.
3. Use the Python runtime detected from requirements.txt and .python-version. Do not enter a Gunicorn Start Command.
4. Add the environment variables below to the Vercel project. Add them to Production and Preview only where those deployments should have real access to the database and device.
5. Deploy. After deployment, open https://YOUR-VERCEL-DOMAIN/api/health. A healthy response should include "ok": true, "connected": true, and "initialized": true.
6. Open the root URL and sign in to the dashboard.

Vercel deploys this Flask app as a Python Function. vercel.json sets a 30-second maximum function duration.

Official reference: https://vercel.com/docs/frameworks/backend/flask

## 5. Vercel environment variables

Set these in Vercel Project → Settings → Environment Variables. Do not commit actual credentials in this repository.

| Variable | Description |
|---|---|
| DATABASE_URL | Supabase PostgreSQL Transaction Pooler connection URI, normally port 6543 |
| SECRET_KEY | Random Flask session secret, at least 32 characters |
| DEVICE_TOKEN | Separate random device token, at least 32 characters |
| ADMIN_USERNAME | Dashboard login username |
| ADMIN_PASSWORD | Strong, unique dashboard password |
| COOKIE_SECURE | true for Vercel HTTPS |
| DEVICE_ONLINE_SECONDS | 30 recommended for this heartbeat interval |

For Vercel/serverless functions, use the Supabase Transaction Pooler URI from Supabase Dashboard → Connect, not a direct database connection. The host is the Supabase pooler host and the port is normally 6543. Replace the password placeholder with the actual database password. Do not paste the URI into chat, firmware, or a public commit.

The backend uses a one-connection application-side pool per warm function instance, short connection/pool wait timeouts, SSL required, and disables prepared statements for compatibility with the shared transaction pooler.

Generate separate secrets locally (PowerShell):

~~~powershell
py -c "import secrets; print(secrets.token_urlsafe(48))"
~~~

Run it twice: one value for SECRET_KEY, another for DEVICE_TOKEN. The device token in the ESP32 firmware must match Vercel's DEVICE_TOKEN exactly.

## 6. Supabase database setup / migration

1. Open Supabase Dashboard → SQL Editor.
2. Run the current schema.sql from this branch.
3. The script is designed to be re-runnable. On an existing v3 database it adds classroom_state.control_mode, classroom_state.control_version, and fan_commands.control_mode without dropping existing history.
4. New classrooms default to AUTO. Existing rows receive AUTO when the new column is first added. Existing pending dashboard commands receive MANUAL as the default mode.
5. Confirm both classes exist and these tables are present: device_state, classroom_state, sensor_logs, fan_commands, and fan_logs.

The database is accessed only by the backend. Do not place database passwords or Supabase service-role credentials in browser JavaScript or ESP32 firmware. RLS is enabled and public/anon/authenticated table grants are revoked by the schema.

## 7. Firmware setup

Open firmware/Smart_Classroom_Fan_Optimized.ino in Arduino IDE. Install:
- DHT sensor library by Adafruit
- Adafruit Unified Sensor
- ArduinoJson 6.x or 7.x

Manual Wi-Fi configuration:
1. Copy firmware/secrets.example.h to firmware/secrets.h.
2. In secrets.h, set WIFI_SSID to the exact 2.4 GHz Wi-Fi/hotspot name.
3. Set WIFI_PASSWORD to that network's password.
4. Set DEVICE_TOKEN to the exact rotated token configured in Vercel.
5. Keep secrets.h private; it is excluded by .gitignore and must never be committed.

The ESP32 connects directly with WiFi.begin(WIFI_SSID, WIFI_PASSWORD). There is no SCF-Setup portal or WiFiManager dependency. If Wi-Fi is unavailable, local sensor/button/relay control starts immediately; the network task retries the configured SSID every 10 seconds.

The provided prototype firmware currently has INSECURE_TLS_FOR_TESTING=true. This skips HTTPS certificate validation and is suitable only for temporary testing on a trusted network. Before production use, set it to false and provide the correct trusted root CA in ROOT_CA_CERT.

The ESP32 network task uses a 15-second HTTP timeout. This does not block the sensor/button/relay control loop because cloud requests run on a separate FreeRTOS task. Command polling is every 5 seconds. Sensor/fan telemetry is sent shortly after a detected value/state change, with a 15-second heartbeat as a fallback. The dashboard fetches live status every 1 second; sensor and fan history panels refresh every 30 seconds.

For branch testing, the firmware defaults to the production API domain. To direct it to a Vercel Preview deployment instead, define `API_BASE_URL` in the private `firmware/secrets.h` file using the actual Preview URL. The example file documents this optional override.

## 8. GPIO map in the current firmware

Follow the firmware currently in this branch, not older pin tables or old dashboard screenshots.

| Component | Kelas A | Kelas B |
|---|---:|---:|
| DHT11 DATA | GPIO16 | GPIO17 |
| PIR OUT | GPIO26 | GPIO25 |
| Relay IN | GPIO32 | GPIO33 |
| Push button | GPIO14 | GPIO27 |

Button wiring uses INPUT_PULLUP: one terminal to the GPIO and the other to ESP32 GND. GPIO14 is used for Button A and GPIO27 for Button B in this firmware.

## 9. API behavior

| Endpoint | Purpose |
|---|---|
| GET /api/health | Backend and database health |
| GET /api/status | Dashboard status, actual fan state, desired state, mode, and device online state |
| POST /api/device/telemetry | ESP32 sensor + actual relay state |
| GET /api/device/commands | ESP32 fetches mode/version and pending dashboard commands |
| POST /api/device/ack | ESP32 acknowledges a dashboard command |
| POST /api/device/local-control | Physical button state is synced as MANUAL |
| POST /api/control | Dashboard queues MANUAL ON/OFF or AUTO mode |
| GET /api/history | Sensor history |
| GET /api/fan-history | Fan state history |

Device routes require X-Device-Token. Dashboard routes require an authenticated Flask session, and dashboard POSTs require the CSRF header.

### Mode synchronization

Each classroom stores control_mode and a monotonically increasing control_version. Whenever the dashboard or physical button changes control mode, the version increases. The ESP32 processes a new version even when there is no pending fan command, which lets the device restore the cloud mode after a restart. A command acknowledgement does not increment the version.

Telemetry updates sensor values and actual relay state. In AUTO it also mirrors actual fan state into the desired state; in MANUAL it does not overwrite the requested state.

## 10. Test checklist

1. Deploy the Vercel branch and verify /api/health.
2. Open dashboard login and confirm status refreshes.
3. Upload firmware after setting the Vercel URL and device token.
4. Confirm Serial Monitor displays Wi-Fi connected and successful HTTP 2xx responses.
5. Confirm Kelas A sensor readings map to A and Kelas B readings map to B.
6. In AUTO, test temperature below 30°C, then at/above 30°C with PIR motion.
7. With temperature ≥30°C, stop moving and confirm the fan turns OFF 5 minutes after the last PIR detection.
8. Press Button A and confirm only A changes and enters MANUAL; repeat for B.
9. Use dashboard AUTO for one room and confirm the local sensor logic resumes for that room only.
10. Use dashboard ON/OFF and confirm only the selected room enters MANUAL.
11. Disconnect internet. Confirm local AUTO and physical buttons continue working; reconnect and confirm telemetry/mode sync recovers.
12. Test one 24V fan first, then both, only after verifying relay DC contact rating and the fan/adaptor current margin.

## 11. Electrical safety and prototype limits

- Do not connect 24V to ESP32 GPIO, 3V3, VIN, or a breadboard rail.
- Keep the 24V load circuit isolated by the relay contacts. The relay control side uses its specified low-voltage supply.
- Verify the actual relay board's active level and DC motor contact rating before connecting fans.
- The 24V adaptor previously identified is labelled 24V DC, 1.5A (36W). Check the fans' actual rated current and startup current; do not assume the nominal estimate is the measured value.
- The firmware reports relay command state only; there is no fan RPM or current feedback.
- Never wire or modify the 24V circuit while the adaptor is powered.

## 12. Validation status

Static source updates are prepared in this branch. The following still require a real deployment/hardware test: Vercel environment variables, Supabase transaction-pooler URI, execution of the updated SQL, successful /api/health, ESP32 compile with the installed Arduino libraries, TLS certificate validation, and relay/fan behavior. Do not consider the system fully verified until these pass.
