// Import the Clay package
var Clay = require('@rebble/clay');
// Load our Clay configuration file
var clayConfig = require('./config');
// Initialize Clay with auto-handling enabled
var clay = new Clay(clayConfig, null, { autoHandleEvents: true });

// --- CONFIGURATION ---
// Change this to whatever endpoint or web hook you want to hit!
var customEndpointUrl = 'https://api.breezylog.com/v1/status/summary'; 

// Helper function for XMLHttpRequest
var xhrRequest = function (url, type, callback) {
  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    callback(this.status, this.responseText);
  };
  xhr.onerror = function () {
    callback(0, null);
  };
  xhr.open(type, url);
  xhr.send();
};

// Convert Open-Meteo weather code to human-readable condition
function weatherCodeToCondition(code) {
  if (code === 0) return 'CLEAR';
  if (code <= 3) return 'CLOUDY';
  if (code <= 48) return 'FOG';
  if (code <= 55) return 'DRIZZLE';
  if (code <= 57) return 'FZ. DRIZZLE';
  if (code <= 65) return 'RAIN';
  if (code <= 67) return 'FZ. RAIN';
  if (code <= 75) return 'SNOW';
  if (code <= 77) return 'SNOW GRAINS';
  if (code <= 82) return 'SHOWERS';
  if (code <= 86) return 'SNOW SHWRS';
  if (code === 95) return 'T-STORM';
  if (code <= 99) return 'T-STORM';
  return 'UNKNOWN';
}

function locationSuccess(pos) {
  // Construct Open-Meteo API URL
  var url = 'https://api.open-meteo.com/v1/forecast?' +
      'latitude=' + pos.coords.latitude +
      '&longitude=' + pos.coords.longitude +
      '&current=temperature_2m,weather_code';

  // Send request to Open-Meteo
  xhrRequest(url, 'GET', function(status, responseText) {
    var temperature = 0;
    var conditions = 'MET ERR';

    if (status === 200 && responseText) {
      var json = JSON.parse(responseText);
      temperature = Math.round(json.current.temperature_2m);
      conditions = weatherCodeToCondition(json.current.weather_code);
    } else {
      conditions = 'OFFLINE';
    }

    // Pass the weather facts into the custom API chain
    fetchCustomApi(temperature, conditions);
  });
}

function locationError(err) {
  console.log('Error requesting location!');
  fetchCustomApi(0, 'NO GPS');
}

function fetchCustomApi(temperature, conditions) {
  xhrRequest(customEndpointUrl, 'GET', function(status, responseText) {
    var customString = 'API ERR';

    if (status === 200 && responseText) {
      try {
        var res = JSON.parse(responseText);
        // Safely extract a 'text', 'status', or fallback to raw string
        var rawText = res.text || res.status || responseText;
        customString = rawText.toString().toUpperCase().substring(0, 20);
      } catch (err) {
        // Fallback to parsing raw text response directly if it isn't JSON
        customString = responseText.toString().toUpperCase().substring(0, 20);
      }
    } else {
      customString = 'API OFFLINE';
    }

    // Now bundle everything and push it over AppMessage down to the C layer
    sendToWatch(temperature, conditions, customString);
  });
}

function sendToWatch(temperature, conditions, customApiString) {
  var dictionary = {
    'MESSAGE_KEY_TEMPERATURE': temperature,
    'MESSAGE_KEY_CONDITIONS': conditions,
    'MESSAGE_KEY_CUSTOM_API': customApiString
  };

  Pebble.sendAppMessage(dictionary,
    function(e) {
      console.log('Telemetry payload sent to Pebble successfully!');
    },
    function(e) {
      console.log('Error sending telemetry payload to Pebble!');
    }
  );
}

function updateTelemetryData() {
  navigator.geolocation.getCurrentPosition(
    locationSuccess,
    locationError,
    { timeout: 15000, maximumAge: 60000 }
  );
}

// Listen for when the watchface is opened
Pebble.addEventListener('ready', function(e) {
  console.log('PebbleKit JS ready!');
  updateTelemetryData();
});

// Listen for when an AppMessage is received
Pebble.addEventListener('appmessage', function(e) {
  console.log('AppMessage received from watch layer!');
  if (e.payload['MESSAGE_KEY_REQUEST_WEATHER'] || e.payload['REQUEST_WEATHER']) {
    updateTelemetryData();
  }
});