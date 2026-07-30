// Import the Clay package
var Clay = require("pebble-clay");
// Load our Clay configuration file
var clayConfig = require("./config.json");
// Initialize Clay with auto-handling enabled
var clay = new Clay(clayConfig);

// Default regex to extract data from the API response.
// First capture group is sent to the watch (max 12 chars).
var defaultApiRegex = /"summary"\s*:\s*"?([^"]+)"?/;

// Convert Open-Meteo weather code to human-readable condition
function weatherCodeToCondition(code) {
  if (code === 0) return "CLEAR";
  if (code <= 3) return "CLOUDY";
  if (code <= 48) return "FOG";
  if (code <= 55) return "DRIZZLE";
  if (code <= 57) return "FZ. DRIZZLE";
  if (code <= 65) return "RAIN";
  if (code <= 67) return "FZ. RAIN";
  if (code <= 75) return "SNOW";
  if (code <= 77) return "SNOW GRAINS";
  if (code <= 82) return "SHOWERS";
  if (code <= 86) return "SNOW SHWRS";
  if (code === 95) return "T-STORM";
  if (code <= 99) return "T-STORM";
  return "UNKNOWN";
}

// Helper function for XMLHttpRequest with optional auth header
function xhrRequest(url, type, authHeader, callback) {
  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    callback(this.status, this.responseText);
  };
  xhr.onerror = function () {
    callback(0, null);
  };
  xhr.open(type, url);
  if (authHeader) {
    xhr.setRequestHeader("Authorization", authHeader);
  }
  xhr.send();
}

// Load custom API config from Clay settings stored in localStorage
function loadApiConfig() {
  var config = {
    endpointUrl: "",
    regex: defaultApiRegex,
    auth: "",
    useFahrenheit: false,
  };
  try {
    var raw = localStorage.getItem("clay-settings");
    if (raw) {
      var claySettings = JSON.parse(raw);
      if (claySettings.CustomApiUrl) {
        config.endpointUrl = claySettings.CustomApiUrl;
      }
      if (claySettings.CustomApiRegex) {
        try {
          config.regex = new RegExp(claySettings.CustomApiRegex);
        } catch (_) {}
      }
      if (claySettings.CustomApiAuth) {
        config.auth = claySettings.CustomApiAuth;
      }
      if (claySettings.TemperatureUnit) {
        config.useFahrenheit = claySettings.TemperatureUnit;
      }
    }
  } catch (e) {
    console.error(e);
  }
  return config;
}

// Parse custom API response body with regex, then JSON fallback, then raw text
function parseCustomApiResponse(body, regex) {
  var match = regex.exec(body);
  if (match && match[1]) {
    return match[1].toString().toUpperCase().substring(0, 12);
  }
  try {
    var res = JSON.parse(body);
    var value = res.text || res.summary || res.status || res.value;
    return (
      String(value || "")
        .toUpperCase()
        .substring(0, 12) || "NO DATA"
    );
  } catch (_) {
    return body.toString().toUpperCase().substring(0, 12);
  }
}

// Fetch custom API endpoint and then send everything to the watch
function fetchCustomApi(conditions, altitude, apiConfig) {
  if (
    !apiConfig.endpointUrl ||
    apiConfig.endpointUrl.indexOf("https://") !== 0
  ) {
    sendToWatch(conditions, altitude, "API ERR");
    return;
  }

  xhrRequest(
    apiConfig.endpointUrl,
    "GET",
    apiConfig.auth,
    function (status, responseText) {
      var customApiString;

      if (status >= 200 && status < 300 && responseText) {
        try {
          customApiString = parseCustomApiResponse(
            responseText,
            apiConfig.regex
          );
        } catch (e) {
          console.error(e);
        }
      } else {
        customApiString = "API OFFLINE";
      }

      sendToWatch(conditions, altitude, customApiString);
    }
  );
}

// Send telemetry data to the Pebble watch
function sendToWatch(conditions, altitude, customApiString) {
  var dictionary = {};
  dictionary["Conditions"] = conditions;
  dictionary["Alt"] = altitude;
  dictionary["CustomApi"] = customApiString;

  Pebble.sendAppMessage(
    dictionary,
    function () {
      console.log("Telemetry payload sent to Pebble successfully!");
    },
    function () {
      console.log("Error sending telemetry payload to Pebble!");
    }
  );
}

// Fetch weather from Open-Meteo, then fetch custom API, then send to watch
function updateData() {
  var config = loadApiConfig();

  navigator.geolocation.getCurrentPosition(
    function (pos) {
      var altitude = (pos.coords.altitude || 0) * 3.281;

      var url =
        "https://api.open-meteo.com/v1/forecast?" +
        "latitude=" +
        pos.coords.latitude +
        "&longitude=" +
        pos.coords.longitude +
        "&current=temperature_2m,weather_code";

      xhrRequest(url, "GET", null, function (status, responseText) {
        var temperature = 0;
        var conditions;

        if (status === 200 && responseText) {
          var json = JSON.parse(responseText);

          var temp_value = Math.round(json.current.temperature_2m);

          if (config.useFahrenheit) {
            temp_value = Math.round((temp_value * 9) / 5 + 32);
          }

          conditions =
            temp_value +
            (config.useFahrenheit ? "F " : "C ") +
            weatherCodeToCondition(json.current.weather_code);
        } else {
          conditions = "OFFLINE";
        }

        fetchCustomApi(conditions, altitude, config);
      });
    },
    function (err) {
      console.log("Error requesting location!");
      fetchCustomApi("NO GPS", 0, config);
    },
    {
      timeout: 15000,
      maximumAge: 60000,
    }
  );
}

Pebble.addEventListener("ready", function () {
  console.log("Pebble ready, initializing app updates.");
  updateData();
});

Pebble.addEventListener("webviewclosed", function (e) {
  if (e && e.response) {
    console.log("Settings closed. Fetching fresh data with new configs...");
    updateData();
  }
});

Pebble.addEventListener("appmessage", function (e) {
  if (e.payload["RequestWeather"]) {
    updateData();
  }
});
