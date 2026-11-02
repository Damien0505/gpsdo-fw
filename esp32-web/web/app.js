"use strict";
const $ = (id) => document.getElementById(id);
const MINUS = "−";
const NO_VALUE = "--";

// ---------------------------------------------------------------- formatting
function signed(text, negative, positive) {
  return (negative ? MINUS : positive ? "+" : "") + text;
}
function ppbText(x100, plus = true) {
  if (x100 === null || x100 === undefined) return NO_VALUE;
  const value = Math.abs(x100) / 100;
  const text = value >= 1000 ? Math.round(value).toLocaleString("en") : value.toFixed(2);
  return signed(text, x100 < 0, plus && x100 > 0);
}
const group = (n) => Number(n).toLocaleString("en").replace(/,/g, " ");
function intText(n) {
  return signed(group(Math.abs(n)), n < 0, false);
}
function duration(seconds) {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  if (d > 0) return `${d} d ${h} h ${m} min`;
  if (h > 0) return `${h} h ${m} min`;
  return `${m} min ${seconds % 60} s`;
}
function coordinate(value, positive, negative) {
  return `${Math.abs(value).toFixed(6)}° ${value < 0 ? negative : positive}`;
}
function set(id, text) {
  const element = $(id);
  if (element.textContent !== text) element.textContent = text;
}

// ---------------------------------------------------------------- null meter
// Signed log scale: 0.1 ppb and 1000 ppb are 4 decades apart, zero in the middle
const METER_MAX = 1000;
function meterPosition(ppb) {
  const size = Math.log10(1 + Math.min(Math.abs(ppb), METER_MAX) / 0.1) / Math.log10(1 + METER_MAX / 0.1);
  return 50 + (ppb < 0 ? -size : size) * 50;
}
function buildMeter() {
  const scale = $("meter-scale");
  const add = (cls, ppb, label) => {
    const tick = document.createElement("div");
    tick.className = "tick " + cls;
    tick.style.left = meterPosition(ppb) + "%";
    scale.append(tick);
    if (label) {
      const text = document.createElement("div");
      // Left out on a narrow screen, where they would touch their neighbours
      text.className = "tick-label" + (Math.abs(ppb) === 0.1 || Math.abs(ppb) === METER_MAX ? " sparse" : "");
      text.textContent = label;
      text.style.left = meterPosition(ppb) + "%";
      if (ppb === METER_MAX) { text.style.left = "auto"; text.style.right = "0"; text.style.transform = "none"; }
      if (ppb === -METER_MAX) text.style.transform = "none";
      scale.append(text);
    }
  };
  for (const decade of [0.1, 1, 10, 100]) {
    for (let k = 2; k < 10; k += 2) {
      add("minor", decade * k);
      add("minor", -decade * k);
    }
  }
  for (const [ppb, label] of [[0.1, "0.1"], [1, "1"], [10, "10"], [100, "100"], [1000, "1000"]]) {
    add("", ppb, label);
    add("", -ppb, MINUS + label);
  }
  add("zero", 0, "0");
}
function updateMeter(x100, thresholdX100) {
  const needle = $("needle");
  needle.hidden = x100 === null || x100 === undefined;
  if (!needle.hidden) needle.style.left = meterPosition(x100 / 100) + "%";
  const band = $("meter-band");
  band.hidden = thresholdX100 === undefined;
  if (!band.hidden) {
    const low = meterPosition(-thresholdX100 / 100);
    band.style.left = low + "%";
    band.style.width = (meterPosition(thresholdX100 / 100) - low) + "%";
  }
}

// ---------------------------------------------------------------- live state
let status = null;
let config = {};
let configInfo = null;
let lastUpdate = 0;

function setLamp(id, cls, text) {
  const lamp = $(id);
  lamp.className = "lamp " + cls;
  lamp.querySelector("span").textContent = text;
}

function verdict(t) {
  const threshold = config.ppb_lock_threshold;
  const thresholdText = threshold === undefined ? "" : ` (limit ±${(threshold / 100).toFixed(2)} ppb)`;
  if (!t.adjusting) {
    const warmup = config.warmup_time;
    if (warmup !== undefined && warmup > t.uptime) {
      return `Warming up the oscillator: corrections start in ${duration(warmup - t.uptime)}.`;
    }
    return "Warming up the oscillator.";
  }
  if (!t.gps_lock) return "No PPS from the GPS. Check the antenna: the oscillator is running without its reference.";
  if (t.ppb_mean === null) return "Collecting the first measurements.";
  if (t.ppb_lock) return `Locked to GPS${thresholdText}.`;
  return `Settling towards lock${thresholdText}. The mean covers 128 s, so this takes a while after a change.`;
}

function renderStatus() {
  const s = status;
  if (!s) return;
  const t = s.tlm;
  const up = s.link.up;
  document.body.classList.toggle("stale", !up);

  if (t) {
    set("ppb", ppbText(t.ppb_mean));
    updateMeter(t.ppb_mean, config.ppb_lock_threshold);
    set("verdict", up ? verdict(t) : "These are the last values received.");
    set("r-freq", `${group(t.freq)} Hz`);
    set("r-error", `${intText(t.error)} Hz`);
    set("r-inst", `${ppbText(t.ppb_inst)} ppb`);
    set("r-correction", t.adjusting ? `${intText(t.correction)} steps` : "Warming up");
    set("r-pwm", group(t.pwm));
    set("r-shift", `${intText(t.pps_shift)} cycles`);
    set("r-shift-ms", `${signed((Math.abs(t.pps_shift_ms) / 10000).toFixed(4), t.pps_shift_ms < 0, false)} ms`);
    set("r-syncs", String(t.pps_sync_count));
    set("r-sats", String(t.sats));
    set("r-uptime", duration(t.uptime));
    if (up) {
      setLamp("lamp-gps", t.gps_lock ? "on" : "off", t.gps_lock ? "GPS locked" : "No GPS lock");
      setLamp("lamp-ppb", t.ppb_lock ? "on" : "", t.ppb_lock ? "PPB locked" : "PPB not locked");
    }
    $("lamp-warm").hidden = t.adjusting || !up;
    if (!t.adjusting) setLamp("lamp-warm", "warn", "Warming up");
  }
  // Without data, the state of the GPSDO is not known: last values would be claims
  if (!up) {
    setLamp("lamp-gps", "", "GPS unknown");
    setLamp("lamp-ppb", "", "PPB unknown");
  }
  set("r-link", up ? "Receiving" : "Down");
  set("r-bad", group(s.link.bad_checksums));
  set("r-wifi", s.esp.ap_mode ? "Setup network" : `${s.esp.rssi} dBm`);
  if (configInfo) set("r-fw", configInfo.fw_version || NO_VALUE);
  set("clock", `${s.gps.time || "--:--:--"} UTC`);

  renderGps(s.gps);
  renderAlert();
}

const FIX_NAMES = { 1: "No fix", 2: "2D", 3: "3D" };
const SYSTEM_NAMES = { GP: "GPS", BD: "BeiDou", GB: "BeiDou", GL: "GLONASS", GA: "Galileo", QZ: "QZSS", GQ: "QZSS" };
let lastSatsKey = "";

function renderGps(g) {
  set("g-time", g.date || g.time ? `${g.date || ""} ${g.time || ""}`.trim() : NO_VALUE);
  set("g-fix", g.fix_quality > 0 ? (FIX_NAMES[g.fix_mode] || "Fix") : "No fix");
  set("g-lat", g.lat === undefined ? NO_VALUE : coordinate(g.lat, "N", "S"));
  set("g-lon", g.lon === undefined ? NO_VALUE : coordinate(g.lon, "E", "W"));
  set("g-loc", g.locator || NO_VALUE);
  set("g-alt", g.alt === undefined ? NO_VALUE : `${g.alt.toFixed(1)} m`);
  set("g-geoid", g.geoid === undefined ? NO_VALUE : `${g.geoid.toFixed(1)} m`);
  set("g-hdop", g.hdop === undefined ? NO_VALUE : g.hdop.toFixed(2));
  set("g-used", String(g.sats_used));
  set("g-view", String(g.sats_view));

  const key = JSON.stringify(g.sats);
  if (key === lastSatsKey) return;
  lastSatsKey = key;
  $("sat-empty").hidden = g.sats.length > 0;
  // Used first, strongest first
  const sorted = [...g.sats].sort((a, b) => (b[5] - a[5]) || (b[4] - a[4]) || (a[1] - b[1]));
  const rows = sorted.map(([talker, prn, elevation, azimuth, snr, used]) => {
    const row = document.createElement("tr");
    if (!used) row.className = "idle";
    const cell = (text, cls) => {
      const td = document.createElement("td");
      td.textContent = text;
      if (cls) td.className = cls;
      row.append(td);
      return td;
    };
    cell(SYSTEM_NAMES[talker] || talker);
    cell(prn, "num");
    cell(elevation < 0 ? NO_VALUE : `${elevation}°`, "num");
    cell(azimuth < 0 ? NO_VALUE : `${azimuth}°`, "num");
    const signal = cell("");
    const wrap = document.createElement("div");
    wrap.className = "snr";
    const value = document.createElement("b");
    value.textContent = snr < 0 ? NO_VALUE : snr;
    const bar = document.createElement("i");
    bar.style.width = `${snr < 0 ? 0 : Math.min(snr, 55) * 1.6}%`;
    wrap.append(value);
    if (snr >= 0) wrap.append(bar);
    signal.append(wrap);
    cell(used ? "Yes" : "No");
    return row;
  });
  $("sat-rows").replaceChildren(...rows);
}

function renderAlert() {
  const alert = $("alert");
  let text = "";
  const silent = Date.now() - lastUpdate > 4000;
  if (!lastUpdate || silent) {
    text = "Can't reach the ESP32. Values below are out of date. Retrying.";
  } else if (status && !status.link.up) {
    text = status.tlm
      ? "No data from the GPSDO. Check its serial cable and power. The ESP32 reconnects by itself."
      : "Nothing received from the GPSDO yet. Check the serial cable: TX to GPIO20, RX to GPIO21, and ground.";
  }
  alert.hidden = !text;
  if (text) alert.textContent = text;
}

function onStatus(data) {
  status = data;
  lastUpdate = Date.now();
  renderStatus();
}
function onConfig(data) {
  configInfo = data;
  config = data.config || {};
  renderSettings();
  renderStatus();
}

// ---------------------------------------------------------------- connection
// WebSocket for live data, plain requests as a fallback while it is down
let socket = null;
let socketLive = false;

function connectSocket() {
  socket = new WebSocket(`${location.protocol === "https:" ? "wss" : "ws"}://${location.host}/ws`);
  socket.onopen = () => { socketLive = true; };
  socket.onmessage = (event) => {
    const data = JSON.parse(event.data);
    if (data.type === "status") onStatus(data);
    else if (data.type === "config") onConfig(data);
  };
  socket.onclose = () => {
    socketLive = false;
    setTimeout(connectSocket, 3000);
  };
  socket.onerror = () => socket.close();
}
async function getJson(path) {
  const response = await fetch(path, { cache: "no-store" });
  if (!response.ok) throw new Error(`${path}: ${response.status}`);
  return response.json();
}
async function pollStatus() {
  if (!socketLive) {
    try {
      onStatus(await getJson("/api/status"));
      if (!configInfo || !configInfo.complete) onConfig(await getJson("/api/config"));
    } catch (error) { /* renderAlert shows it */ }
  }
  renderAlert();
  setTimeout(pollStatus, socketLive ? 2000 : 1000);
}

// ---------------------------------------------------------------- sending
function toast(text) {
  const element = $("toast");
  element.textContent = text;
  element.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { element.hidden = true; }, 5000);
}
const ERROR_TEXT = {
  RANGE: "The GPSDO refused this value: out of range.",
  KEY: "The GPSDO doesn't know this setting.",
  BUSY: "The GPSDO refused while auto-scale is on.",
  FORMAT: "The GPSDO couldn't read this command.",
  CHECKSUM: "The command was damaged twice on the serial link.",
  TIMEOUT: "The GPSDO didn't reply. Check the serial link.",
  LINK_DOWN: "Not sent: no link with the GPSDO.",
};
async function post(path, body) {
  const response = await fetch(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  if (response.status === 401) throw new Error("The admin password is needed to change settings.");
  const data = await response.json().catch(() => null);
  if (data === null) throw new Error(`The ESP32 answered with an error (${response.status}).`);
  return data;
}

function confirmDialog(title, text, action) {
  return new Promise((resolve) => {
    const dialog = $("confirm");
    set("confirm-title", title);
    set("confirm-text", text);
    set("confirm-ok", action);
    dialog.addEventListener("close", () => resolve(dialog.returnValue === "ok"), { once: true });
    dialog.returnValue = "cancel";
    dialog.showModal();
  });
}

async function runAction(action, title, text, button, done) {
  if (!(await confirmDialog(title, text, button))) return;
  try {
    const data = await post("/api/action", { action });
    const result = data.results && data.results[action];
    toast(result && result.ok ? done : (result && ERROR_TEXT[result.error]) || data.error || "That didn't work.");
  } catch (error) {
    toast(error.message);
  }
}

// ---------------------------------------------------------------- settings
const BAUDS = [9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600];
const options = (list) => list.map(([value, label]) => ({ value, label }));
const baudOptions = options(BAUDS.map((b) => [b, String(b)]));
const FACTOR_LIMITS = { 0: [1, 60], 1: [1, 60], 2: [10, 600] };

// Order within a group matters: it is the order of sending, and some settings reset others
const GROUPS = [
  { title: "Control", fields: [
    { key: "ocxo_model", label: "OCXO model", options: options([[0, "Isotemp"], [1, "OX256B"], [2, "Unknown"]]), resets: { key: "warmup_time", values: { 0: 70, 1: 100, 2: 70 } }, hint: "Changing it also sets the warm-up time to suit the model." },
    { key: "warmup_time", label: "Warm-up time", min: 0, max: 1000, unit: "s" },
    { key: "correction_algo", label: "Correction algorithm", options: options([[0, "Dankar"], [1, "Fredzo"], [2, "Eric H"]]), resets: { key: "correction_factor", values: { 0: 10, 1: 10, 2: 300 } }, hint: "Changing it also sets the correction factor to suit the algorithm." },
    { key: "correction_factor", label: "Correction factor", limits: () => FACTOR_LIMITS[currentValue("correction_algo")] || [1, 600] },
    { key: "ppb_lock_threshold", label: "PPB lock limit", min: 0, max: 10, step: 0.05, scale: 100, unit: "ppb" },
    { key: "pwm_auto_save", label: "Save PWM once locked", toggle: true },
    { key: "pps_auto_sync", label: "Sync PPS once locked", toggle: true },
  ] },
  { title: "PPS output", fields: [
    { key: "pps_sync_on", label: "Resync when it drifts", toggle: true },
    { key: "pps_sync_delay", label: "Resync after", min: 0, max: 4294967294, hint: "Seconds beyond the threshold before the output resyncs." },
    { key: "pps_sync_threshold", label: "Resync threshold", min: 0, max: 4294967294, hint: "In clock cycles, at 70 MHz." },
  ] },
  { title: "GPS", fields: [
    { key: "gps_time_offset", label: "Time zone offset", min: -14, max: 14, unit: "h", hint: "For the LCD. Times on this page stay in UTC." },
    { key: "date_format", label: "LCD date format", options: options([[0, "dd/mm/yy"], [1, "mm/dd/yy"], [2, "yy/mm/dd"], [3, "dd.mm.yy"], [4, "yy-mm-dd"]]) },
    { key: "gps_baudrate", label: "GPS module baud rate", options: baudOptions, hint: "Must match the module, or the GPSDO loses the GPS. Leave at 9600 unless you know it differs.",
      warn: "The GPSDO will talk to its GPS module at the new speed. If the module doesn't follow, the time and satellites disappear until this is set back from the GPS menu." },
    { key: "comm_baudrate", label: "Serial link baud rate", options: baudOptions, hint: "Applies when the GPSDO restarts. This interface only works at 115200.",
      warn: "This interface only works at 115200. After the GPSDO restarts it will lose contact until GPS > Comm is set back to 115200 on the front panel." },
    { readOnly: "gps_model", label: "GPS module", names: ["ATGM336H", "NEO-6M", "NEO-M9N", "Not detected"] },
  ] },
  { title: "Display and trend", fields: [
    { key: "contrast", label: "LCD contrast", min: 0, max: 100, unit: "%" },
    { key: "boot_menu", label: "LCD start screen", options: options([[0, "Main"], [1, "Date"], [2, "Date and time"], [3, "Trend"]]) },
    { key: "trend_auto_v", label: "Automatic vertical scale", toggle: true },
    { key: "trend_v_scale", label: "Vertical scale", min: 0, max: 42949672, step: 0.1, scale: 100, unit: "ppb", disabledBy: "trend_auto_v", hint: "In ppb, rounded by the GPSDO." },
    { key: "trend_auto_h", label: "Automatic horizontal scale", toggle: true },
    { key: "trend_h_scale", label: "Horizontal scale", options: options([1, 2, 4, 8, 16, 32, 64].map((v) => [v, `${v} s per point`])), disabledBy: "trend_auto_h" },
  ] },
];
const FIELDS = GROUPS.flatMap((g) => g.fields).filter((f) => f.key);
const fieldElements = {};
let formBuilt = false;

function factorOf(field) { return field.scale || 1; }

function buildForm() {
  const form = $("settings-form");
  for (const group of GROUPS) {
    const set_ = document.createElement("fieldset");
    const legend = document.createElement("legend");
    legend.textContent = group.title;
    set_.append(legend);
    for (const field of group.fields) {
      const row = document.createElement("div");
      row.className = "field";
      const label = document.createElement(field.readOnly ? "span" : "label");
      label.textContent = field.label;
      row.append(label);
      if (field.readOnly) {
        const value = document.createElement("span");
        value.className = "static-value";
        value.dataset.readOnly = field.readOnly;
        row.append(value);
        set_.append(row);
        field.element = value;
        continue;
      }
      let input;
      if (field.options) {
        input = document.createElement("select");
        for (const option of field.options) input.add(new Option(option.label, option.value));
      } else if (field.toggle) {
        input = document.createElement("input");
        input.type = "checkbox";
        input.className = "switch";
      } else {
        input = document.createElement("input");
        input.type = "number";
        input.step = field.step || 1;
        input.inputMode = (field.min < 0 || field.step) ? "text" : "numeric";
      }
      input.id = "set-" + field.key;
      label.htmlFor = input.id;
      row.append(input);
      const hint = document.createElement("div");
      hint.className = "hint";
      row.append(hint);
      const result = document.createElement("div");
      result.className = "result";
      result.hidden = true;
      row.append(result);
      set_.append(row);
      fieldElements[field.key] = { input, row, hint, result, field };
      input.addEventListener(field.options || field.toggle ? "change" : "input", onFieldEdit);
    }
    form.append(set_);
  }
  form.addEventListener("submit", (event) => { event.preventDefault(); });
  formBuilt = true;
}

function inputValue(entry) {
  const { input, field } = entry;
  if (field.toggle) return input.checked ? 1 : 0;
  if (input.value === "") return null;
  return Math.round(Number(input.value) * factorOf(field));
}
function currentValue(key) {
  const entry = fieldElements[key];
  return entry ? inputValue(entry) : config[key];
}
function showValue(entry, value) {
  const { input, field } = entry;
  if (field.toggle) input.checked = value === 1;
  else input.value = field.scale ? String(value / field.scale) : String(value);
}
function fieldLimits(field) {
  return field.limits ? field.limits() : [field.min, field.max];
}
function validationError(entry) {
  const { field } = entry;
  if (field.options || field.toggle) return null;
  const value = inputValue(entry);
  if (value === null || Number.isNaN(value)) return "Enter a number.";
  const [min, max] = fieldLimits(field);
  const scaled = value / factorOf(field);
  if (scaled < min || scaled > max) return `Enter a value from ${min} to ${max}.`;
  return null;
}

function refreshFields() {
  let dirty = 0;
  let invalid = false;
  for (const field of FIELDS) {
    const entry = fieldElements[field.key];
    const value = inputValue(entry);
    const changed = field.key in config && value !== config[field.key];
    entry.row.classList.toggle("dirty", changed);
    if (changed) dirty++;
    const error = validationError(entry);
    if (error && changed) invalid = true;
    if (field.disabledBy) {
      const auto = currentValue(field.disabledBy) === 1;
      entry.input.disabled = auto;
    }
    const parts = [];
    if (!field.options && !field.toggle) {
      const [min, max] = fieldLimits(field);
      if (max < 1e6) parts.push(`${min} to ${max}${field.unit ? " " + field.unit : ""}`);
    }
    if (field.disabledBy && entry.input.disabled) parts.push("Set by the automatic scale.");
    else if (field.hint) parts.push(field.hint);
    if (error && changed) parts.splice(0, parts.length, error);
    const text = parts.join(". ").replace(/\.\./g, ".");
    if (entry.hint.textContent !== text) entry.hint.textContent = text;
    entry.hint.hidden = !text;
  }
  $("applybar").hidden = dirty === 0;
  set("apply-summary", dirty === 1 ? "1 change not applied" : `${dirty} changes not applied`);
  $("apply").disabled = invalid || applying;
  $("discard").disabled = applying;
  return dirty;
}
function onFieldEdit(event) {
  const entry = Object.values(fieldElements).find((e) => e.input === event.target);
  entry.result.hidden = true;
  const resets = entry.field.resets;
  if (resets && resets.key in config) {
    // Show what the GPSDO is going to do. Back to the saved value when the choice is undone.
    const value = inputValue(entry);
    const other = fieldElements[resets.key];
    showValue(other, value === config[entry.field.key] ? config[resets.key] : resets.values[value]);
    other.result.hidden = true;
  }
  refreshFields();
}

// New values from the GPSDO go in fields, except where an edit is in progress
const lastShown = {};
function renderSettings() {
  if (!formBuilt) buildForm();
  const ready = FIELDS.some((f) => f.key in config);
  $("settings-wait").hidden = ready;
  $("settings-form").hidden = !ready;
  if (!ready) return;
  for (const field of FIELDS) {
    if (!(field.key in config)) continue;
    const entry = fieldElements[field.key];
    const edited = field.key in lastShown && inputValue(entry) !== lastShown[field.key];
    if (!edited) {
      showValue(entry, config[field.key]);
      lastShown[field.key] = config[field.key];
    }
  }
  for (const group of GROUPS) {
    for (const field of group.fields) {
      if (field.readOnly && field.element) {
        field.element.textContent = field.names[config[field.readOnly]] || NO_VALUE;
      }
    }
  }
  refreshFields();
}

let applying = false;
function changedFields() {
  return FIELDS.filter((f) => f.key in config && inputValue(fieldElements[f.key]) !== config[f.key]);
}
async function applySettings() {
  const changed = changedFields();
  if (changed.length === 0) return;
  const warnings = changed.filter((f) => f.warn);
  for (const field of warnings) {
    if (!(await confirmDialog(`Change the ${field.label.toLowerCase()}?`, field.warn, "Change it"))) return;
  }
  const body = {};
  for (const field of changed) body[field.key] = inputValue(fieldElements[field.key]);
  applying = true;
  refreshFields();
  set("apply-summary", "Applying");
  try {
    const data = await post("/api/config", body);
    if (!data.results) throw new Error(data.error || "The ESP32 refused the request.");
    let failed = 0;
    for (const field of changed) {
      const entry = fieldElements[field.key];
      const result = data.results[field.key];
      entry.result.hidden = false;
      if (result && result.ok) {
        entry.result.className = "result ok";
        entry.result.textContent = "Applied";
        lastShown[field.key] = body[field.key];
        config[field.key] = body[field.key];
        const hide = entry.result;
        setTimeout(() => { hide.hidden = true; }, 4000);
      } else {
        failed++;
        entry.result.className = "result fail";
        entry.result.textContent = (result && ERROR_TEXT[result.error]) || "Not applied.";
      }
    }
    toast(failed ? `${failed} of ${changed.length} not applied.` : "Applied. Keep the GPSDO powered for 15 s while it saves.");
    // Values the GPSDO rounded or reset
    onConfig(await getJson("/api/config"));
  } catch (error) {
    toast(error.message);
  }
  applying = false;
  refreshFields();
}
function discardSettings() {
  for (const field of FIELDS) {
    if (field.key in config) {
      const entry = fieldElements[field.key];
      showValue(entry, config[field.key]);
      lastShown[field.key] = config[field.key];
      entry.result.hidden = true;
    }
  }
  refreshFields();
}

// ---------------------------------------------------------------- trend
let plot = null;
let plotRange = "1h";
let trendTimer = null;
let zoomed = false;

function cssVar(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}
function buildPlot(range, data) {
  const element = $("chart");
  if (plot) plot.destroy();
  const ink = cssVar("--muted");
  const rule = cssVar("--rule");
  const accent = cssVar("--accent");
  const band = cssVar("--band");
  const timeText = (seconds, withSeconds) => {
    const text = new Date(seconds * 1000).toISOString();
    return text.slice(11, withSeconds ? 19 : 16);
  };
  const axis = { stroke: ink, grid: { stroke: rule, width: 1 }, ticks: { stroke: rule, width: 1, size: 5 }, font: "13px " + cssVar("--panel-face"), gap: 4 };
  const coarse = range === "24h";
  const series = [{}];
  if (coarse) {
    series.push({ label: "Lowest", stroke: "transparent", width: 0, points: { show: false } });
    series.push({ label: "Highest", stroke: "transparent", width: 0, points: { show: false } });
  }
  series.push({ label: "Mean", stroke: accent, width: 2, points: { show: false } });
  plot = new uPlot({
    width: element.clientWidth,
    height: element.clientHeight || 340,
    legend: { show: false },
    cursor: { y: false, points: { size: 8, fill: accent, stroke: cssVar("--panel"), width: 2 }, drag: { x: true, y: false, setScale: true } },
    scales: { x: { time: true }, y: { range: (u, min, max) => {
      if (min === null || max === null) return [-1, 1];
      const pad = Math.max((max - min) * 0.08, 0.05);
      return [min - pad, max + pad];
    } } },
    bands: coarse ? [{ series: [2, 1], fill: band }] : [],
    axes: [{ ...axis, values: (u, ticks) => ticks.map((t) => timeText(t, false)) }, { ...axis, size: 60, values: (u, ticks) => ticks.map((v) => (v < 0 ? MINUS : "") + Math.abs(v).toFixed(Math.abs(ticks[1] - ticks[0]) < 1 ? 2 : 0)) }],
    series,
    hooks: {
      setCursor: [(u) => showTrendReadout(u)],
      setScale: [(u, key) => { if (key === "x") zoomed = u.scales.x.min > u.data[0][0] || u.scales.x.max < u.data[0][u.data[0].length - 1]; }],
    },
  }, data, element);
  element.ondblclick = () => { zoomed = false; };
}
function showTrendReadout(u) {
  const i = u.cursor.idx;
  const out = $("trend-readout");
  if (i === null || i === undefined) { out.innerHTML = "&nbsp;"; return; }
  const time = new Date(u.data[0][i] * 1000).toISOString().slice(11, plotRange === "1h" ? 19 : 16);
  const fmt = (v) => (v === null || v === undefined ? "no data" : `${ppbText(Math.round(v * 100))} ppb`);
  out.textContent = plotRange === "1h"
    ? `${time}: ${fmt(u.data[1][i])}`
    : `${time}: mean ${fmt(u.data[3][i])}, from ${fmt(u.data[1][i])} to ${fmt(u.data[2][i])}`;
}
async function loadTrend() {
  let history;
  try {
    history = await getJson(`/api/history?range=${plotRange}`);
  } catch (error) {
    set("trend-note", "Couldn't load the history. Retrying.");
    return;
  }
  const now = Date.now() / 1000;
  const count = history.rows.length;
  const times = history.rows.map((_, i) => now - (count - 1 - i) * history.step_s);
  const toPpb = (v) => (v === null ? null : v / 100);
  let data;
  let means;
  if (plotRange === "1h") {
    means = history.rows.map(toPpb);
    data = [times, means];
    updateStats(means, means, means);
  } else {
    const lows = history.rows.map((r) => toPpb(r[0]));
    means = history.rows.map((r) => toPpb(r[1]));
    const highs = history.rows.map((r) => toPpb(r[2]));
    data = [times, lows, highs, means];
    updateStats(lows, means, highs);
  }
  set("trend-note", count === 0
    ? (plotRange === "24h" ? "The first point appears after a minute of data." : "No measurements yet. They start once the warm-up is over.")
    : "History restarts when the ESP32 restarts. Drag across the chart to zoom, double-click to reset.");
  if (!plot || plot.series.length !== data.length) buildPlot(plotRange, data);
  else if (!zoomed) plot.setData(data);
}
function updateStats(lows, means, highs) {
  const present = (a) => a.filter((v) => v !== null);
  const l = present(lows);
  const m = present(means);
  const h = present(highs);
  const fmt = (v) => `${ppbText(Math.round(v * 100))} ppb`;
  set("t-min", l.length ? fmt(l.reduce((a, b) => Math.min(a, b))) : NO_VALUE);
  set("t-mean", m.length ? fmt(m.reduce((a, b) => a + b) / m.length) : NO_VALUE);
  set("t-max", h.length ? fmt(h.reduce((a, b) => Math.max(a, b))) : NO_VALUE);
}
function setTrendActive(active) {
  clearInterval(trendTimer);
  trendTimer = null;
  if (active) {
    loadTrend();
    trendTimer = setInterval(loadTrend, plotRange === "1h" ? 10000 : 60000);
    requestAnimationFrame(resizePlot);
  }
}
function resizePlot() {
  if (plot && $("chart").clientWidth > 0) plot.setSize({ width: $("chart").clientWidth, height: $("chart").clientHeight || 340 });
}

// ---------------------------------------------------------------- console
let consoleTimer = null;
async function loadConsole() {
  try {
    const response = await fetch("/api/console", { cache: "no-store" });
    if (!response.ok) return;
    const text = await response.text();
    const out = $("console-text");
    if ($("console-follow").checked || !out.textContent) {
      out.textContent = text;
      out.scrollTop = out.scrollHeight;
    }
  } catch (error) { /* The alert already tells */ }
}
function setConsoleActive(active) {
  clearInterval(consoleTimer);
  consoleTimer = null;
  if (active) {
    loadConsole();
    consoleTimer = setInterval(loadConsole, 2000);
  }
}

// ---------------------------------------------------------------- navigation
function showTab(name) {
  for (const button of document.querySelectorAll("[role=tab]")) {
    button.setAttribute("aria-selected", String(button.dataset.tab === name));
  }
  for (const panel of document.querySelectorAll("[role=tabpanel]")) panel.hidden = panel.id !== name;
  setTrendActive(name === "trend");
  setConsoleActive(name === "console");
  try { history.replaceState(null, "", "#" + name); } catch (error) { /* Not important */ }
}

function init() {
  buildMeter();
  buildForm();
  for (const button of document.querySelectorAll("[role=tab]")) {
    button.addEventListener("click", () => showTab(button.dataset.tab));
  }
  for (const button of document.querySelectorAll("[data-range]")) {
    button.addEventListener("click", () => {
      plotRange = button.dataset.range;
      for (const other of document.querySelectorAll("[data-range]")) {
        other.setAttribute("aria-pressed", String(other === button));
      }
      zoomed = false;
      if (plot) { plot.destroy(); plot = null; }
      setTrendActive(true);
    });
  }
  $("save-pwm").addEventListener("click", () => runAction("savepwm", "Save the current PWM?",
    "The GPSDO will start from this PWM value after its next power-up. Best done when it is locked.", "Save PWM", "PWM saved. Keep the GPSDO powered for 15 s while it saves."));
  $("force-sync").addEventListener("click", () => runAction("forcesync", "Force a PPS sync?",
    "The PPS output will jump to line up with the GPS. Anything timing from it will see one irregular pulse.", "Force sync", "PPS sync requested."));
  $("apply").addEventListener("click", applySettings);
  $("discard").addEventListener("click", discardSettings);
  window.addEventListener("resize", resizePlot);
  // Colours of the chart follow the mode of the system
  window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", () => {
    if (plot) { const data = plot.data; buildPlot(plotRange, data); }
  });
  const start = location.hash.slice(1);
  if (document.getElementById(start) && document.getElementById(start).getAttribute("role") === "tabpanel") showTab(start);
  connectSocket();
  pollStatus();
}
init();
