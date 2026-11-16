# SmartCold Guard — Step 9: send ESP32-S3 data to your public Render website

This is a **firmware-only update**. Keep your existing `server.js`, `dashboard.html`, and Supabase database unchanged.

Public website: https://smartcold-guard.onrender.com/dashboard

## 1. Confirm Render is configured

In Render Dashboard → your `smartcold-guard` service → Environment, check these variable **names** exist with nonempty values:

```bash
NODE_ENV=production
SUPABASE_URL=...
SUPABASE_SECRET_KEY=...
DEVICE_TOKEN=...
DASHBOARD_USERNAME=...
DASHBOARD_PASSWORD=...
```

**Do not send or upload the values.** `DEVICE_TOKEN` is a separate secret from your dashboard password and Supabase key. If you have not created a token, generate one on your Windows laptop:

```bash
node -e "console.log(require('crypto').randomBytes(24).toString('hex'))"
```

Set it as Render's `DEVICE_TOKEN` and choose **Save and deploy**.

## 2. Install one additional Arduino library

Open Arduino IDE → Sketch → Include Library → Manage Libraries. Search `ESP32CertBundle` (by TANAKA Masayuki) and install it. It supplies an up-to-date X.509 CA trust bundle to validate HTTPS. The sketch also uses DHT, MFRC522, and TinyGPSPlus, which you already installed.

Library source: https://github.com/tanakamasayuki/ESP32CertBundle

## 3. Edit the Arduino sketch

Open `SmartColdGuard_Step9_Online.ino`. Change only these three string values:

```cpp
const char *WIFI_SSID = "YOUR_WIFI_NAME";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
const char *DEVICE_TOKEN = "PASTE_RENDER_DEVICE_TOKEN_HERE";
```

Use the exact `DEVICE_TOKEN` from Render's Environment page. **Do not paste `DASHBOARD_PASSWORD` or `SUPABASE_SECRET_KEY` into Arduino.**

The sketch is already set to the correct online domain:

```cpp
const char *SERVER_BASE = "https://smartcold-guard.onrender.com";
```

## 4. Upload and verify

1. Connect the ESP32-S3 to your laptop with USB.
2. Select **ESP32S3 Dev Module** and your actual COM port.
3. Keep **USB CDC On Boot = Enabled**.
4. Click Verify, then Upload.
5. Open Serial Monitor at 115200 baud.
6. Wait for `UTC time synchronized; HTTPS enabled.` and `POST /api/sensor-data => HTTP 200`.
7. Scan RFID card and check `POST /api/rfid-event => HTTP 200`.
8. Open your Render dashboard in a browser, log in, and see live data and RFID events.

If POST returns:

- **401**: Device token mismatch. Check Arduino `DEVICE_TOKEN` against Render `DEVICE_TOKEN` (not dashboard password).
- **400**: Data format rejected; send the whole JSON output, **redacting credentials**.
- **503**: Backend database write failed; check Render Logs and Supabase permissions.
- **negative code**: Network/TLS/timeout problem; check Wi-Fi, NTP clock, and HTTPS cert library.

## Notes

- Your DHT22, HC-SR04, RFID, and GPS wiring does not change.
- GPS coordinates remain null until a satellite fix is available.
- Render Free can sleep after 15 minutes idle; the first request can time out during wakeup. Subsequent retries should work.
- `POST /api/sensor-data` sends every 5 seconds; Supabase history persists approximately every 30 seconds.
- The firmware does not yet drive the LDR light sensor or buzzer. They can be added later.
- Keep the `.ino` file **private** once credentials have been inserted. If you ever publish it, remove all secrets first.

References:
- Render: https://render.com/docs/free
- Certificate library: https://github.com/tanakamasayuki/ESP32CertBundle
- Espressif NetworkClientSecure: https://github.com/espressif/arduino-esp32/blob/master/libraries/NetworkClientSecure/README.md
