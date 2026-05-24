/**
 * SmartCane Mobile App
 * ---------------------
 * Monitors phone accelerometer for sudden changes (fall / drop detection).
 * On fall: fetches GPS location and sends an SMS to the configured
 * emergency contact via the Twilio REST API.
 *
 * Fall-detection logic mirrors smartCaneAccelerometer.ino:
 *   magnitude = sqrt(ax^2 + ay^2 + az^2)
 *   if magnitude < FREE_FALL_G_THRESHOLD (0.65 g) → fall detected
 *
 * Setup:
 *   1. npm install  (or expo install)
 *   2. expo start  → scan QR with Expo Go on your phone
 *   3. Open the Settings tab and enter Twilio credentials + contact number
 */

import React, { useState, useEffect, useRef, useCallback } from 'react';
import {
  View,
  Text,
  TextInput,
  TouchableOpacity,
  ScrollView,
  StyleSheet,
  Alert,
  ActivityIndicator,
  Platform,
} from 'react-native';
import { Accelerometer } from 'expo-sensors';
import * as Location from 'expo-location';
import AsyncStorage from '@react-native-async-storage/async-storage';
import { StatusBar } from 'expo-status-bar';

// ─── Fall-detection constants (mirrors smartCaneAccelerometer.ino) ────────────
const FREE_FALL_G_THRESHOLD = 0.65;   // g — same as Arduino constant
const SAMPLE_INTERVAL_MS    = 25;     // 40 Hz — same as Arduino SAMPLE_INTERVAL_MS
const FALL_COOLDOWN_MS      = 10000;  // 10 s cooldown between alerts

// ─── AsyncStorage keys ────────────────────────────────────────────────────────
const KEY_TWILIO_SID    = 'twilio_account_sid';
const KEY_TWILIO_TOKEN  = 'twilio_auth_token';
const KEY_TWILIO_FROM   = 'twilio_from_number';
const KEY_CONTACT_NAME  = 'contact_name';
const KEY_CONTACT_PHONE = 'contact_phone';

// ─── Twilio SMS helper ────────────────────────────────────────────────────────
async function sendTwilioSms(accountSid, authToken, fromNumber, toNumber, body) {
  const url = `https://api.twilio.com/2010-04-01/Accounts/${accountSid}/Messages.json`;

  const params = new URLSearchParams();
  params.append('To',   toNumber);
  params.append('From', fromNumber);
  params.append('Body', body);

  const credentials = btoa(`${accountSid}:${authToken}`);

  const response = await fetch(url, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/x-www-form-urlencoded',
      'Authorization': `Basic ${credentials}`,
    },
    body: params.toString(),
  });

  if (!response.ok) {
    const err = await response.text();
    throw new Error(`Twilio error ${response.status}: ${err}`);
  }

  return await response.json();
}

// ─── Main App ─────────────────────────────────────────────────────────────────
export default function App() {
  const [tab, setTab] = useState('monitor'); // 'monitor' | 'settings'

  // Accelerometer
  const [accelData, setAccelData]     = useState({ x: 0, y: 0, z: 0, magnitude: 1.0 });
  const [monitoring, setMonitoring]   = useState(false);

  // Fall state
  const [fallDetected, setFallDetected]     = useState(false);
  const [lastFallTime, setLastFallTime]     = useState(null);
  const [alertLog, setAlertLog]             = useState([]);

  // GPS
  const [lastLocation, setLastLocation]     = useState(null);

  // SMS sending state
  const [sending, setSending]               = useState(false);
  const [lastSmsStatus, setLastSmsStatus]   = useState(null); // 'ok' | 'error' | null

  // Settings
  const [twilioSid,    setTwilioSid]    = useState('');
  const [twilioToken,  setTwilioToken]  = useState('');
  const [twilioFrom,   setTwilioFrom]   = useState('');
  const [contactName,  setContactName]  = useState('');
  const [contactPhone, setContactPhone] = useState('');
  const [settingsSaved, setSettingsSaved] = useState(false);

  const fallCooldownRef  = useRef(false);
  const subscriptionRef  = useRef(null);

  // ── Load settings on mount ────────────────────────────────────────────────
  useEffect(() => {
    (async () => {
      const [sid, token, from, name, phone] = await Promise.all([
        AsyncStorage.getItem(KEY_TWILIO_SID),
        AsyncStorage.getItem(KEY_TWILIO_TOKEN),
        AsyncStorage.getItem(KEY_TWILIO_FROM),
        AsyncStorage.getItem(KEY_CONTACT_NAME),
        AsyncStorage.getItem(KEY_CONTACT_PHONE),
      ]);
      if (sid)    setTwilioSid(sid);
      if (token)  setTwilioToken(token);
      if (from)   setTwilioFrom(from);
      if (name)   setContactName(name);
      if (phone)  setContactPhone(phone);
    })();
  }, []);

  // ── Save settings ─────────────────────────────────────────────────────────
  const saveSettings = useCallback(async () => {
    await Promise.all([
      AsyncStorage.setItem(KEY_TWILIO_SID,    twilioSid.trim()),
      AsyncStorage.setItem(KEY_TWILIO_TOKEN,  twilioToken.trim()),
      AsyncStorage.setItem(KEY_TWILIO_FROM,   twilioFrom.trim()),
      AsyncStorage.setItem(KEY_CONTACT_NAME,  contactName.trim()),
      AsyncStorage.setItem(KEY_CONTACT_PHONE, contactPhone.trim()),
    ]);
    setSettingsSaved(true);
    setTimeout(() => setSettingsSaved(false), 2000);
  }, [twilioSid, twilioToken, twilioFrom, contactName, contactPhone]);

  // ── Request permissions + start/stop monitoring ───────────────────────────
  const startMonitoring = useCallback(async () => {
    const { status: locStatus } = await Location.requestForegroundPermissionsAsync();
    if (locStatus !== 'granted') {
      Alert.alert('Permission denied', 'Location permission is required to send your position in an emergency.');
      return;
    }

    Accelerometer.setUpdateInterval(SAMPLE_INTERVAL_MS);

    subscriptionRef.current = Accelerometer.addListener(({ x, y, z }) => {
      const magnitude = Math.sqrt(x * x + y * y + z * z);
      setAccelData({ x, y, z, magnitude });

      // Mirror the Arduino free-fall check
      if (magnitude < FREE_FALL_G_THRESHOLD && !fallCooldownRef.current) {
        handleFallDetected();
      }
    });

    setMonitoring(true);
  }, []);

  const stopMonitoring = useCallback(() => {
    if (subscriptionRef.current) {
      subscriptionRef.current.remove();
      subscriptionRef.current = null;
    }
    setMonitoring(false);
  }, []);

  // ── Fall handler ──────────────────────────────────────────────────────────
  const handleFallDetected = useCallback(async () => {
    fallCooldownRef.current = true;
    const now = new Date();
    setFallDetected(true);
    setLastFallTime(now);
    setTimeout(() => {
      setFallDetected(false);
      fallCooldownRef.current = false;
    }, FALL_COOLDOWN_MS);

    // Get GPS
    let coords = null;
    try {
      const loc = await Location.getCurrentPositionAsync({
        accuracy: Location.Accuracy.High,
      });
      coords = loc.coords;
      setLastLocation(coords);
    } catch (e) {
      console.warn('GPS unavailable:', e.message);
    }

    // Build SMS body
    const timeStr = now.toLocaleTimeString();
    let smsBody = `🚨 FALL ALERT from SmartCane at ${timeStr}.`;
    if (coords) {
      const mapsUrl = `https://maps.google.com/?q=${coords.latitude},${coords.longitude}`;
      smsBody += ` Location: ${mapsUrl}`;
      if (coords.accuracy) {
        smsBody += ` (±${Math.round(coords.accuracy)} m accuracy)`;
      }
    } else {
      smsBody += ' GPS location unavailable.';
    }

    // Log the alert locally
    setAlertLog(prev => [
      { id: now.getTime(), time: timeStr, coords, smsStatus: 'sending' },
      ...prev.slice(0, 9),
    ]);

    // Send SMS
    const sid   = (await AsyncStorage.getItem(KEY_TWILIO_SID))    || '';
    const token = (await AsyncStorage.getItem(KEY_TWILIO_TOKEN))  || '';
    const from  = (await AsyncStorage.getItem(KEY_TWILIO_FROM))   || '';
    const to    = (await AsyncStorage.getItem(KEY_CONTACT_PHONE)) || '';

    if (!sid || !token || !from || !to) {
      setLastSmsStatus('error');
      setAlertLog(prev =>
        prev.map(a => a.id === now.getTime() ? { ...a, smsStatus: 'no-config' } : a)
      );
      Alert.alert('Twilio not configured', 'Go to Settings and enter your Twilio credentials and emergency contact number.');
      return;
    }

    setSending(true);
    try {
      await sendTwilioSms(sid, token, from, to, smsBody);
      setLastSmsStatus('ok');
      setAlertLog(prev =>
        prev.map(a => a.id === now.getTime() ? { ...a, smsStatus: 'sent' } : a)
      );
    } catch (e) {
      setLastSmsStatus('error');
      setAlertLog(prev =>
        prev.map(a => a.id === now.getTime() ? { ...a, smsStatus: 'failed' } : a)
      );
      console.error('SMS send failed:', e.message);
    } finally {
      setSending(false);
    }
  }, []);

  // ── Manual test (for demo) ────────────────────────────────────────────────
  const triggerTestAlert = useCallback(() => {
    if (fallCooldownRef.current) {
      Alert.alert('Cooldown active', `Wait ${FALL_COOLDOWN_MS / 1000} s between alerts.`);
      return;
    }
    handleFallDetected();
  }, [handleFallDetected]);

  // ─────────────────────────────────────────────────────────────────────────
  // Render
  // ─────────────────────────────────────────────────────────────────────────
  return (
    <View style={styles.root}>
      <StatusBar style="light" />

      {/* ── Header ── */}
      <View style={styles.header}>
        <Text style={styles.headerTitle}>🦯 SmartCane</Text>
        <Text style={styles.headerSub}>Fall Detection & GPS Alert</Text>
      </View>

      {/* ── Tab bar ── */}
      <View style={styles.tabBar}>
        <TouchableOpacity
          style={[styles.tabBtn, tab === 'monitor' && styles.tabBtnActive]}
          onPress={() => setTab('monitor')}
        >
          <Text style={[styles.tabLabel, tab === 'monitor' && styles.tabLabelActive]}>Monitor</Text>
        </TouchableOpacity>
        <TouchableOpacity
          style={[styles.tabBtn, tab === 'settings' && styles.tabBtnActive]}
          onPress={() => setTab('settings')}
        >
          <Text style={[styles.tabLabel, tab === 'settings' && styles.tabLabelActive]}>Settings</Text>
        </TouchableOpacity>
      </View>

      {/* ── Monitor tab ── */}
      {tab === 'monitor' && (
        <ScrollView contentContainerStyle={styles.content}>

          {/* Fall alert banner */}
          {fallDetected && (
            <View style={styles.fallBanner}>
              <Text style={styles.fallBannerText}>⚠️ FALL DETECTED</Text>
              {sending && <ActivityIndicator color="#fff" style={{ marginTop: 4 }} />}
              {!sending && lastSmsStatus === 'ok'    && <Text style={styles.fallBannerSub}>✅ SMS sent to emergency contact</Text>}
              {!sending && lastSmsStatus === 'error' && <Text style={styles.fallBannerSub}>❌ SMS failed — check Settings</Text>}
            </View>
          )}

          {/* Accelerometer card */}
          <View style={styles.card}>
            <Text style={styles.cardTitle}>Accelerometer</Text>
            <View style={styles.accelRow}>
              <Text style={styles.accelLabel}>X</Text>
              <Text style={styles.accelValue}>{accelData.x.toFixed(3)} g</Text>
              <Text style={styles.accelLabel}>Y</Text>
              <Text style={styles.accelValue}>{accelData.y.toFixed(3)} g</Text>
              <Text style={styles.accelLabel}>Z</Text>
              <Text style={styles.accelValue}>{accelData.z.toFixed(3)} g</Text>
            </View>
            <View style={styles.magnitudeRow}>
              <Text style={styles.magnitudeLabel}>Magnitude</Text>
              <Text style={[
                styles.magnitudeValue,
                accelData.magnitude < FREE_FALL_G_THRESHOLD && styles.magnitudeDanger,
              ]}>
                {accelData.magnitude.toFixed(3)} g
              </Text>
            </View>
            <View style={styles.thresholdRow}>
              <Text style={styles.thresholdText}>
                Fall threshold: &lt; {FREE_FALL_G_THRESHOLD} g
              </Text>
            </View>
          </View>

          {/* GPS card */}
          <View style={styles.card}>
            <Text style={styles.cardTitle}>Last Known Location</Text>
            {lastLocation ? (
              <>
                <Text style={styles.gpsText}>Lat: {lastLocation.latitude.toFixed(6)}</Text>
                <Text style={styles.gpsText}>Lon: {lastLocation.longitude.toFixed(6)}</Text>
                {lastLocation.accuracy != null && (
                  <Text style={styles.gpsText}>Accuracy: ±{Math.round(lastLocation.accuracy)} m</Text>
                )}
              </>
            ) : (
              <Text style={styles.gpsMuted}>Not yet fetched (will update on next fall)</Text>
            )}
          </View>

          {/* Controls */}
          <TouchableOpacity
            style={[styles.primaryBtn, monitoring && styles.primaryBtnActive]}
            onPress={monitoring ? stopMonitoring : startMonitoring}
          >
            <Text style={styles.primaryBtnText}>
              {monitoring ? '⏹ Stop Monitoring' : '▶ Start Monitoring'}
            </Text>
          </TouchableOpacity>

          <TouchableOpacity
            style={styles.secondaryBtn}
            onPress={triggerTestAlert}
          >
            <Text style={styles.secondaryBtnText}>🧪 Simulate Fall (Test SMS)</Text>
          </TouchableOpacity>

          {/* Alert log */}
          {alertLog.length > 0 && (
            <View style={styles.card}>
              <Text style={styles.cardTitle}>Alert History</Text>
              {alertLog.map(a => (
                <View key={a.id} style={styles.logRow}>
                  <Text style={styles.logTime}>{a.time}</Text>
                  <Text style={[
                    styles.logStatus,
                    a.smsStatus === 'sent'      && styles.logOk,
                    a.smsStatus === 'failed'    && styles.logFail,
                    a.smsStatus === 'no-config' && styles.logFail,
                    a.smsStatus === 'sending'   && styles.logSending,
                  ]}>
                    {a.smsStatus === 'sent'      ? '✅ SMS sent' :
                     a.smsStatus === 'failed'    ? '❌ SMS failed' :
                     a.smsStatus === 'no-config' ? '⚠️ Not configured' :
                                                   '⏳ Sending…'}
                  </Text>
                  {a.coords && (
                    <Text style={styles.logCoords}>
                      {a.coords.latitude.toFixed(4)}, {a.coords.longitude.toFixed(4)}
                    </Text>
                  )}
                </View>
              ))}
            </View>
          )}
        </ScrollView>
      )}

      {/* ── Settings tab ── */}
      {tab === 'settings' && (
        <ScrollView contentContainerStyle={styles.content}>
          <Text style={styles.sectionTitle}>Emergency Contact</Text>

          <Text style={styles.inputLabel}>Contact Name</Text>
          <TextInput
            style={styles.input}
            value={contactName}
            onChangeText={setContactName}
            placeholder="e.g. Dad"
            placeholderTextColor="#888"
          />

          <Text style={styles.inputLabel}>Contact Phone (E.164 format)</Text>
          <TextInput
            style={styles.input}
            value={contactPhone}
            onChangeText={setContactPhone}
            placeholder="+1234567890"
            placeholderTextColor="#888"
            keyboardType="phone-pad"
          />

          <Text style={styles.sectionTitle}>Twilio Credentials</Text>
          <Text style={styles.settingsNote}>
            Sign up free at twilio.com. Get an Account SID, Auth Token, and a
            Twilio phone number. The first verified number gets $15 trial credit
            (enough for hundreds of SMS).
          </Text>

          <Text style={styles.inputLabel}>Account SID</Text>
          <TextInput
            style={styles.input}
            value={twilioSid}
            onChangeText={setTwilioSid}
            placeholder="ACxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
            placeholderTextColor="#888"
            autoCapitalize="none"
          />

          <Text style={styles.inputLabel}>Auth Token</Text>
          <TextInput
            style={styles.input}
            value={twilioToken}
            onChangeText={setTwilioToken}
            placeholder="your_auth_token"
            placeholderTextColor="#888"
            secureTextEntry
            autoCapitalize="none"
          />

          <Text style={styles.inputLabel}>Twilio Phone Number (E.164 format)</Text>
          <TextInput
            style={styles.input}
            value={twilioFrom}
            onChangeText={setTwilioFrom}
            placeholder="+15551234567"
            placeholderTextColor="#888"
            keyboardType="phone-pad"
          />

          <TouchableOpacity style={styles.primaryBtn} onPress={saveSettings}>
            <Text style={styles.primaryBtnText}>
              {settingsSaved ? '✅ Saved!' : 'Save Settings'}
            </Text>
          </TouchableOpacity>

          <View style={styles.infoBox}>
            <Text style={styles.infoTitle}>How SMS is sent</Text>
            <Text style={styles.infoText}>
              When a fall is detected (accelerometer magnitude drops below{' '}
              {FREE_FALL_G_THRESHOLD} g), the app:{'\n'}
              1. Gets your GPS coordinates{'\n'}
              2. Builds a Google Maps link{'\n'}
              3. Calls the Twilio REST API to send an SMS to your contact{'\n\n'}
              No backend server is required — the app calls Twilio directly.
            </Text>
          </View>
        </ScrollView>
      )}
    </View>
  );
}

// ─── Styles ───────────────────────────────────────────────────────────────────
const ACCENT  = '#4f8ef7';
const DANGER  = '#e74c3c';
const SUCCESS = '#2ecc71';
const BG      = '#0f0f1a';
const CARD    = '#1a1a2e';
const TEXT    = '#e8e8f0';
const MUTED   = '#888';

const styles = StyleSheet.create({
  root: { flex: 1, backgroundColor: BG },

  header: {
    backgroundColor: CARD,
    paddingTop: Platform.OS === 'ios' ? 54 : 40,
    paddingBottom: 14,
    paddingHorizontal: 20,
    borderBottomWidth: 1,
    borderBottomColor: '#2a2a3e',
  },
  headerTitle: { color: TEXT, fontSize: 22, fontWeight: '700' },
  headerSub:   { color: MUTED, fontSize: 12, marginTop: 2 },

  tabBar: {
    flexDirection: 'row',
    backgroundColor: CARD,
    borderBottomWidth: 1,
    borderBottomColor: '#2a2a3e',
  },
  tabBtn:       { flex: 1, paddingVertical: 12, alignItems: 'center' },
  tabBtnActive: { borderBottomWidth: 2, borderBottomColor: ACCENT },
  tabLabel:       { color: MUTED, fontSize: 14 },
  tabLabelActive: { color: ACCENT, fontWeight: '600' },

  content: { padding: 16, paddingBottom: 40 },

  fallBanner: {
    backgroundColor: DANGER,
    borderRadius: 10,
    padding: 16,
    marginBottom: 14,
    alignItems: 'center',
  },
  fallBannerText: { color: '#fff', fontSize: 20, fontWeight: '800' },
  fallBannerSub:  { color: '#ffd', fontSize: 13, marginTop: 6 },

  card: {
    backgroundColor: CARD,
    borderRadius: 10,
    padding: 14,
    marginBottom: 14,
  },
  cardTitle: { color: TEXT, fontSize: 14, fontWeight: '600', marginBottom: 10 },

  accelRow: { flexDirection: 'row', justifyContent: 'space-between', marginBottom: 8 },
  accelLabel: { color: MUTED, fontSize: 12, marginRight: 2 },
  accelValue: { color: TEXT, fontSize: 14, fontWeight: '500', marginRight: 12 },

  magnitudeRow:  { flexDirection: 'row', alignItems: 'center', marginBottom: 6 },
  magnitudeLabel: { color: MUTED, fontSize: 13, marginRight: 8 },
  magnitudeValue: { color: TEXT, fontSize: 18, fontWeight: '700' },
  magnitudeDanger:{ color: DANGER },

  thresholdRow: { marginTop: 2 },
  thresholdText: { color: MUTED, fontSize: 11 },

  gpsText: { color: TEXT, fontSize: 13, marginBottom: 2 },
  gpsMuted: { color: MUTED, fontSize: 13, fontStyle: 'italic' },

  primaryBtn: {
    backgroundColor: ACCENT,
    borderRadius: 10,
    paddingVertical: 14,
    alignItems: 'center',
    marginBottom: 10,
  },
  primaryBtnActive: { backgroundColor: '#c0392b' },
  primaryBtnText:   { color: '#fff', fontSize: 16, fontWeight: '700' },

  secondaryBtn: {
    borderColor: ACCENT,
    borderWidth: 1,
    borderRadius: 10,
    paddingVertical: 12,
    alignItems: 'center',
    marginBottom: 14,
  },
  secondaryBtnText: { color: ACCENT, fontSize: 14, fontWeight: '600' },

  logRow: {
    borderTopWidth: 1,
    borderTopColor: '#2a2a3e',
    paddingTop: 8,
    marginTop: 8,
  },
  logTime:    { color: MUTED, fontSize: 11, marginBottom: 2 },
  logStatus:  { fontSize: 13, fontWeight: '600', color: TEXT },
  logOk:      { color: SUCCESS },
  logFail:    { color: DANGER },
  logSending: { color: ACCENT },
  logCoords:  { color: MUTED, fontSize: 11, marginTop: 2 },

  sectionTitle: { color: TEXT, fontSize: 16, fontWeight: '700', marginTop: 10, marginBottom: 10 },
  inputLabel:   { color: MUTED, fontSize: 12, marginBottom: 4 },
  input: {
    backgroundColor: CARD,
    color: TEXT,
    borderRadius: 8,
    padding: 12,
    fontSize: 14,
    marginBottom: 12,
    borderWidth: 1,
    borderColor: '#2a2a3e',
  },

  settingsNote: {
    color: MUTED,
    fontSize: 12,
    marginBottom: 12,
    lineHeight: 18,
  },

  infoBox: {
    backgroundColor: '#1a2a1a',
    borderRadius: 10,
    padding: 14,
    marginTop: 8,
    borderLeftWidth: 3,
    borderLeftColor: SUCCESS,
  },
  infoTitle: { color: SUCCESS, fontWeight: '700', marginBottom: 6 },
  infoText:  { color: MUTED, fontSize: 12, lineHeight: 18 },
});
