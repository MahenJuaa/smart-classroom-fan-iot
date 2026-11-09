# API Contract — Smart Classroom Fan v3

Base URL adalah domain backend publik dengan HTTPS, misalnya `https://your-app.onrender.com`.

## Authentication

- Endpoint perangkat membutuhkan header `X-Device-Token: <DEVICE_TOKEN>`.
- Endpoint dashboard membutuhkan sesi login Flask.
- Request `POST` dari dashboard juga membutuhkan header `X-CSRF-Token` yang disediakan di meta tag dashboard.
- Database credential hanya disimpan di environment backend.

## `GET /api/health`

Public health check. Example response:

```json
{
  "ok": true,
  "database": "supabase-postgres",
  "connected": true,
  "classrooms_initialized": 2,
  "initialized": true
}
```

## `POST /api/device/telemetry`

Device token required. ESP32 sends the latest sensor data and relay output state for both classes.

```json
{
  "classes": {
    "A": {
      "temperature": 29.4,
      "humidity": 62.0,
      "motion": true,
      "fan_on": false
    },
    "B": {
      "temperature": 30.1,
      "humidity": 67.0,
      "motion": false,
      "fan_on": true
    }
  }
}
```

DHT22 failure is represented by `temperature: null` and `humidity: null`. `motion` and `fan_on` must remain booleans. Telemetry inserts one record for each room into `sensor_logs`, updates current room state and updates device heartbeat.

## `GET /api/device/commands`

Device token required. ESP32 polls every 2 seconds.

```json
{
  "ok": true,
  "classes": {
    "A": {"command_id": 41, "fan_on": true},
    "B": {"command_id": 0, "fan_on": false}
  }
}
```

`command_id > 0` means a pending dashboard command. `command_id = 0` means no unprocessed dashboard command for that class; `fan_on` remains the desired cloud state for initial synchronization.

## `POST /api/device/ack`

Device token required. Acknowledges that the ESP32 has set the relay GPIO to the requested state.

```json
{"class_id":"A", "command_id":41, "fan_on":true}
```

This confirms the MCU output state, not measured motor rotation. The hardware has no RPM/current feedback sensor.

## `POST /api/device/local-control`

Device token required. Synchronizes a physical button toggle to the cloud.

```json
{"class_id":"A", "fan_on":true}
```

The latest pending remote command for that class is marked superseded so it cannot unexpectedly override the physical button action.

## `POST /api/control`

Dashboard session + CSRF token required. Creates a queued remote control command. Command can be queued even while the device is offline; it is applied when the ESP32 reconnects.

```json
{"class_id":"B", "fan_on":true}
```

Example response:

```json
{"ok":true,"class_id":"B","desired_fan_on":true,"command_id":42,"queued":true}
```

## `GET /api/status`

Dashboard session required. Returns device online status, current sensor values, actual relay state (`fan_on`), desired state (`desired_fan_on`) and pending indicator for both classes.

## `GET /api/history?limit=30&class_id=A`

Dashboard session required. Returns sensor records. `class_id` is optional; when provided, it must be `A` or `B`. `limit` is clamped to 1–200.

## `GET /api/fan-history?limit=40&class_id=B`

Dashboard session required. Returns fan change events and source: `dashboard`, `physical_button`, or `device_sync`.
