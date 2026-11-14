/**
 * Li-Fi Transmitter v2
 * Manchester coded, self-clocking protocol with CRC (matches arduino_receiver.ino)
 *
 *  HEADER : light ON for 8T            (receiver measures T = speed from this)
 *  GAP    : light OFF for 2T
 *  BYTES  : 0xA5 (sync) | LEN | payload (UTF-8) | CRC8(LEN + payload)
 *  bit 1  = ON then OFF  (falling edge mid-bit)
 *  bit 0  = OFF then ON  (rising  edge mid-bit)
 *  T = half-bit time (ms). One byte = 16 slots = 16 * T.
 */

const SYNC_BYTE = 0xA5;
const HEADER_SLOTS = 8;
const GAP_SLOTS = 2;
const MAX_PAYLOAD = 64;
const LEAD_IN_MS = 600;     // darkness before the header
const COUNTDOWN_S = 3;      // time to place the phone on the sensor

/* ---------------- Protocol helpers (pure functions) ---------------- */

function crc8(bytes) {
  let crc = 0;
  for (const b of bytes) {
    crc ^= b;
    for (let i = 0; i < 8; i++) {
      crc = (crc & 0x80) ? (((crc << 1) ^ 0x07) & 0xFF) : ((crc << 1) & 0xFF);
    }
  }
  return crc;
}

function buildFrame(payload) {
  const body = [payload.length, ...payload];
  return [SYNC_BYTE, ...body, crc8(body)];
}

/** Returns an array of 0/1 "slots", each lasting T ms. */
function buildSlots(payload) {
  const slots = [];
  for (let i = 0; i < HEADER_SLOTS; i++) slots.push(1);
  for (let i = 0; i < GAP_SLOTS; i++) slots.push(0);
  for (const byte of buildFrame(payload)) {
    for (let b = 7; b >= 0; b--) {
      if ((byte >> b) & 1) slots.push(1, 0);   // bit 1 = ON, OFF
      else slots.push(0, 1);                   // bit 0 = OFF, ON
    }
  }
  slots.push(0, 0);
  return slots;
}

if (typeof module !== 'undefined') {
  module.exports = { crc8, buildFrame, buildSlots };
}

/* ---------------- Browser part ---------------- */

if (typeof document !== 'undefined') {

let isTransmitting = false;
let cancelRequested = false;
let mediaStream = null;
let videoTrack = null;
let torchSupported = false;
let torchQueue = Promise.resolve();
let torchFailed = false;
let torchLat = [];
let wakeLock = null;
let currentMode = 'torch';

// Elements
const msgInput = document.getElementById('message-input');
const transmitBtn = document.getElementById('transmit-btn');
const stopBtn = document.getElementById('stop-btn');
const speedSelect = document.getElementById('speed-select');
const modeSelect = document.getElementById('mode-select');

const statChars = document.getElementById('stat-chars');
const statBits = document.getElementById('stat-bits');
const statTime = document.getElementById('stat-packets');

const statusDot = document.getElementById('status-dot');
const statusText = document.getElementById('status-text');
const signalLamp = document.getElementById('signal-lamp');

const progressWrap = document.getElementById('progress-wrap');
const progressBar = document.getElementById('progress-bar');
const bitstreamInfo = document.getElementById('bitstream-info');
const screenFlasher = document.getElementById('screen-flasher');

const encoder = new TextEncoder();

window.addEventListener('DOMContentLoaded', () => {
  setupMetricsCounter();
  setupNavModals();
  setupTransmitter();
});

function getT() {
  return parseInt(speedSelect.value, 10) || 150;
}

function setupMetricsCounter() {
  const updateStats = () => {
    // Keep message within the protocol limit (bytes, not characters)
    let text = msgInput.value.replace(/[\r\n]+/g, ' ');
    while (encoder.encode(text).length > MAX_PAYLOAD) text = text.slice(0, -1);
    if (text !== msgInput.value) msgInput.value = text;

    const bytes = encoder.encode(text).length;
    const slots = bytes > 0 ? buildSlots(Array.from(encoder.encode(text))).length : 0;
    statChars.innerText = text.length;
    statBits.innerText = bytes > 0 ? (bytes + 3) * 8 : 0;
    statTime.innerText = bytes > 0 ? Math.ceil((slots * getT() + LEAD_IN_MS) / 1000) + 's' : '0s';
  };

  msgInput.addEventListener('input', updateStats);
  speedSelect.addEventListener('change', updateStats);
  updateStats();
}

/* ---------- Light output ---------- */

async function setupCamera() {
  if (videoTrack && videoTrack.readyState === 'live') return;
  torchSupported = false;
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) return;
  try {
    mediaStream = await navigator.mediaDevices.getUserMedia({
      video: { facingMode: { ideal: 'environment' } }
    });
    videoTrack = mediaStream.getVideoTracks()[0];
    const caps = videoTrack.getCapabilities ? videoTrack.getCapabilities() : {};
    torchSupported = ('torch' in caps);
    if (!torchSupported) releaseCamera();
  } catch (e) {
    videoTrack = null;
  }
}

function releaseCamera() {
  if (mediaStream) mediaStream.getTracks().forEach(t => t.stop());
  mediaStream = null;
  videoTrack = null;
}

function applyTorch(isOn) {
  if (!videoTrack) return;
  // Calls are chained so the ON/OFF commands can never arrive out of order.
  const issued = performance.now();
  torchQueue = torchQueue
    .then(() => videoTrack && videoTrack.applyConstraints({ advanced: [{ torch: isOn }] }))
    .then(() => { torchLat.push(performance.now() - issued); })
    .catch(() => { torchFailed = true; });
}

function setOpticalState(isOn) {
  signalLamp.classList.toggle('active-light', isOn);
  if (currentMode === 'torch') {
    applyTorch(isOn);
  } else {
    screenFlasher.style.background = isOn ? '#ffffff' : '#000000';
  }
}

async function enterScreenMode() {
  screenFlasher.style.background = '#000000';
  screenFlasher.classList.add('active');
  try {
    if (screenFlasher.requestFullscreen) await screenFlasher.requestFullscreen();
  } catch (e) {}
}

function leaveScreenMode() {
  screenFlasher.classList.remove('active');
  if (document.fullscreenElement && document.exitFullscreen) {
    document.exitFullscreen().catch(() => {});
  }
}

async function keepAwake() {
  try {
    if (navigator.wakeLock) wakeLock = await navigator.wakeLock.request('screen');
  } catch (e) {}
}

/* ---------- Precise timing ---------- */

const sleep = ms => new Promise(r => setTimeout(r, ms));

/** Wait until performance.now() >= target (setTimeout, then a short spin for accuracy). */
async function waitUntil(target) {
  for (;;) {
    const d = target - performance.now();
    if (d <= 0) return;
    if (d > 10) await sleep(d - 8);
    else { while (performance.now() < target) {} return; }
  }
}

/* ---------- Transmission ---------- */

function setupTransmitter() {
  transmitBtn.addEventListener('click', startTransmission);
  stopBtn.addEventListener('click', () => { cancelRequested = true; });

  document.getElementById('screen-flasher-exit').addEventListener('click', () => {
    cancelRequested = true;
    leaveScreenMode();
  });

  document.addEventListener('visibilitychange', () => {
    if (document.hidden && isTransmitting) cancelRequested = true;   // browsers freeze timers in the background
  });
}

async function startTransmission() {
  if (isTransmitting) return;
  const text = msgInput.value.replace(/[\r\n]+/g, ' ').trim();
  if (!text) {
    alert("Please enter a message to transmit!");
    msgInput.focus();
    return;
  }
  const payload = Array.from(encoder.encode(text));
  if (payload.length > MAX_PAYLOAD) {
    alert(`Message too long (max ${MAX_PAYLOAD} bytes).`);
    return;
  }

  isTransmitting = true;
  cancelRequested = false;
  torchFailed = false;
  torchLat = [];
  transmitBtn.style.display = 'none';
  stopBtn.style.display = 'flex';
  progressWrap.classList.add('active');
  progressBar.style.width = '0%';
  statusDot.classList.add('pulsing');

  try {
    // Decide output mode
    const wanted = modeSelect.value;
    if (wanted !== 'screen') await setupCamera();
    currentMode = (wanted === 'screen' || !torchSupported) ? 'screen' : 'torch';

    if (wanted === 'torch' && !torchSupported) {
      bitstreamInfo.innerText = "Flashlight not available on this device/browser - using screen flash instead.";
    }

    await keepAwake();
    if (currentMode === 'screen') await enterScreenMode();

    // Countdown so the phone can be placed on the LDR
    for (let s = COUNTDOWN_S; s > 0; s--) {
      if (cancelRequested) break;
      statusText.innerText = `Place phone on sensor... starting in ${s}`;
      await sleep(1000);
    }

    if (!cancelRequested) await transmitMessage(text, payload);
  } catch (err) {
    console.error("Transmission error:", err);
    statusText.innerText = "Error: " + (err && err.message ? err.message : err);
  } finally {
    isTransmitting = false;
    transmitBtn.style.display = 'flex';
    stopBtn.style.display = 'none';
    statusDot.classList.remove('pulsing');
    setOpticalState(false);
    await torchQueue;
    leaveScreenMode();
    releaseCamera();
    if (wakeLock) { try { await wakeLock.release(); } catch (e) {} wakeLock = null; }
  }
}

async function transmitMessage(text, payload) {
  const T = getT();
  const slots = buildSlots(payload);
  const totalMs = slots.length * T;

  statusText.innerText = `Transmitting "${text}"`;
  bitstreamInfo.innerHTML = `Sending ${payload.length} byte(s) at ${T}ms/slot ` +
    `(${currentMode === 'torch' ? 'flashlight' : 'screen'}). Keep the phone steady and close to the sensor!`;

  setOpticalState(false);
  const start = performance.now() + LEAD_IN_MS;
  let prev = 0;
  let lastPct = -1;

  for (let i = 0; i < slots.length; i++) {
    await waitUntil(start + i * T);
    if (cancelRequested) {
      statusText.innerText = "Transmission cancelled";
      return;
    }
    if (slots[i] !== prev) {
      setOpticalState(slots[i] === 1);
      prev = slots[i];
    }
    const pct = Math.floor((i / slots.length) * 100);
    if (pct !== lastPct) {
      lastPct = pct;
      progressBar.style.width = pct + '%';
    }
  }

  await waitUntil(start + totalMs);
  setOpticalState(false);
  progressBar.style.width = '100%';

  await torchQueue;
  let warn = torchFailed ? " (warning: some flashlight commands failed - try Screen mode)" : "";
  if (currentMode === 'torch' && torchLat.length) {
    const max = Math.max(...torchLat), avg = torchLat.reduce((a, b) => a + b, 0) / torchLat.length;
    warn += `<br>Flashlight response: avg ${Math.round(avg)} ms, worst ${Math.round(max)} ms.`;
    if (max > T * 1.2) warn += ` <b>Too slow for this speed - choose Safe speed or "Screen flash".</b>`;
  }
  statusText.innerText = `Ready to transmit (Sent "${text}")`;
  bitstreamInfo.innerHTML = `✓ Complete! Sent: "<b>${escapeHtml(text)}</b>"${warn}`;
}

function escapeHtml(s) {
  return s.replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

/* ---------- Navigation & Modals ---------- */

function setupNavModals() {
  const navLinks = document.querySelectorAll('.nav-link');
  const modals = document.querySelectorAll('.modal-overlay');
  const closeBtns = document.querySelectorAll('.modal-close');

  navLinks.forEach(link => {
    link.addEventListener('click', (e) => {
      e.preventDefault();
      const target = link.dataset.modal;

      navLinks.forEach(l => l.classList.remove('active'));
      link.classList.add('active');

      modals.forEach(m => m.classList.remove('active'));

      if (target !== 'home') {
        const modal = document.getElementById(`modal-${target}`);
        if (modal) modal.classList.add('active');
      }
    });
  });

  const goHome = () => {
    modals.forEach(m => m.classList.remove('active'));
    navLinks.forEach(l => l.classList.remove('active'));
    document.querySelector('.nav-link[data-modal="home"]').classList.add('active');
  };

  closeBtns.forEach(btn => btn.addEventListener('click', goHome));
  modals.forEach(modal => {
    modal.addEventListener('click', (e) => { if (e.target === modal) goHome(); });
  });

  const copyBtn = document.getElementById('copy-code-btn');
  if (copyBtn) {
    copyBtn.addEventListener('click', () => {
      const code = document.getElementById('arduino-source').innerText;
      navigator.clipboard.writeText(code).then(() => {
        copyBtn.innerText = "✓ Copied to Clipboard!";
        setTimeout(() => { copyBtn.innerText = "📋 Copy Code"; }, 2500);
      });
    });
  }
}

} // end browser part
