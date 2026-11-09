# Smart Classroom Fan v3 — Cloud IoT + Supabase

Versi ini mengubah proyek menjadi **sistem monitoring dan kontrol IoT berbasis cloud**, bukan sistem kontrol otomatis/cerdas.

## 1. Perilaku sistem yang dituju

- DHT22 mengukur suhu dan kelembapan Kelas A dan B.
- PIR mendeteksi gerakan pada masing-masing kelas. Nilainya hanya ditampilkan dan disimpan sebagai data monitoring.
- ESP32 mengirim data sensor ke backend melalui internet.
- Backend menyimpan status terakhir dan riwayat ke **Supabase PostgreSQL**.
- Dashboard web dapat dibuka dari luar jaringan lokal setelah backend di-deploy ke hosting publik dengan HTTPS.
- Petugas menyalakan/mematikan kipas Kelas A dan B secara terpisah melalui dashboard.
- Tombol fisik di tiap kelas juga berfungsi untuk toggle kipas kelas tersebut.
- Tidak ada ambang suhu, timer occupancy, mode AUTO, atau aturan otomatis PIR/DHT22 yang mengubah status kipas.

**Catatan status kipas:** data `fan_on` menunjukkan status perintah GPIO/relay yang dilaporkan ESP32. Karena prototipe tidak memakai sensor arus atau sensor RPM, dashboard tidak bisa membuktikan bahwa motor kipas benar-benar berputar.

## 2. Arsitektur cloud

```text
KELAS A: DHT22 A + PIR A + Button A + Relay A + Fan A
KELAS B: DHT22 B + PIR B + Button B + Relay B + Fan B
                         |
                         v
                  ESP32 DevKit V1
               baca sensor + relay manual
                         |
                   Wi-Fi / Internet
                         |
                         v
              Backend Flask REST API
              (hosting publik + HTTPS)
                 /               \
                v                 v
       Supabase PostgreSQL    Dashboard Web
       status + history       monitoring + control
```

ESP32 **tidak** menyimpan Supabase database password atau Supabase secret key. ESP32 hanya mengenal URL backend dan `DEVICE_TOKEN`. Backend yang memegang koneksi database cloud. Dashboard juga berbicara ke backend, bukan langsung ke kredensial database.

## 3. Struktur folder

```text
Smart-Classroom-Fan-v3-Supabase/
├── app.py
├── schema.sql
├── requirements.txt
├── Procfile
├── .env.example
├── README.md
├── API.md
├── AUDIT.md
├── firmware/
│   └── smart_classroom_fan.ino
└── templates/
    ├── index.html
    └── login.html
```

## 4. Database Supabase

1. Buat project baru di Supabase.
2. Buka **SQL Editor** pada project tersebut.
3. Jalankan seluruh isi `schema.sql` satu kali.
4. Pastikan tabel berikut tersedia:
   - `device_state`: heartbeat/last-seen ESP32.
   - `classroom_state`: status terkini dua kelas, termasuk desired dan actual fan state.
   - `sensor_logs`: riwayat suhu, kelembapan, dan PIR.
   - `fan_commands`: antrean perintah ON/OFF yang dibuat dashboard.
   - `fan_logs`: riwayat perubahan kipas dan sumber perintah.
5. Pada **Connect** di Supabase, salin connection string PostgreSQL. Untuk hosting backend yang hanya mendukung IPv4, gunakan **Session pooler**. Ganti placeholder password dengan password database project.

Schema mengaktifkan RLS dan mencabut grant tabel dari role `anon` dan `authenticated`. Akses tabel dilakukan melalui koneksi backend yang disimpan sebagai environment variable pada hosting. Jangan pernah menyalin database password atau Supabase secret key ke HTML, JavaScript browser, firmware, GitHub, maupun ZIP yang dibagikan.

## 5. Menjalankan backend secara lokal

Python 3.10+ direkomendasikan.

### Windows PowerShell

```powershell
py -m venv .venv
.venv\Scripts\Activate.ps1
pip install -r requirements.txt
Copy-Item .env.example .env
```

Isi `.env` dengan nilai milik project:

```dotenv
DATABASE_URL=postgresql://USER:PASSWORD@POOLER_HOST:5432/postgres
SECRET_KEY=isi_dengan_random_secret_minimal_32_karakter
DEVICE_TOKEN=isi_dengan_token_acak_tersendiri_minimal_32_karakter
ADMIN_USERNAME=admin
ADMIN_PASSWORD=password_admin_yang_kuat
COOKIE_SECURE=false
DEVICE_ONLINE_SECONDS=20
PORT=5000
```

`COOKIE_SECURE=false` hanya untuk uji lokal menggunakan `http://localhost`. Saat di-host publik dengan HTTPS, ubah menjadi `true`.

Generate secret acak melalui:

```powershell
py -c "import secrets; print(secrets.token_urlsafe(48))"
```

Gunakan nilai acak yang berbeda untuk `SECRET_KEY` dan `DEVICE_TOKEN`.

Jalankan:

```powershell
python app.py
```

Buka `http://127.0.0.1:5000`, kemudian login dengan `ADMIN_USERNAME` dan `ADMIN_PASSWORD`. Health check: `http://127.0.0.1:5000/api/health`.

## 6. Deploy backend agar dapat diakses publik

Contoh target hosting: **Render Web Service**. Hosting lain yang mendukung aplikasi Python persistent web service juga dapat dipakai.

1. Push folder project ke repository GitHub privat.
2. Buat Web Service dari repository tersebut.
3. Runtime: Python.
4. Build command:

   ```bash
   pip install -r requirements.txt
   ```

5. Start command:

   ```bash
   gunicorn --bind 0.0.0.0:$PORT app:app
   ```

6. Tambahkan environment variables berikut pada dashboard hosting:
   - `DATABASE_URL`: Session pooler connection string Supabase.
   - `SECRET_KEY`: random secret minimal 32 karakter.
   - `DEVICE_TOKEN`: random token berbeda minimal 32 karakter.
   - `ADMIN_USERNAME`: username dashboard.
   - `ADMIN_PASSWORD`: password dashboard yang kuat.
   - `COOKIE_SECURE`: `true`.
   - `DEVICE_ONLINE_SECONDS`: `20`.
7. Deploy dan tunggu status service aktif.
8. Uji `https://ALAMAT-HOSTING/api/health`. Harus mengembalikan `ok: true`, `connected: true`, dan `initialized: true`.
9. Buka alamat HTTPS root untuk login dashboard.

Jangan menaruh `.env` di repository. File `.gitignore` sudah mengecualikannya. Jangan memublikasikan repository yang berisi password, token, atau kredensial database.

## 7. Konfigurasi firmware ESP32

Buka `firmware/smart_classroom_fan.ino` di Arduino IDE. Pasang library:

- DHT sensor library by Adafruit.
- Adafruit Unified Sensor.
- ArduinoJson 6.x atau 7.x.

Edit konfigurasi berikut:

```cpp
const char* WIFI_SSID = "NAMA_WIFI_2_4GHZ";
const char* WIFI_PASSWORD = "PASSWORD_WIFI";
const char* SERVER_BASE_URL = "https://ALAMAT-HOSTING";
const char* DEVICE_TOKEN = "TOKEN_YANG_SAMA_DENGAN_ENV";
```

`DEVICE_TOKEN` harus sama persis dengan environment variable di hosting.

### TLS/HTTPS

Firmware menyediakan opsi `INSECURE_TLS_FOR_TESTING`. Nilai default `true` mematikan verifikasi sertifikat HTTPS dan hanya layak untuk pengujian prototipe sementara di jaringan tepercaya. Sebelum penggunaan publik, ubah ke `false` dan isi `ROOT_CA_CERT` dengan root CA tepercaya yang memvalidasi sertifikat domain hosting. Jangan menganggap HTTPS aman hanya karena URL diawali `https://` bila verifikasi sertifikat dimatikan.

### Interval komunikasi default

- DHT22 dibaca setiap 3 detik.
- Telemetry ke cloud dikirim setiap 10 detik.
- ESP32 memeriksa perintah dashboard setiap 2 detik.
- Dashboard memperbarui status setiap 3 detik.

Interval telemetry dapat diperbesar menjadi 15–30 detik untuk mengurangi pertumbuhan tabel `sensor_logs`.

## 8. Cara kerja kontrol

### Dashboard ke kipas

1. Petugas menekan NYALAKAN atau MATIKAN pada panel kelas yang diinginkan.
2. Backend membuat baris pada `fan_commands` dan memperbarui `desired_fan_on` di Supabase.
3. ESP32 mengambil antrean perintah setiap 2 detik.
4. ESP32 mengatur GPIO relay pada kelas tersebut.
5. ESP32 mengirim acknowledgment (`/api/device/ack`).
6. Backend memperbarui `fan_on`, status perintah, dan `fan_logs`.

Jika ESP32 sedang offline, perintah dashboard tetap tersimpan sebagai antrean. Saat ESP32 tersambung kembali, perangkat mengambil perintah yang masih pending.

### Tombol fisik

- Button A mengubah relay/fan A secara langsung.
- Button B mengubah relay/fan B secara langsung.
- Setiap toggle dikirim ke backend ketika jaringan tersedia.
- Ketika internet terputus, tombol fisik tetap mengontrol relay secara lokal. Perubahan lokal disinkronkan saat koneksi kembali.

### Sensor

- DHT22 hanya mengirim suhu dan kelembapan.
- PIR hanya mengirim status gerakan HIGH/LOW.
- Nilai sensor tidak memicu relay.

## 9. Peta GPIO final

| Komponen | Kelas A | Kelas B |
|---|---:|---:|
| DHT22 DATA | GPIO16 | GPIO17 |
| PIR OUT | GPIO32 | GPIO33 |
| Relay IN | GPIO25 | GPIO26 |
| Push Button | GPIO27 | GPIO18 |

Jangan mengubah peta pin tanpa memperbarui firmware dan diagram wiring.

## 10. Pemeriksaan perangkat keras

Gunakan hasil pengecekan hardware sebelumnya sebagai acuan, dengan pemeriksaan wajib berikut:

1. DHT22 VCC 3.3V, DATA ke GPIO, GND ke GND ESP32.
2. PIR HC-SR501 VCC 5V, GND bersama GND kontrol, OUT tidak boleh melebihi batas GPIO ESP32 3.3V.
3. Relay module VCC sesuai spesifikasi modul (umumnya 5V), GND sisi kontrol ke GND ESP32, IN ke GPIO relay.
4. Pastikan input relay kompatibel dengan logika GPIO ESP32 3.3V serta pastikan active-LOW/active-HIGH sesuai modul aktual.
5. Pastikan rating kontak relay sesuai beban motor DC 24V dan arus startup motor.
6. Adaptor yang sebelumnya diperiksa bertanda 24V DC maksimum 1.5A/36W. Pastikan arus total dan startup kedua kipas tidak melampaui kemampuan adaptor dengan margin aman.
7. Jalur beban 24V tidak boleh masuk ke ESP32, expansion board, atau breadboard.
8. Adaptor +24V ke COM relay A dan B; NO A ke positif kipas A; NO B ke positif kipas B; negatif kipas kembali ke negatif adaptor. NC tidak digunakan.
9. Negatif adaptor 24V tidak perlu disatukan dengan GND ESP32 karena sisi beban dipisahkan oleh kontak relay.
10. Jangan menyambung/melepas kabel ketika adaptor terhubung ke listrik.

Uji firmware, sensor, tombol, dan relay tanpa memasang beban kipas terlebih dahulu. Pastikan relay tidak aktif saat boot sebelum memasang 24V.

## 11. Endpoint API

Detail payload ada di `API.md`.

- `GET /api/health`: cek koneksi Supabase.
- `GET /api/status`: status dua kelas (login dashboard).
- `POST /api/device/telemetry`: ESP32 mengirim data sensor dan status relay.
- `GET /api/device/commands`: ESP32 mengambil perintah dashboard.
- `POST /api/device/ack`: ESP32 mengonfirmasi perintah.
- `POST /api/device/local-control`: sinkronisasi tombol fisik.
- `POST /api/control`: dashboard mengirim perintah ON/OFF (login + CSRF).
- `GET /api/history`: riwayat sensor (login dashboard).
- `GET /api/fan-history`: riwayat kipas (login dashboard).

## 12. Checklist uji sistem

1. Supabase: jalankan `schema.sql`; pastikan 5 tabel tersedia dan dua kelas sudah terinisialisasi.
2. Backend: jalankan lokal, login dashboard, cek `/api/health`.
3. Hosting: deploy backend; health check HTTPS mengembalikan `initialized: true`.
4. Firmware: compile dan upload; pastikan ESP32 berhasil tersambung ke Wi-Fi dan API.
5. Sensor: pastikan data A hanya masuk ke Kelas A dan B hanya ke Kelas B.
6. PIR: gerakkan tangan di depan PIR A/B; status pada dashboard berubah tanpa memengaruhi relay.
7. Dashboard: ON Kelas A; relay A berubah, relay B tetap. Ulangi untuk OFF dan Kelas B.
8. Tombol fisik: tombol A hanya mengubah relay A; tombol B hanya mengubah relay B.
9. Cloud: pastikan `sensor_logs` dan `fan_logs` bertambah di Supabase.
10. Offline: putuskan internet; tombol lokal tetap bekerja. Perintah dashboard yang dibuat saat offline akan menunggu hingga perangkat kembali online.
11. Beban kipas: setelah rating relay, polaritas adaptor, dan arus startup dipastikan, uji satu kipas terlebih dahulu baru kedua kipas.

## 13. Batasan dan validasi

Paket ini adalah implementasi sumber yang disusun untuk arsitektur cloud. Sebelum bisa diklaim berjalan penuh, masih harus dilakukan compile di Arduino IDE, pengisian kredensial Supabase/hosting milik pengguna, eksekusi schema pada project Supabase, deployment, serta pengujian ESP32 dan beban kipas nyata. Paket tidak menyertakan kredensial cloud milik pengguna.
