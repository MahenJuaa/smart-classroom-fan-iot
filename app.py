"""Cloud IoT API and protected dashboard for the two-room Smart Classroom Fan."""

from __future__ import annotations

import hmac
import math
import os
import secrets
from datetime import datetime, timedelta
from functools import wraps
from pathlib import Path
from zoneinfo import ZoneInfo

import psycopg
from dotenv import load_dotenv
from flask import Flask, jsonify, redirect, render_template, request, session, url_for
from psycopg.rows import dict_row

BASE_DIR = Path(__file__).resolve().parent
load_dotenv(BASE_DIR / ".env")

app = Flask(__name__)
CONFIGURED_SECRET_KEY = os.getenv("SECRET_KEY", "").strip()
app.config.update(
    SECRET_KEY=CONFIGURED_SECRET_KEY or secrets.token_hex(32),
    SESSION_COOKIE_HTTPONLY=True,
    SESSION_COOKIE_SAMESITE="Lax",
    SESSION_COOKIE_SECURE=os.getenv("COOKIE_SECURE", "true").lower() == "true",
    PERMANENT_SESSION_LIFETIME=timedelta(hours=8),
    JSON_SORT_KEYS=False,
)

DATABASE_URL = os.getenv("DATABASE_URL", "").strip()
DEVICE_TOKEN = os.getenv("DEVICE_TOKEN", "").strip()
ADMIN_USERNAME = os.getenv("ADMIN_USERNAME", "admin").strip()
ADMIN_PASSWORD = os.getenv("ADMIN_PASSWORD", "").strip()
DEVICE_ONLINE_SECONDS = max(10, int(os.getenv("DEVICE_ONLINE_SECONDS", "20")))
VALID_CLASSES = {"A", "B"}


def db():
    """Open a TLS-protected connection to Supabase Postgres."""
    if not DATABASE_URL:
        raise RuntimeError("DATABASE_URL belum dikonfigurasi")
    return psycopg.connect(
        DATABASE_URL,
        row_factory=dict_row,
        connect_timeout=8,
        sslmode="require",
        prepare_threshold=None,
    )


def json_object():
    value = request.get_json(silent=True)
    return value if isinstance(value, dict) else None


def numeric_value(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    result = float(value)
    return result if math.isfinite(result) else None


def bool_value(value):
    if isinstance(value, bool):
        return value
    if isinstance(value, int) and value in (0, 1):
        return bool(value)
    return None


def class_id_value(value):
    return value if isinstance(value, str) and value in VALID_CLASSES else None


def timestamp_text(value):
    if value is None:
        return None
    if isinstance(value, datetime):
        return value.astimezone(ZoneInfo("Asia/Jakarta")).strftime("%Y-%m-%d %H:%M:%S WIB")
    return str(value)


def device_required(fn):
    @wraps(fn)
    def wrapper(*args, **kwargs):
        if not DEVICE_TOKEN or len(DEVICE_TOKEN) < 32:
            return jsonify({"ok": False, "error": "DEVICE_TOKEN belum dikonfigurasi dengan benar"}), 503
        supplied = request.headers.get("X-Device-Token", "")
        if not hmac.compare_digest(supplied, DEVICE_TOKEN):
            return jsonify({"ok": False, "error": "Token perangkat tidak valid"}), 401
        return fn(*args, **kwargs)
    return wrapper


def admin_required(fn):
    @wraps(fn)
    def wrapper(*args, **kwargs):
        if not session.get("logged_in"):
            if request.path.startswith("/api/"):
                return jsonify({"ok": False, "error": "Sesi login berakhir. Silakan login kembali."}), 401
            return redirect(url_for("login"))
        return fn(*args, **kwargs)
    return wrapper


def csrf_required(fn):
    @wraps(fn)
    def wrapper(*args, **kwargs):
        supplied = request.headers.get("X-CSRF-Token", "")
        expected = session.get("csrf_token", "")
        supplied = supplied or request.form.get("csrf_token", "")
        if not expected or not hmac.compare_digest(supplied, expected):
            return jsonify({"ok": False, "error": "Token keamanan sesi tidak valid. Muat ulang dashboard."}), 403
        return fn(*args, **kwargs)
    return wrapper


def database_error():
    app.logger.exception("Supabase Postgres request failed")
    return jsonify({"ok": False, "error": "Database cloud tidak dapat diakses. Periksa DATABASE_URL dan status Supabase."}), 503


def add_fan_log(cur, class_id, fan_on, source):
    cur.execute(
        "INSERT INTO fan_logs (class_id, fan_on, source) VALUES (%s, %s, %s)",
        (class_id, fan_on, source),
    )


def load_status():
    with db() as conn:
        with conn.cursor() as cur:
            cur.execute(
                """SELECT last_seen,
                          EXTRACT(EPOCH FROM (NOW() - last_seen)) AS seconds_since_seen
                   FROM device_state WHERE id = 1"""
            )
            device = cur.fetchone()
            cur.execute(
                """SELECT c.class_id, c.temperature, c.humidity, c.motion, c.fan_on,
                          c.desired_fan_on, c.sensor_updated_at, c.updated_at,
                          EXISTS (SELECT 1 FROM fan_commands f
                                  WHERE f.class_id=c.class_id AND f.status='pending') AS has_pending_command
                   FROM classroom_state c ORDER BY c.class_id"""
            )
            rows = cur.fetchall()

    if not device:
        return None
    age = device["seconds_since_seen"]
    online = age is not None and float(age) <= DEVICE_ONLINE_SECONDS
    classes = {}
    for row in rows:
        cid = row["class_id"]
        classes[cid] = {
            "temperature": float(row["temperature"]) if row["temperature"] is not None else None,
            "humidity": float(row["humidity"]) if row["humidity"] is not None else None,
            "motion": bool(row["motion"]),
            "fan_on": bool(row["fan_on"]),
            "desired_fan_on": bool(row["desired_fan_on"]),
            "command_pending": bool(row["has_pending_command"]),
            "sensor_updated_at": timestamp_text(row["sensor_updated_at"]),
            "updated_at": timestamp_text(row["updated_at"]),
        }
    return {
        "ok": True,
        "online": online,
        "last_seen": timestamp_text(device["last_seen"]),
        "classes": classes,
    }


@app.after_request
def security_headers(response):
    response.headers.setdefault("X-Content-Type-Options", "nosniff")
    response.headers.setdefault("X-Frame-Options", "DENY")
    response.headers.setdefault("Referrer-Policy", "strict-origin-when-cross-origin")
    response.headers.setdefault("Cache-Control", "no-store")
    return response


@app.route("/login", methods=["GET", "POST"])
def login():
    if session.get("logged_in"):
        return redirect(url_for("dashboard"))
    error = None
    if request.method == "POST":
        username = request.form.get("username", "")
        password = request.form.get("password", "")
        if not ADMIN_PASSWORD or len(CONFIGURED_SECRET_KEY) < 32:
            error = "Konfigurasi ADMIN_PASSWORD atau SECRET_KEY belum lengkap di environment."
        elif hmac.compare_digest(username, ADMIN_USERNAME) and hmac.compare_digest(password, ADMIN_PASSWORD):
            session.clear()
            session.permanent = True
            session["logged_in"] = True
            session["csrf_token"] = secrets.token_urlsafe(32)
            return redirect(url_for("dashboard"))
        else:
            error = "Username atau password tidak sesuai."
    return render_template("login.html", error=error)


@app.post("/logout")
@admin_required
@csrf_required
def logout():
    session.clear()
    return redirect(url_for("login"))


@app.get("/")
@admin_required
def dashboard():
    return render_template("index.html", csrf_token=session["csrf_token"])


@app.get("/api/health")
def health():
    try:
        with db() as conn:
            with conn.cursor() as cur:
                cur.execute("SELECT 1 AS alive")
                cur.fetchone()
                cur.execute("SELECT COUNT(*) AS total FROM classroom_state WHERE class_id IN ('A','B')")
                room_count = int(cur.fetchone()["total"])
        return jsonify({"ok": True, "database": "supabase-postgres", "connected": True,
                        "classrooms_initialized": room_count, "initialized": room_count == 2})
    except Exception:
        return database_error()


@app.get("/api/status")
@admin_required
def status():
    try:
        result = load_status()
        if result is None or set(result["classes"]) != VALID_CLASSES:
            return jsonify({"ok": False, "error": "Jalankan schema.sql di Supabase terlebih dahulu."}), 503
        return jsonify(result)
    except Exception:
        return database_error()


@app.post("/api/device/telemetry")
@device_required
def receive_telemetry():
    data = json_object()
    classes = data.get("classes") if data else None
    if not isinstance(classes, dict) or not all(cid in classes for cid in VALID_CLASSES):
        return jsonify({"ok": False, "error": "Payload harus memuat classes.A dan classes.B."}), 400

    validated = {}
    for cid in ("A", "B"):
        item = classes.get(cid)
        if not isinstance(item, dict):
            return jsonify({"ok": False, "error": f"Data Kelas {cid} tidak valid."}), 400
        temperature = item.get("temperature")
        humidity = item.get("humidity")
        if temperature is not None:
            temperature = numeric_value(temperature)
            if temperature is None or not -20 <= temperature <= 80:
                return jsonify({"ok": False, "error": f"Suhu Kelas {cid} tidak valid."}), 400
        if humidity is not None:
            humidity = numeric_value(humidity)
            if humidity is None or not 0 <= humidity <= 100:
                return jsonify({"ok": False, "error": f"Kelembapan Kelas {cid} tidak valid."}), 400
        if (temperature is None) != (humidity is None):
            return jsonify({"ok": False, "error": f"Suhu dan kelembapan Kelas {cid} harus angka atau null bersamaan."}), 400
        motion = bool_value(item.get("motion"))
        fan_on = bool_value(item.get("fan_on"))
        if motion is None or fan_on is None:
            return jsonify({"ok": False, "error": f"motion dan fan_on Kelas {cid} harus boolean."}), 400
        validated[cid] = {"temperature": temperature, "humidity": humidity,
                          "motion": motion, "fan_on": fan_on}

    try:
        with db() as conn:
            with conn.cursor() as cur:
                cur.execute("UPDATE device_state SET last_seen = NOW() WHERE id = 1")
                for cid, item in validated.items():
                    cur.execute("SELECT fan_on FROM classroom_state WHERE class_id = %s FOR UPDATE", (cid,))
                    previous = cur.fetchone()
                    if not previous:
                        return jsonify({"ok": False, "error": f"Kelas {cid} belum diinisialisasi."}), 503
                    cur.execute(
                        """UPDATE classroom_state
                           SET temperature=%s, humidity=%s, motion=%s, fan_on=%s,
                               sensor_updated_at=NOW(), updated_at=NOW()
                           WHERE class_id=%s""",
                        (item["temperature"], item["humidity"], item["motion"], item["fan_on"], cid),
                    )
                    cur.execute(
                        """INSERT INTO sensor_logs
                           (class_id, temperature, humidity, motion)
                           VALUES (%s, %s, %s, %s)""",
                        (cid, item["temperature"], item["humidity"], item["motion"]),
                    )
                    if bool(previous["fan_on"]) != item["fan_on"]:
                        add_fan_log(cur, cid, item["fan_on"], "device_sync")
        return jsonify({"ok": True})
    except Exception:
        return database_error()


@app.get("/api/device/commands")
@device_required
def device_commands():
    try:
        with db() as conn:
            with conn.cursor() as cur:
                cur.execute("UPDATE device_state SET last_seen=NOW() WHERE id=1")
                cur.execute(
                    """SELECT c.class_id, c.desired_fan_on,
                              COALESCE((SELECT MAX(f.id) FROM fan_commands f
                                        WHERE f.class_id=c.class_id AND f.status='pending'), 0) AS command_id
                       FROM classroom_state c ORDER BY c.class_id"""
                )
                rows = cur.fetchall()
        classes = {row["class_id"]: {"command_id": int(row["command_id"]),
                                     "fan_on": bool(row["desired_fan_on"])} for row in rows}
        if set(classes) != VALID_CLASSES:
            return jsonify({"ok": False, "error": "Kelas A dan B belum diinisialisasi."}), 503
        return jsonify({"ok": True, "classes": classes})
    except Exception:
        return database_error()


@app.post("/api/device/ack")
@device_required
def acknowledge_command():
    data = json_object()
    if not data:
        return jsonify({"ok": False, "error": "Payload JSON tidak valid."}), 400
    cid = class_id_value(data.get("class_id"))
    command_id = data.get("command_id")
    fan_on = bool_value(data.get("fan_on"))
    if cid is None or isinstance(command_id, bool) or not isinstance(command_id, int) or command_id < 1 or fan_on is None:
        return jsonify({"ok": False, "error": "class_id, command_id, dan fan_on tidak valid."}), 400
    try:
        with db() as conn:
            with conn.cursor() as cur:
                cur.execute(
                    """UPDATE fan_commands SET status='applied', applied_at=NOW()
                       WHERE id=%s AND class_id=%s AND status='pending' AND desired_fan_on=%s
                       RETURNING id""", (command_id, cid, fan_on)
                )
                applied = cur.fetchone()
                if not applied:
                    return jsonify({"ok": True, "ignored": True, "reason": "Perintah sudah diproses atau digantikan."})
                cur.execute("SELECT fan_on FROM classroom_state WHERE class_id=%s FOR UPDATE", (cid,))
                previous = cur.fetchone()
                cur.execute(
                    """UPDATE classroom_state SET fan_on=%s, desired_fan_on=%s, updated_at=NOW()
                       WHERE class_id=%s""", (fan_on, fan_on, cid)
                )
                if previous and bool(previous["fan_on"]) != fan_on:
                    add_fan_log(cur, cid, fan_on, "dashboard")
                cur.execute("UPDATE device_state SET last_seen=NOW() WHERE id=1")
        return jsonify({"ok": True, "class_id": cid, "fan_on": fan_on})
    except Exception:
        return database_error()


@app.post("/api/device/local-control")
@device_required
def physical_control_sync():
    data = json_object()
    if not data:
        return jsonify({"ok": False, "error": "Payload JSON tidak valid."}), 400
    cid = class_id_value(data.get("class_id"))
    fan_on = bool_value(data.get("fan_on"))
    if cid is None or fan_on is None:
        return jsonify({"ok": False, "error": "class_id A/B dan fan_on boolean wajib."}), 400
    try:
        with db() as conn:
            with conn.cursor() as cur:
                cur.execute("SELECT fan_on FROM classroom_state WHERE class_id=%s FOR UPDATE", (cid,))
                previous = cur.fetchone()
                if not previous:
                    return jsonify({"ok": False, "error": f"Kelas {cid} belum ada di database."}), 503
                cur.execute("UPDATE fan_commands SET status='superseded' WHERE class_id=%s AND status='pending'", (cid,))
                cur.execute(
                    """UPDATE classroom_state SET fan_on=%s, desired_fan_on=%s, updated_at=NOW()
                       WHERE class_id=%s""", (fan_on, fan_on, cid)
                )
                if bool(previous["fan_on"]) != fan_on:
                    add_fan_log(cur, cid, fan_on, "physical_button")
                cur.execute("UPDATE device_state SET last_seen=NOW() WHERE id=1")
        return jsonify({"ok": True, "class_id": cid, "fan_on": fan_on})
    except Exception:
        return database_error()


@app.post("/api/control")
@admin_required
@csrf_required
def control_fan():
    data = json_object()
    if not data:
        return jsonify({"ok": False, "error": "Payload JSON tidak valid."}), 400
    cid = class_id_value(data.get("class_id"))
    fan_on = bool_value(data.get("fan_on"))
    if cid is None or fan_on is None:
        return jsonify({"ok": False, "error": "Kirim class_id A/B dan fan_on boolean."}), 400
    try:
        with db() as conn:
            with conn.cursor() as cur:
                cur.execute("SELECT class_id FROM classroom_state WHERE class_id=%s FOR UPDATE", (cid,))
                if not cur.fetchone():
                    return jsonify({"ok": False, "error": f"Kelas {cid} belum diinisialisasi."}), 503
                cur.execute("UPDATE fan_commands SET status='superseded' WHERE class_id=%s AND status='pending'", (cid,))
                cur.execute(
                    """UPDATE classroom_state SET desired_fan_on=%s, updated_at=NOW()
                       WHERE class_id=%s""", (fan_on, cid)
                )
                cur.execute(
                    """INSERT INTO fan_commands (class_id, desired_fan_on, source)
                       VALUES (%s, %s, 'dashboard') RETURNING id""", (cid, fan_on)
                )
                command_id = int(cur.fetchone()["id"])
        return jsonify({"ok": True, "class_id": cid, "desired_fan_on": fan_on,
                        "command_id": command_id, "queued": True})
    except Exception:
        return database_error()


@app.get("/api/history")
@admin_required
def sensor_history():
    try:
        limit = min(max(int(request.args.get("limit", "30")), 1), 200)
    except ValueError:
        return jsonify({"ok": False, "error": "limit harus angka 1–200."}), 400
    cid = request.args.get("class_id")
    if cid is not None and class_id_value(cid) is None:
        return jsonify({"ok": False, "error": "class_id harus A atau B."}), 400
    try:
        with db() as conn:
            with conn.cursor() as cur:
                if cid:
                    cur.execute("""SELECT id, class_id, temperature, humidity, motion, created_at
                                   FROM sensor_logs WHERE class_id=%s ORDER BY id DESC LIMIT %s""", (cid, limit))
                else:
                    cur.execute("""SELECT id, class_id, temperature, humidity, motion, created_at
                                   FROM sensor_logs ORDER BY id DESC LIMIT %s""", (limit,))
                rows = cur.fetchall()
        items = [{"id": row["id"], "class_id": row["class_id"],
                  "temperature": float(row["temperature"]) if row["temperature"] is not None else None,
                  "humidity": float(row["humidity"]) if row["humidity"] is not None else None,
                  "motion": bool(row["motion"]), "created_at": timestamp_text(row["created_at"])} for row in rows]
        return jsonify({"ok": True, "items": items})
    except Exception:
        return database_error()


@app.get("/api/fan-history")
@admin_required
def fan_history():
    try:
        limit = min(max(int(request.args.get("limit", "40")), 1), 200)
    except ValueError:
        return jsonify({"ok": False, "error": "limit harus angka 1–200."}), 400
    cid = request.args.get("class_id")
    if cid is not None and class_id_value(cid) is None:
        return jsonify({"ok": False, "error": "class_id harus A atau B."}), 400
    try:
        with db() as conn:
            with conn.cursor() as cur:
                if cid:
                    cur.execute("""SELECT id, class_id, fan_on, source, created_at
                                   FROM fan_logs WHERE class_id=%s ORDER BY id DESC LIMIT %s""", (cid, limit))
                else:
                    cur.execute("""SELECT id, class_id, fan_on, source, created_at
                                   FROM fan_logs ORDER BY id DESC LIMIT %s""", (limit,))
                rows = cur.fetchall()
        items = [{"id": row["id"], "class_id": row["class_id"], "fan_on": bool(row["fan_on"]),
                  "source": row["source"], "created_at": timestamp_text(row["created_at"])} for row in rows]
        return jsonify({"ok": True, "items": items})
    except Exception:
        return database_error()


if __name__ == "__main__":
    # Local development only. Public deployment should use Gunicorn behind HTTPS.
    app.run(host="0.0.0.0", port=int(os.getenv("PORT", "5000")), debug=False)
