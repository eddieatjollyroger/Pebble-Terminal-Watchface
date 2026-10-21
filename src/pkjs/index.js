// Phone side of the watchface: weather, settings storage, and the settings page.
// Weather comes from the free Open-Meteo service (no account or API key needed).

// Where the settings page is hosted. Put watchface-settings.html in your GitHub Pages repo
// and change this to its address.
var CONFIG_URL = 'https://eddieatjollyroger.github.io/Pebble-HA-TV-Remote/watchface-settings.html';

var REFRESH_MS = 30 * 60 * 1000;

var DEFAULTS = {
  prompt: 'user@pebble',
  temp_unit: 'auto',      // 'auto' | 'c' | 'f'
  dist_unit: 'auto',      // 'auto' | 'km' | 'mi'
  seconds: true,
  comps: { time: true, date: true, temp: true, batt: true, step: true, dist: true, hr: true }
};

var settings = JSON.parse(JSON.stringify(DEFAULTS));
var weatherTimer = null;

function loadSettings() {
  try {
    var saved = JSON.parse(localStorage.getItem('wf_settings') || 'null');
    if (!saved) return;
    ['prompt', 'temp_unit', 'dist_unit', 'seconds'].forEach(function(k) {
      if (saved[k] !== undefined) settings[k] = saved[k];
    });
    Object.keys(DEFAULTS.comps).forEach(function(k) {
      if (saved.comps && saved.comps[k] !== undefined) settings.comps[k] = !!saved.comps[k];
    });
  } catch (e) { /* use defaults */ }
}

function saveSettings() {
  try { localStorage.setItem('wf_settings', JSON.stringify(settings)); } catch (e) { }
}

function isUS() {
  return String(navigator.language || '').toLowerCase() === 'en-us';
}
function useFahrenheit() {
  if (settings.temp_unit === 'f') return true;
  if (settings.temp_unit === 'c') return false;
  return isUS();
}
function useMiles() {
  if (settings.dist_unit === 'mi') return true;
  if (settings.dist_unit === 'km') return false;
  return isUS();
}

// Bit positions match the watch: time, date, temp, battery, steps, distance, heart rate
function complicationMask() {
  var order = ['time', 'date', 'temp', 'batt', 'step', 'dist', 'hr'];
  var mask = 0;
  order.forEach(function(k, i) { if (settings.comps[k]) mask |= (1 << i); });
  return mask;
}

function send(payload, tries) {
  Pebble.sendAppMessage(payload, null, function() {
    if (tries < 3) setTimeout(function() { send(payload, tries + 1); }, 1000);
  });
}

function sendSettings() {
  send({
    '2': complicationMask(),
    '3': String(settings.prompt || DEFAULTS.prompt).substring(0, 12),
    '4': useMiles() ? 1 : 0,
    '5': settings.seconds ? 1 : 0
  }, 0);
}

/* ---------- Weather ---------- */

// Kept short so it fits on one line of the watchface
function conditionText(code) {
  if (code === 0) return 'Clear';
  if (code === 1 || code === 2) return 'Partly';
  if (code === 3) return 'Cloudy';
  if (code === 45 || code === 48) return 'Fog';
  if (code >= 51 && code <= 57) return 'Drizzle';
  if (code >= 61 && code <= 67) return 'Rain';
  if (code >= 71 && code <= 77) return 'Snow';
  if (code >= 80 && code <= 82) return 'Showers';
  if (code === 85 || code === 86) return 'Snow';
  if (code >= 95) return 'Storm';
  return '';
}

function fetchWeather() {
  if (!settings.comps.temp) return;    // no weather line: don't use location or network
  navigator.geolocation.getCurrentPosition(function(pos) {
    var fahrenheit = useFahrenheit();
    var url = 'https://api.open-meteo.com/v1/forecast' +
      '?latitude=' + pos.coords.latitude.toFixed(2) +
      '&longitude=' + pos.coords.longitude.toFixed(2) +
      '&current=temperature_2m,weather_code' +
      '&temperature_unit=' + (fahrenheit ? 'fahrenheit' : 'celsius');

    var xhr = new XMLHttpRequest();
    xhr.open('GET', url, true);
    xhr.onload = function() {
      if (xhr.status !== 200) return;
      try {
        var cur = JSON.parse(xhr.responseText).current;
        send({
          '0': Math.round(cur.temperature_2m) + (fahrenheit ? 'F' : 'C'),
          '1': conditionText(cur.weather_code)
        }, 0);
      } catch (e) { console.log('weather parse error: ' + e); }
    };
    xhr.onerror = function() { console.log('weather network error'); };
    xhr.send();
  }, function(err) {
    console.log('location error: ' + err.message);
  }, { timeout: 15000, maximumAge: 600000 });
}

function startWeather() {
  if (weatherTimer) clearInterval(weatherTimer);
  fetchWeather();
  weatherTimer = setInterval(fetchWeather, REFRESH_MS);
}

/* ---------- Settings page ---------- */

Pebble.addEventListener('ready', function() {
  loadSettings();
  sendSettings();
  startWeather();
});

Pebble.addEventListener('showConfiguration', function() {
  var returnUrl = 'pebblejs://close#';
  Pebble.openURL(CONFIG_URL + '?s=' + encodeURIComponent(JSON.stringify(settings)) +
                 '&return_to=' + encodeURIComponent(returnUrl));
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e.response) return;
  try {
    var incoming = JSON.parse(decodeURIComponent(e.response));
    if (typeof incoming.prompt === 'string') {
      // letters, digits and a few symbols only, so the watch font can draw it
      var clean = incoming.prompt.replace(/[^A-Za-z0-9@._~-]/g, '').substring(0, 12);
      settings.prompt = clean || DEFAULTS.prompt;
    }
    if (incoming.temp_unit === 'auto' || incoming.temp_unit === 'c' || incoming.temp_unit === 'f') settings.temp_unit = incoming.temp_unit;
    if (incoming.dist_unit === 'auto' || incoming.dist_unit === 'km' || incoming.dist_unit === 'mi') settings.dist_unit = incoming.dist_unit;
    if (incoming.seconds !== undefined) settings.seconds = !!incoming.seconds;
    if (incoming.comps) {
      Object.keys(DEFAULTS.comps).forEach(function(k) {
        if (incoming.comps[k] !== undefined) settings.comps[k] = !!incoming.comps[k];
      });
    }
    saveSettings();
    sendSettings();
    startWeather();     // picks up a changed unit, or a re-enabled weather line
  } catch (err) {
    console.log('settings parse error: ' + err);
  }
});
