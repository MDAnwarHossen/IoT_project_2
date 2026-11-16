'use strict';

const DEVICE_ID_PATTERN = /^[A-Za-z0-9_-]{1,64}$/;
const RFID_UID_PATTERN = /^[0-9A-F]{8,20}$/;
const ALARM_CODES = new Set(['high_temp', 'unauthorized_open']);
const KINDS = new Set(['sensor', 'rfid', 'door', 'alarm']);
const numOrNull = x => x === null || (typeof x === 'number' && Number.isFinite(x));
const bool = x => typeof x === 'boolean';

function validateRecord(r) {
  if (!r || typeof r !== 'object' || Array.isArray(r)) return 'Expected object';
  if (!DEVICE_ID_PATTERN.test(r.deviceId || '')) return 'Invalid deviceId';
  if (!Number.isInteger(r.sequence) || r.sequence < 1 || r.sequence > 4294967295) return 'Invalid sequence';
  if (r.id !== `${r.deviceId}-${String(r.sequence).padStart(10,'0')}`) return 'Invalid event id';
  if (!KINDS.has(r.kind)) return 'Invalid record kind';
  if (r.occurredAt !== null && r.occurredAt !== undefined &&
      (typeof r.occurredAt !== 'string' || !/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/.test(r.occurredAt) ||
       !Number.isFinite(Date.parse(r.occurredAt)))) return 'Invalid timestamp';
  if (!Number.isInteger(r.uptimeMs) || r.uptimeMs < 0) return 'Invalid uptime';
  for (const key of ['temperatureC','humidityPercent','distanceCm']) {
    if (!numOrNull(r[key])) return `Invalid ${key}`;
  }
  if (!Number.isInteger(r.ldrRawValue) || r.ldrRawValue < 0 || r.ldrRawValue > 4095) return 'Invalid LDR';
  for (const key of ['doorOpen', 'doorKnown', 'accessGranted', 'temperatureAlarm', 'securityAlarm', 'buzzerActive', 'storageFault']) {
    if (!bool(r[key])) return `Invalid ${key}`;
  }
  if (!r.gps || !bool(r.gps.hasFix) || !numOrNull(r.gps.latitude) ||
      !numOrNull(r.gps.longitude) || !numOrNull(r.gps.speedKmph) ||
      !Number.isInteger(r.gps.charsProcessed) || r.gps.charsProcessed < 0) return 'Invalid GPS';
  if (r.gps.hasFix && (!Number.isFinite(r.gps.latitude) || !Number.isFinite(r.gps.longitude) ||
      Math.abs(r.gps.latitude) > 90 || Math.abs(r.gps.longitude) > 180)) return 'Invalid GPS fix';
  if (!r.gps.hasFix && (r.gps.latitude !== null || r.gps.longitude !== null)) return 'GPS no-fix must use null coordinates';
  if (r.kind === 'rfid' && (!RFID_UID_PATTERN.test(r.uid || '') ||
       !['authorized','unauthorized'].includes(r.authorization))) return 'Invalid RFID event';
  if (r.kind === 'alarm' && (!ALARM_CODES.has(r.code) || !bool(r.active))) return 'Invalid alarm event';
  if (r.kind === 'door' && !['opened','closed'].includes(r.code)) return 'Invalid door event';
  return null;
}
function notificationCode(record) {
  return record.kind === 'alarm' && record.active === true && ALARM_CODES.has(record.code) ? record.code : null;
}
function parseLegacyReading(r) {
  return {
    deviceId:r.device_id, temperatureC:r.temperature_c, humidityPercent:r.humidity_percent,
    distanceCm:r.distance_cm, ldrRawValue:r.ldr_raw_value ?? null,
    gps:{hasFix:!!r.gps_has_fix,latitude:r.gps_latitude,longitude:r.gps_longitude,
      speedKmph:r.gps_speed_kmph,charsProcessed:r.gps_chars_processed},
    receivedAt:r.received_at, occurredAt:r.received_at,
    temperatureAlarm: typeof r.temperature_c === 'number' ? r.temperature_c > 23.5 : null,
    securityAlarm: null, buzzerActive:null, doorOpen:null, legacy:true
  };
}
function recordForUI(row) {
  const p=row.payload;
  return {...p, receivedAt:row.received_at, occurredAt:p.occurredAt || null,
          id:p.id, legacy:false};
}
module.exports = {validateRecord, notificationCode, parseLegacyReading, recordForUI};
