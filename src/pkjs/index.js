// Import the Clay package
import Clay from "@rebble/clay";
// Load our Clay configuration file
import clayConfig from "./config";
// Initialize Clay with auto-handling enabled

const clay = new Clay(clayConfig, null, { autoHandleEvents: true });

var customEndpointUrl = "";

// Helper function for XMLHttpRequest
const xhrRequest = async (url, type) => {
  return new Promise((resolve, _) => {
    const xhr = new XMLHttpRequest();
    xhr.onload = function () {
      resolve({ status: this.status, body: this.responseText });
    };
    xhr.onerror = () => {
      resolve({ status: 0, body: undefined });
    };
    xhr.open(type, url);
    xhr.send();
  });
};

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

const updateData = async () => {
  const pos = await new Promise((resolve, reject) => {
    navigator.geolocation.getCurrentPosition(resolve, reject, {
      timeout: 15000,
      maximumAge: 60000,
    });
  }).catch((_) => {});

  let temperature = 0;
  let conditions = "MET ERR";
  let altitude = 0;

  if (pos.coords !== undefined) {
    altitude = pos.coords.altitude;

    const url =
      "https://api.open-meteo.com/v1/forecast?" +
      "latitude=" +
      pos.coords.latitude +
      "&longitude=" +
      pos.coords.longitude +
      "&current=temperature_2m,weather_code";
    // Send request to Open-Meteo
    const weatherRequest = await xhrRequest(url, "GET");
    if (weatherRequest.status === 200 && weatherRequest.body) {
      const json = JSON.parse(weatherRequest.body);
      temperature = Math.round(json.current.temperature_2m);
      conditions = weatherCodeToCondition(json.current.weather_code);
    } else {
      conditions = "OFFLINE";
    }
  }

  let customApiString = "API ERR";

  // we do not allow insecure calls in this house
  if (customEndpointUrl?.includes("https://")) {
    const customResponse = await xhrRequest(customEndpointUrl, "GET");

    if (customResponse.status === 200 && customResponse.body) {
      try {
        const res = JSON.parse(customResponse.body);

        const rawText = res.text || res.status || responseText;
        customApiString = rawText.toString().toUpperCase().substring(0, 12);
      } catch (_) {
        customApiString = responseText
          .toString()
          .toUpperCase()
          .substring(0, 12);
      }
    } else {
      customApiString = "API OFFLINE";
    }
  }

  const dictionary = {
    MESSAGE_KEY_TEMPERATURE: temperature,
    MESSAGE_KEY_CONDITIONS: conditions,
    MESSAGE_KEY_CUSTOM_API: customApiString,
    MESSAGE_KEY_ALT: altitude,
  };

  Pebble.sendAppMessage(
    dictionary,
    (_) => {
      console.log("Telemetry payload sent to Pebble successfully!");
    },
    (_) => {
      console.log("Error sending telemetry payload to Pebble!");
    },
  );
};

// Listen for when the watchface is opened
Pebble.addEventListener("ready", (_) => {
  console.log("PebbleKit JS ready!");
  updateData();
});

// Listen for when an AppMessage is received
Pebble.addEventListener("appmessage", (e) => {
  console.log("AppMessage received from watch layer!");
  if (
    e.payload["MESSAGE_KEY_REQUEST_WEATHER"] ||
    e.payload["REQUEST_WEATHER"]
  ) {
    updateData();
  }
});
