# API Contract — Smart Classroom Fan v4

Base URL: the Vercel HTTPS origin, e.g. `https://YOUR-VERCEL-DOMAIN`.

## Authentication

- Device endpoints require `X-Device-Token: <DEVICE_TOKEN>`.
- Dashboard endpoints require a Flask login session.
- Dashboard POST requests require `X-CSRF-Token` from the dashboard meta tag.
- Database credentials stay in Vercel environment variables and are never sent to the browser or ESP32.

## Control model

Every classroom independently stores:
- `control_mode`: `AUTO` or `MANUAL`.
- `control_version`: increasing integer revision. It changes when a dashboard command or a physical button state is synchronized.
- `fan_on`: latest actual relay output state reported by ESP32.
- `desired_fan_on`: cloud desired/last-confirmed state.

AUTO logic executes locally on ESP32:
- Temperature >= 30°C and PIR motion -> ON.
- Temperature < 30°C -> OFF.
- No motion for 5 minutes -> OFF.
- A DHT11 read failure fails safe and does not start the fan automatically.

MANUAL is entered by either physical button or dashboard ON/OFF. Dashboard can return a room to AUTO. Cloud failure does not block local control.

## `GET /api/health`

Public health check; also verifies Supabase connectivity.

Success:
```json
{"ok":true,"database":"supabase-postgres","connected":true,"classrooms_initialized":2,"initialized":true}
```

## `POST /api/device/telemetry`

Device token required. Sends latest sensor data and relay state for both classes:

```json
{
  "classes": {
    "A": {"temperature": 29.4, "humidity": 62.0, "motion": true, "fan_on": false},
    "B": {"temperature": 30.1, "humidity": 67.0, "motion": false, "fan_on": true}
  }
}
```

DHT11 failure is represented by `temperature: null` and `humidity: null`. `motion` and `fan_on` must be booleans. Telemetry updates sensor state and the actual relay state. In AUTO, `desired_fan_on` is mirrored from the actual state. In MANUAL, telemetry does not overwrite the operator's desired state. It also updates the device heartbeat and inserts sensor history.

## `GET /api/device/commands`

Device token required. Returns control state and any pending dashboard command:

```json
{
  "ok": true,
  "classes": {
    "A": {"command_id": 41, "fan_on": true, "control_mode": "MANUAL", "control_version": 3},
    "B": {"command_id": 0, "fan_on": false, "control_mode": "AUTO", "control_version": 0}
  }
}
```

- `command_id > 0`: pending dashboard command to apply and acknowledge.
- `command_id = 0`: no pending dashboard command; `control_mode` and `control_version` are still authoritative and must be synchronized if the version changed.
- Device polls every 5 seconds.

## `POST /api/device/ack`

Device token required. Confirms that ESP32 set its relay GPIO to the requested state:

```json
{"class_id":"A","command_id":41,"fan_on":true,"control_mode":"MANUAL"}
```

This is a GPIO/firmware confirmation, not measured motor rotation. Acknowledgement does not increment `control_version`.

## `POST /api/device/local-control`

Device token required. Synchronizes a physical button toggle to the cloud:

```json
{"class_id":"A","fan_on":true}
```

The API sets that room to MANUAL, increments its control version, and supersedes pending dashboard commands so a stale remote command cannot override the physical button.

## `POST /api/control`

Dashboard session + CSRF required.

Manual command:
```json
{"class_id":"B","fan_on":true,"control_mode":"MANUAL"}
```

Return one class to automatic mode:
```json
{"class_id":"B","control_mode":"AUTO"}
```

The API updates that room's mode/revision and queues a device command. In AUTO, ESP32 uses its local sensor values to decide the relay state. In MANUAL, the dashboard-provided `fan_on` becomes the override state.

## `GET /api/status`

Dashboard session required. Returns:
- Device online status and last heartbeat.
- Per-class temperature, humidity, and PIR.
- Actual relay state and desired state.
- Per-class `control_mode`.
- Pending command indicator.

## `GET /api/history?limit=30&class_id=A`

Dashboard session required. Sensor history. `class_id` is optional and must be A or B.

## `GET /api/fan-history?limit=40&class_id=B`

Dashboard session required. Fan transition history with source: `dashboard`, `physical_button`, or `device_sync`.
