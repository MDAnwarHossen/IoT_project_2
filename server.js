'use strict';

// SmartCold Guard Step 8: same API + dashboard as Step 7, Supabase persistence.
require('dotenv').config();
const express = require('express');
const path = require('path');
const crypto = require('crypto');
const { createClient } = require('@supabase/supabase-js');

const app = express();
const PORT = Number(process.env.PORT || 3000);
const SUPABASE_URL = process.env.SUPABASE_URL;
const SUPABASE_SECRET_KEY = process.env.SUPABASE_SECRET_KEY;
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || '';
const DASHBOARD_USERNAME = process.env.DASHBOARD_USERNAME || 'demo';
const DASHBOARD_PASSWORD = process.env.DASHBOARD_PASSWORD || '';
const STORE_EVERY_MS = 30000; // Database history: one sample / 30 seconds

if (!SUPABASE_URL || !SUPABASE_SECRET_KEY) {
  console.error('Missing SUPABASE_URL or SUPABASE_SECRET_KEY. See .env.example');
  process.exit(1);
}
if (process.env.NODE_ENV === 'production' && (!DEVICE_TOKEN || !DASHBOARD_PASSWORD)) {
  console.error('Production needs DEVICE_TOKEN and DASHBOARD_PASSWORD environment variables.');
  process.exit(1);
}

const db = createClient(SUPABASE_URL, SUPABASE_SECRET_KEY, {
  auth: { autoRefreshToken: false, persistSession: false }
});

app.disable('x-powered-by');
app.use(express.json({ limit: '10kb' }));

let latestReading = null;
let lastStoredMs = 0;

function sameSecret(a, b) {
  const aBuf = Buffer.from(String(a || ''));
  const bBuf = Buffer.from(String(b || ''));
  return aBuf.length === bBuf.length && crypto.timingSafeEqual(aBuf, bBuf);
}
function deviceAuth(req, res, next) {
  // Local trusted-LAN testing works without DEVICE_TOKEN. ALWAYS set it on Render.
  if (DEVICE_TOKEN && !sameSecret(req.get('X-Device-Token'), DEVICE_TOKEN)) {
    return res.status(401).json({ error: 'Invalid device token' });
  }
  next();
}
function dashboardAuth(req, res, next) {
  // Local testing works without a password. ALWAYS set it on Render.
  if (!DASHBOARD_PASSWORD) return next();
  const header = req.get('authorization') || '';
  let decoded = '';
  if (header.startsWith('Basic ')) {
    try { decoded = Buffer.from(header.slice(6), 'base64').toString('utf8'); }
    catch (_err) { /* invalid header */ }
  }
  const separator = decoded.indexOf(':');
  const username = separator >= 0 ? decoded.slice(0, separator) : '';
  const password = separator >= 0 ? decoded.slice(separator + 1) : '';
  if (!sameSecret(username, DASHBOARD_USERNAME) ||
      !sameSecret(password, DASHBOARD_PASSWORD)) {
    res.set('WWW-Authenticate', 'Basic realm="SmartCold Guard", charset="UTF-8"');
    return res.status(401).send('Dashboard login required');
  }
  next();
}
function validNumberOrNull(v) {
  return v === null || (typeof v === 'number' && Number.isFinite(v));
}
function wrap(handler) {
  return (req, res, next) => Promise.resolve(handler(req, res)).catch(next);
}
function toReading(r) {
  return {
    deviceId: r.device_id,
    temperatureC: r.temperature_c,
    humidityPercent: r.humidity_percent,
    distanceCm: r.distance_cm,
    ldrRawValue: r.ldr_raw_value ?? null,
    gps: {
      hasFix: r.gps_has_fix,
      latitude: r.gps_latitude,
      longitude: r.gps_longitude,
      speedKmph: r.gps_speed_kmph,
      charsProcessed: r.gps_chars_processed
    },
    receivedAt: r.received_at
  };
}
function toEvent(r) {
  return {type: r.event_type, uid: r.uid, receivedAt: r.received_at};
}

app.get('/', (_req, res) => res.send('SmartCold Guard Server is Running! Dashboard: /dashboard'));
app.get('/api/status', (_req, res) => res.json({
  project: 'SmartCold Guard', server: 'online', device: 'ESP32-S3',
  lastReadingAt: latestReading?.receivedAt || null,
  database: 'configured'
}));
app.get('/dashboard', dashboardAuth, (_req, res) => {
  res.sendFile(path.join(__dirname, 'dashboard.html'));
});

app.post('/api/sensor-data', deviceAuth, wrap(async (req, res) => {
  const d = req.body;
  if (!d || typeof d !== 'object' || typeof d.deviceId !== 'string' ||
      !d.deviceId || d.deviceId.length > 64 ||
      ![d.temperatureC, d.humidityPercent, d.distanceCm].every(validNumberOrNull) ||
      !d.gps || typeof d.gps !== 'object' ||
      typeof d.gps.hasFix !== 'boolean' ||
      ![d.gps.latitude, d.gps.longitude, d.gps.speedKmph].every(validNumberOrNull) ||
      !Number.isInteger(d.gps.charsProcessed) || d.gps.charsProcessed < 0 ||
      (d.gps.hasFix && (d.gps.latitude === null || d.gps.longitude === null ||
       d.gps.latitude < -90 || d.gps.latitude > 90 ||
       d.gps.longitude < -180 || d.gps.longitude > 180))) {
    return res.status(400).json({ error: 'Invalid sensor data' });
  }

  // Optional LDR reading from ESP32-S3 GPIO1 (12-bit ADC).
  // Older ESP32 firmware may omit the field entirely.
  const ldrRawValue = d.ldrRawValue ?? null;
  if (ldrRawValue !== null && (!Number.isInteger(ldrRawValue) ||
      ldrRawValue < 0 || ldrRawValue > 4095)) {
    return res.status(400).json({ error: 'Invalid LDR reading (expected integer 0-4095)' });
  }

  const receivedAt = new Date().toISOString();
  const now = Date.now();
  const row = {
    device_id: d.deviceId,
    temperature_c: d.temperatureC,
    humidity_percent: d.humidityPercent,
    distance_cm: d.distanceCm,
    ldr_raw_value: ldrRawValue,
    gps_has_fix: d.gps.hasFix,
    gps_latitude: d.gps.latitude,
    gps_longitude: d.gps.longitude,
    gps_speed_kmph: d.gps.speedKmph,
    gps_chars_processed: d.gps.charsProcessed,
    received_at: receivedAt
  };

  // Read the newest sample live every 5 sec, but persist history every 30 sec
  // to stay comfortably within the small free database allowance.
  let stored = false;
  if (now - lastStoredMs >= STORE_EVERY_MS) {
    const { error } = await db.from('sensor_readings').insert(row);
    if (error) {
      console.error('Supabase sensor insert error:', error.message);
      return res.status(503).json({ error: 'Database write failed' });
    }
    lastStoredMs = now;
    stored = true;
  }
  latestReading = toReading(row);
  console.log('Sensor data received', row.device_id, row.temperature_c, 'C; persisted:', stored);
  res.json({ success: true, message: 'Sensor data received', historySaved: stored });
}));

app.post('/api/rfid-event', deviceAuth, wrap(async (req, res) => {
  const uid = req.body?.uid;
  if (typeof uid !== 'string' || !/^[0-9A-F]{8,20}$/.test(uid)) {
    return res.status(400).json({ error: 'Invalid RFID UID' });
  }
  const row = { event_type: 'card_scanned', uid, received_at: new Date().toISOString() };
  const { error } = await db.from('rfid_events').insert(row);
  if (error) {
    console.error('Supabase RFID insert error:', error.message);
    return res.status(503).json({ error: 'Database write failed' });
  }
  console.log('RFID scan recorded', uid);
  res.json({ success: true, event: toEvent(row) });
}));

app.get('/api/latest', dashboardAuth, wrap(async (_req, res) => {
  if (latestReading) return res.json(latestReading);
  const { data, error } = await db.from('sensor_readings')
    .select('*').order('received_at', { ascending: false }).limit(1);
  if (error) throw error;
  if (!data?.length) return res.status(404).json({ message: 'No sensor data received yet' });
  res.json(toReading(data[0]));
}));
app.get('/api/history', dashboardAuth, wrap(async (_req, res) => {
  const { data, error } = await db.from('sensor_readings')
    .select('*').order('received_at', { ascending: false }).limit(100);
  if (error) throw error;
  res.json((data || []).reverse().map(toReading));
}));
app.get('/api/rfid-events', dashboardAuth, wrap(async (_req, res) => {
  const { data, error } = await db.from('rfid_events')
    .select('*').order('received_at', { ascending: false }).limit(100);
  if (error) throw error;
  res.json((data || []).reverse().map(toEvent));
}));

app.use((err, _req, res, _next) => {
  if (err?.type === 'entity.parse.failed') return res.status(400).json({ error: 'Malformed JSON' });
  console.error('Server error:', err?.message || err);
  res.status(500).json({ error: 'Internal server/database error' });
});
app.listen(PORT, '0.0.0.0', () => {
  console.log('SmartCold Guard Step 8 running');
  console.log(`Local: http://localhost:${PORT}/dashboard`);
  console.log('Database: Supabase; history saved every 30 seconds');
});
