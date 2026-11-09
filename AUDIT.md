# Audit — v3 Cloud IoT + Supabase

## Requirement changes reflected in this version

The user clarified the target as a full cloud-based IoT system, not an automatic/intelligent control system. The source deck had local PHP/MySQL server architecture and AUTO logic based on a fixed temperature threshold and PIR timeout. This v3 replaces that design with public HTTPS backend hosting plus Supabase PostgreSQL.

## Functional design

- Two-room sensing with one ESP32.
- DHT22 and PIR are monitoring inputs only.
- No automatic rule may toggle relays based on temperature or PIR.
- Fan is toggled by the dashboard or by a physical per-room button.
- Sensor telemetry, fan commands, state, and history are stored in Supabase PostgreSQL.
- Device commands are queued in the database and polled by ESP32.
- Dashboard is protected by login session; device endpoints use a separate device token.
- Database credentials stay on backend. Browser and ESP32 do not contain the database password or Supabase secret key.

## Source files changed

- `app.py`: Flask API changed from PyMySQL/MySQL to psycopg/PostgreSQL; adds session login, CSRF check, device token protection, Supabase-backed telemetry, remote command queue, command acknowledgment, local button sync, and histories.
- `schema.sql`: PostgreSQL schema for device, classroom current state, sensor history, fan command queue, and fan history; RLS enabled and public/ authenticated table grants revoked.
- `firmware/smart_classroom_fan.ino`: no temperature or PIR-based fan automation; physical buttons toggle relays; telemetry and cloud command polling use HTTPS API.
- `templates/index.html`: monitoring-only sensor cards and direct ON/OFF dashboard controls; removes AUTO/MANUAL and temperature threshold controls.
- `templates/login.html`: adds dashboard login screen.
- `README.md`: setup, Supabase, deployment, cloud data flow, TLS, hardware, and testing instructions.
- `API.md`: endpoint and payload contract.

## Static checks performed

- Python source compiled with `py_compile`.
- Dashboard JavaScript checked with `node --check`.
- Archive contents listed and checked.

## Not live-tested

- Arduino compilation against the user's exact ESP32 board core and installed libraries.
- Import/execution of `schema.sql` on a real Supabase project.
- Cloud database connection using the user's connection string.
- Hosting deployment, HTTPS certificate validation, dashboard session login in deployed environment.
- ESP32 Wi-Fi/API communication.
- GPIO relay behavior, sensors, push buttons, 24V fan load, startup current, or power adapter under simultaneous load.

Do not describe this package as proven live-working until those tests pass. In particular, firmware currently leaves `INSECURE_TLS_FOR_TESTING=true` for prototype bring-up; production deployment must set it to `false` and provide a trusted root CA certificate in `ROOT_CA_CERT`.
