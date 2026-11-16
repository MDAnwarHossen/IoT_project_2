'use strict';
/* SmartCold Guard FINAL — Render + Supabase + optional Blynk HTTPS API.
   Every /api/record HTTP 200 is sent AFTER durable Supabase acceptance.
   Older /api/sensor-data and /api/rfid-event routes remain during migration. */
require('dotenv').config();
const express = require('express');
const path = require('path');
const crypto = require('crypto');
const { createClient } = require('@supabase/supabase-js');
const {validateRecord, notificationCode, parseLegacyReading, recordForUI} = require('./rules');

const app = express();
const PORT = Number(process.env.PORT || 3000);
const DEVICE_ID = process.env.DEVICE_ID || 'esp32-s3-01';
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || '';
const DASHBOARD_USERNAME = process.env.DASHBOARD_USERNAME || 'demo';
const DASHBOARD_PASSWORD = process.env.DASHBOARD_PASSWORD || '';
const BLYNK_AUTH_TOKEN = process.env.BLYNK_AUTH_TOKEN || '';
const BLYNK_HOST = process.env.BLYNK_HOST || 'blynk.cloud';
const BLYNK_UPDATE_INTERVAL_MS = Math.max(5000,Number(process.env.BLYNK_UPDATE_INTERVAL_MS || 15000));
const SUPABASE_URL = process.env.SUPABASE_URL;
const SUPABASE_SECRET_KEY = process.env.SUPABASE_SECRET_KEY;
const LEGACY_STORE_MS = 30000;
if (!SUPABASE_URL || !SUPABASE_SECRET_KEY) throw Error('Missing Supabase environment variables');
if (process.env.NODE_ENV === 'production' && (!DEVICE_TOKEN || !DASHBOARD_PASSWORD)) {
  throw Error('DEVICE_TOKEN and DASHBOARD_PASSWORD required in production');
}
if (!/^[a-z0-9.-]+$/i.test(BLYNK_HOST) || !BLYNK_HOST.endsWith('blynk.cloud')) {
  throw Error('BLYNK_HOST must be a blynk.cloud hostname');
}
const db = createClient(SUPABASE_URL,SUPABASE_SECRET_KEY,
  {auth:{persistSession:false,autoRefreshToken:false}});
app.disable('x-powered-by');
app.use(express.json({limit:'20kb'}));
app.use((_req,res,next)=>{res.set('Cache-Control','no-store');next();});
let latestLegacy = null;
let legacyLastStored = 0;
let lastBlynkUpdateAt = 0;
let notificationWorkerBusy = false;
let lastNotificationWorkerAt = 0;

function sameSecret(a,b) {
  const x=Buffer.from(String(a||'')),y=Buffer.from(String(b||''));
  return x.length === y.length && crypto.timingSafeEqual(x,y);
}
function deviceAuth(req,res,next) {
  if (DEVICE_TOKEN && !sameSecret(req.get('X-Device-Token'),DEVICE_TOKEN))
    return res.status(401).json({error:'Invalid device token'});
  next();
}
function dashboardAuth(req,res,next) {
  if (!DASHBOARD_PASSWORD) return next();
  const h=req.get('Authorization')||'';
  let text='';
  try { if(h.startsWith('Basic ')) text=Buffer.from(h.slice(6),'base64').toString('utf8'); }
  catch (_) { /* treated as invalid */ }
  const split=text.indexOf(':');
  if (split<0 || !sameSecret(text.slice(0,split),DASHBOARD_USERNAME) ||
      !sameSecret(text.slice(split+1),DASHBOARD_PASSWORD)) {
    res.set('WWW-Authenticate','Basic realm="SmartCold Guard", charset="UTF-8"');
    return res.status(401).send('Dashboard sign in required');
  }
  next();
}
const wrap = fn => (req,res,next)=>Promise.resolve(fn(req,res)).catch(next);
const errorFrom = (error,context) => {if(error) throw Error(`${context}: ${error.message}`);};

// Keep Blynk device token on Render, never in firmware. Each batch is one
// HTTPS request, reduces monthly message usage. Sensor readings remain in
// Supabase even when Blynk is unavailable.
async function blynkGet(route, params) {
  if (!BLYNK_AUTH_TOKEN) return false;
  const qs=new URLSearchParams({token:BLYNK_AUTH_TOKEN,...params});
  const url=`https://${BLYNK_HOST}/external/api/${route}?${qs}`;
  const response=await fetch(url,{signal:AbortSignal.timeout(6500)});
  if (!response.ok) throw Error(`Blynk ${route}: HTTP ${response.status}`);
  return true;
}
async function updateBlynkSensor(payload) {
  if (!BLYNK_AUTH_TOKEN || Date.now()-lastBlynkUpdateAt < BLYNK_UPDATE_INTERVAL_MS) return;
  lastBlynkUpdateAt=Date.now();
  const pins={};
  const put=(key,val)=>{if(typeof val==='number' && Number.isFinite(val))pins[key]=String(val);};
  put('v0',payload.temperatureC);
  put('v1',payload.humidityPercent);
  put('v2',payload.distanceCm);
  put('v3',payload.ldrRawValue);
  pins.v4=payload.accessGranted ? 'AUTHORIZED' : 'NOT AUTHORIZED';
  pins.v5=payload.temperatureAlarm ? '1' : '0';
  pins.v6=payload.securityAlarm ? '1' : '0';
  pins.v9=payload.buzzerActive ? '1' : '0';
  pins.v10=payload.gps?.hasFix ? '1' : '0';
  if(payload.gps?.hasFix){put('v7',payload.gps.latitude);put('v8',payload.gps.longitude);}
  try { await blynkGet('batch/update',pins); }
  catch(e){console.error(e.message);}
}
// Persisted notification outbox lives in device_records; a missed Blynk call
// is retried on later device uploads. Exactly-once PUSH is not guaranteed:
// a process crash between Blynk accepting and DB marking can repeat it.
async function deliverPendingNotifications() {
  if (!BLYNK_AUTH_TOKEN || notificationWorkerBusy ||
      Date.now()-lastNotificationWorkerAt < 4500) return;
  notificationWorkerBusy=true;
  lastNotificationWorkerAt=Date.now();
  try {
    const {data,error}=await db.from('device_records')
      .select('id,notification_code,payload,device_id')
      .eq('notification_sent',false).not('notification_code','is',null)
      .order('received_at',{ascending:true}).limit(3);
    errorFrom(error,'Pending Blynk notifications');
    for(const row of data||[]) {
      const detail=row.notification_code==='high_temp'
        ? `Temperature above 23.5 C; measured ${row.payload?.temperatureC ?? 'unknown'} C`
        : `Unauthorized door opening. Distance ${row.payload?.distanceCm ?? '?'} cm, LDR ${row.payload?.ldrRawValue ?? '?'}`;
      try {
        await blynkGet('logEvent',{code:row.notification_code,description:detail});
        const {error:markError}=await db.from('device_records')
          .update({notification_sent:true}).eq('id',row.id);
        errorFrom(markError,'Mark notification delivered');
      } catch(e) {
        console.error('Blynk notification pending retry:',e.message);
        break;
      }
    }
  } catch(e){console.error('Blynk queue error:',e.message);}
  finally{notificationWorkerBusy=false;}
}
function afterPersist(record) {
  if(record.kind==='sensor') void updateBlynkSensor(record);
  void deliverPendingNotifications();
}

app.get('/',(_req,res)=>res.send('SmartCold Guard running — /dashboard'));
app.get('/api/status',(_req,res)=>res.json({project:'SmartCold Guard',server:'online',
  database:'configured', blynkConfigured:!!BLYNK_AUTH_TOKEN}));
app.get('/dashboard',dashboardAuth,(_req,res)=>
  res.sendFile(path.join(__dirname,'dashboard.html')));

// NEW durable, idempotent endpoint. Duplication across retries returns ACK,
// but conflicting payloads with the same ID return 409, not a false ACK.
app.post('/api/record',deviceAuth,wrap(async(req,res)=>{
  const r=req.body;
  const validationError=validateRecord(r);
  if(validationError) return res.status(400).json({error:validationError});
  if(r.deviceId!==DEVICE_ID) return res.status(403).json({error:'Wrong device ID'});
  const digest=crypto.createHash('sha256').update(JSON.stringify(r)).digest('hex');
  const row={device_id:r.deviceId,event_id:r.id,sequence:r.sequence,
    record_type:r.kind,occurred_at:r.occurredAt||null,
    payload:r,payload_digest:digest,
    notification_code:notificationCode(r),notification_sent:false};
  const {error}=await db.from('device_records').insert(row);
  if (error) {
    if (error.code!=='23505') {
      console.error('Supabase final insert failed:',error.message);
      return res.status(503).json({error:'Database not ready; retry'});
    }
    const {data:existing,error:lookupError}=await db.from('device_records')
      .select('payload_digest').eq('device_id',r.deviceId)
      .eq('event_id',r.id).maybeSingle();
    if(lookupError) return res.status(503).json({error:'Duplicate lookup failed; retry'});
    if(!existing || existing.payload_digest!==digest)
      return res.status(409).json({error:'Event id conflicts with a different record'});
    void deliverPendingNotifications();
    return res.json({success:true,ackId:r.id,duplicate:true});
  }
  res.json({success:true,ackId:r.id,duplicate:false});
  afterPersist(r);
}));

// OLD routes remain so the existing ESP sketch can keep reporting while you
// deploy server and SQL ahead of the final firmware.
const optionalNumber=v=>v===null||(typeof v==='number'&&Number.isFinite(v));
app.post('/api/sensor-data',deviceAuth,wrap(async(req,res)=>{
  const r=req.body;
  if(!r||typeof r.deviceId!=='string'||!r.deviceId||
     ![r.temperatureC,r.humidityPercent,r.distanceCm].every(optionalNumber)||
     !r.gps||typeof r.gps.hasFix!=='boolean'||
     ![r.gps.latitude,r.gps.longitude,r.gps.speedKmph].every(optionalNumber)||
     !Number.isInteger(r.gps.charsProcessed)) return res.status(400).json({error:'Invalid reading'});
  const ldr=r.ldrRawValue??null;
  if(ldr!==null&&(!Number.isInteger(ldr)||ldr<0||ldr>4095))return res.status(400).json({error:'Invalid LDR'});
  const row={device_id:r.deviceId,temperature_c:r.temperatureC,humidity_percent:r.humidityPercent,
    distance_cm:r.distanceCm,ldr_raw_value:ldr,
    gps_has_fix:r.gps.hasFix,gps_latitude:r.gps.latitude,gps_longitude:r.gps.longitude,
    gps_speed_kmph:r.gps.speedKmph,gps_chars_processed:r.gps.charsProcessed,
    received_at:new Date().toISOString()};
  if(Date.now()-legacyLastStored>=LEGACY_STORE_MS){
    const {error}=await db.from('sensor_readings').insert(row);
    if(error)return res.status(503).json({error:'Legacy database write failed'});
    legacyLastStored=Date.now();
  }
  latestLegacy=parseLegacyReading(row);
  res.json({success:true});
}));
app.post('/api/rfid-event',deviceAuth,wrap(async(req,res)=>{
  const uid=req.body?.uid;
  if(typeof uid!=='string'||!/^[0-9A-F]{8,20}$/.test(uid))return res.status(400).json({error:'Invalid UID'});
  const {error}=await db.from('rfid_events').insert({uid,event_type:'card_scanned',received_at:new Date().toISOString()});
  if(error)return res.status(503).json({error:'Legacy RFID database write failed'});
  res.json({success:true});
}));

app.get('/api/latest',dashboardAuth,wrap(async(_req,res)=>{
  const {data,error}=await db.from('device_records').select('*')
    .eq('device_id',DEVICE_ID).eq('record_type','sensor')
    .order('sequence',{ascending:false}).limit(1);
  errorFrom(error,'Latest reading');
  if(data?.length)return res.json(recordForUI(data[0]));
  if(latestLegacy)return res.json(latestLegacy);
  const {data:legacy,error:oldError}=await db.from('sensor_readings')
    .select('*').order('received_at',{ascending:false}).limit(1);
  errorFrom(oldError,'Legacy latest');
  if(!legacy?.length)return res.status(404).json({error:'No readings yet'});
  res.json(parseLegacyReading(legacy[0]));
}));
app.get('/api/history',dashboardAuth,wrap(async(req,res)=>{
  const limit=Math.min(500,Math.max(1,parseInt(req.query.limit||'200',10)||200));
  const [{data,error},{data:old,error:oldError}]=await Promise.all([
    db.from('device_records').select('*').eq('device_id',DEVICE_ID).eq('record_type','sensor')
      .order('sequence',{ascending:false}).limit(limit),
    db.from('sensor_readings').select('*').order('received_at',{ascending:false}).limit(Math.min(limit,100))
  ]);
  errorFrom(error,'History');errorFrom(oldError,'Legacy history');
  const all=[...(data||[]).map(recordForUI),...(old||[]).map(parseLegacyReading)];
  all.sort((a,b)=>new Date(a.occurredAt||a.receivedAt)-new Date(b.occurredAt||b.receivedAt));
  res.json(all.slice(-limit));
}));
app.get('/api/rfid-events',dashboardAuth,wrap(async(req,res)=>{
  const limit=Math.min(250,Math.max(1,parseInt(req.query.limit||'100',10)||100));
  const [{data,error},{data:old,error:oldError}]=await Promise.all([
    db.from('device_records').select('*').eq('device_id',DEVICE_ID)
      .eq('record_type','rfid').order('sequence',{ascending:false}).limit(limit),
    db.from('rfid_events').select('*').order('received_at',{ascending:false}).limit(Math.min(100,limit))
  ]);
  errorFrom(error,'RFID events');errorFrom(oldError,'Legacy RFID events');
  const records=(data||[]).map(row=>{
    const p=row.payload;
    return {id:p.id,uid:p.uid,authorization:p.authorization,
      latitude:p.gps?.latitude??null,longitude:p.gps?.longitude??null,
      occurredAt:p.occurredAt,receivedAt:row.received_at,legacy:false};
  });
  const historic=(old||[]).map(r=>({id:String(r.id),uid:r.uid,authorization:null,
    latitude:null,longitude:null,occurredAt:r.received_at,receivedAt:r.received_at,legacy:true}));
  res.json([...records,...historic].sort((a,b)=>new Date(b.receivedAt)-new Date(a.receivedAt)).slice(0,limit));
}));
app.get('/api/events',dashboardAuth,wrap(async(req,res)=>{
  const limit=Math.min(200,Math.max(1,parseInt(req.query.limit||'80',10)||80));
  const {data,error}=await db.from('device_records').select('*').eq('device_id',DEVICE_ID)
    .in('record_type',['alarm','door','rfid']).order('sequence',{ascending:false}).limit(limit);
  errorFrom(error,'Event history');
  res.json((data||[]).map(row=>({kind:row.record_type,code:row.payload.code||null,
    active:row.payload.active??null,uid:row.payload.uid||null,
    authorization:row.payload.authorization||null,occurredAt:row.payload.occurredAt,
    receivedAt:row.received_at,latitude:row.payload.gps?.latitude??null,
    longitude:row.payload.gps?.longitude??null}))); 
}));
app.use((err,_req,res,_next)=>{
  if(err?.type==='entity.parse.failed')return res.status(400).json({error:'Invalid JSON'});
  console.error('Server error:',err?.message||err);
  return res.status(500).json({error:'Internal server error'});
});
app.listen(PORT,'0.0.0.0',()=>{
  console.log(`SmartCold Guard FINAL on port ${PORT}; Supabase enabled; Blynk ${BLYNK_AUTH_TOKEN?'enabled':'not configured'}`);
});
