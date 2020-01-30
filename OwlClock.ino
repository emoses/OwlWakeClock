#include "secrets.h"
#include "types.h"
#include <WiFi.h>
#include "time.h"
#include <ESPmDNS.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include "color.h"
#include <math.h>


const char* MDNS_NAME = "owlclock";

#define BUTTON_PIN 23

#define RED_PIN 5
#define GREEN_PIN 18
#define BLUE_PIN 19
#define RED_CHANNEL 0
#define GREEN_CHANNEL 1
#define BLUE_CHANNEL 2

// use 8 bit precission for LEDC timer
#define LEDC_RESOLUTION  8
// use 5000 Hz as a LEDC base frequency
#define LEDC_BASE_FREQ 4096

//America/Los_Angeles
const char* TZ_STR = "PST+8PDT,M3.2.0,M11.1.0";

WebServer server(80);

uint16_t minuteOfDay(int hour, int minute) {
    return hour * 60 + minute;
}

char* minuteToTimeStr(uint16_t minuteOfDay, char* result, size_t n) {
    struct tm t;
    t.tm_hour = (int)minuteOfDay / 60;
    t.tm_min = minuteOfDay % 60;

    strftime(result, n, "%I:%M %p", &t);
    return result;
}

bool isBefore(uint16_t minuteOfDay, struct tm* timeinfo) {
    uint16_t minute = timeinfo->tm_hour * 60 + timeinfo->tm_min;
    return minuteOfDay < minute;
}

bool isAfter(uint16_t minuteOfDay, struct tm* timeinfo) {
    uint16_t minute = timeinfo->tm_hour * 60 + timeinfo->tm_min;
    return minuteOfDay > minute;
}


const settings_t DEFAULT_SETTINGS = {
    60,
    minuteOfDay(18, 0),
    minuteOfDay(5, 0),
    minuteOfDay(7, 0),
    30
};

const HSV OFF_COLOR = {0, 0, 0};
const HSV SLEEP_COLOR = {0, 200, 255};
const HSV WARN_COLOR = {42, 200, 255};
const HSV OK_COLOR = {85, 200, 255};

const int AUTO_OFF_TIME_SECONDS = 60 * 60;
const long pulseDuration = 1300;

//Globals
time_t sleepStart = 0;
long pulseStart = 0;
HSV pulseColor;
settings_t settings = DEFAULT_SETTINGS;
Mode_t mode = OFF;
Preferences preferences;

void wifiConnect() {
    // We start by connecting to a WiFi network
    Serial.print("Connecting to ");
    Serial.println(WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    Serial.println();
    Serial.println();
    Serial.print("Waiting for WiFi... ");

    while(WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }

    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
}

void setupLedPins() {
    pinMode(BLUE_PIN, OUTPUT);
    pinMode(RED_PIN, OUTPUT);
    pinMode(GREEN_PIN, OUTPUT);
    pinMode(BUTTON_PIN, INPUT_PULLDOWN);
    ledcSetup(RED_CHANNEL, LEDC_BASE_FREQ, LEDC_RESOLUTION);
    ledcAttachPin(RED_PIN, RED_CHANNEL);
    ledcSetup(GREEN_CHANNEL, LEDC_BASE_FREQ, LEDC_RESOLUTION);
    ledcAttachPin(GREEN_PIN, GREEN_CHANNEL);
    ledcSetup(BLUE_CHANNEL, LEDC_BASE_FREQ, LEDC_RESOLUTION);
    ledcAttachPin(BLUE_PIN, BLUE_CHANNEL);
}

bool writeSettings() {
    preferences.begin("owlclock");
    bool ret = _writeSettings();
    preferences.end();
    return ret;
}

bool _writeSettings() {
    size_t result = preferences.putBytes("settings", &settings, sizeof(settings_t));
    if (result != sizeof(settings_t)){
        Serial.println(String("Error writing settings, only ") + result + " bytes written");
        return false;
    }
    return true;
}


void loadSettings() {
    preferences.begin("owlclock");
    size_t settingsLen = preferences.getBytesLength("settings");
    if (settingsLen == 0){
        Serial.println("Using default settings");
        settings = DEFAULT_SETTINGS;
        _writeSettings();
    } else if (settingsLen != sizeof(settings_t)) {
        Serial.println("Clearing settings");
        preferences.remove("settings");
        settings = DEFAULT_SETTINGS;
        _writeSettings();
    } else {
        size_t result = preferences.getBytes("settings", &settings, sizeof(settings_t));
        if (result != sizeof(settings_t)){
            Serial.println("Error reading settings, resuming defaults");
            settings = DEFAULT_SETTINGS;
            _writeSettings();
        }
    }
    preferences.end();
}

const char * modeToString(enum Mode_t mode) {
    switch (mode) {
        case OFF:
            return "Off";
        case NAP:
            return "Nap";
        case SLEEP:
            return "Sleep";
        default:
            return "Unknown!?";
    }
}

char * colorToString(const HSV& color, char* result, int num) {
    if (color == OFF_COLOR) {
        return strncpy(result, "Off", num);
    } else if (color == SLEEP_COLOR) {
        return strncpy(result, "Sleepy Red", num);
    } else if (color == WARN_COLOR) {
        return strncpy(result, "Getting up yellow", num);
    } else if (color == OK_COLOR) {
        return strncpy(result, "Fine to wake green", num);
    } else {
        RGB rgb_color;
        color_HSV2RGB(&color, &rgb_color);
        snprintf(result, num, "(%d, %d, %d)", rgb_color.r, rgb_color.g, rgb_color.b);
        return result;
    }
}

const char* GET_ROOT_TPL = "<html><head>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />"
        "<title>Hoo!</title></head>"
        "<body><h1>Hoo!</h1><p>It's currently %s</p>"
        "<h2>Mode: %s</h2>"
        "<p>Current color: %s</p>"
        "<h2>Settings</h2><div>Nap length: %d minutes<br/>"
        "It's a nap if it starts before %s<br/>"
        "It's warning if it's after %s<br/>"
        "It's OK to get up after %s<br/>"
        "Brightness (0 - 255): %d</div>"
        "<form action=\"/start\" method=\"post\">"
        "<input type=\"submit\" value=\"Start (1 hour)\" />"
        "</form>"
        "<h3>Update settings:</h3>"
        "<form action=\"/editSettings\" method=\"post\">"
        "<label for=\"brightness\">Brightness</label> "
        "<input type=\"number\" min=\"0\" max=\"255\" name=\"brightness\" id=\"brightness\" value=\"%d\"/>"
        "<input type=\"submit\" value=\"Submit\" />"
        "</form>"
        "</body></html>";

const char* POST_START_TPL = "<html><head>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />"
        "<title>Timer set</title></head>"
        "<body><h1>OK!</h1><p>Timer set!</p><a href=\"/\">Back</a>"
        "</body></html>";

const char* UPDATED_TPL = "<html><head>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />"
        "<title>Updated</title></head>"
        "<body><h1>OK!</h1><p>Settings updated</p><a href=\"/\">Back</a>"
        "</body></html>";

const char* UPDATED_ERR_TPL = "<html><head>"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\" />"
        "<title>Update Error</title></head>"
        "<body><h1>OK!</h1><p>Settings not updated: %s</p><a href=\"/\">Back</a>"
        "</body></html>";

void writeTpl(WebServer &server, int status, const char* tpl, ...) {
    size_t len = (strlen(tpl) + 512);
    char* content = (char*)calloc(len, sizeof(char));
    va_list args;
    va_start(args, tpl);
    vsnprintf(content, len, tpl, args);

    server.send(status, "text/html", content);
    free(content);
    va_end(args);
}


void setup() {
    Serial.begin(115200);
    delay(10);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    setupLedPins();
    setColor(OFF_COLOR);
    loadSettings();

    wifiConnect();
    configTzTime(TZ_STR, "pool.ntp.org");
    time_t this_second = 0;
    while(!this_second) {
        time(&this_second);
        Serial.print("-");
        delay(100);
    }
    Serial.print(ctime(&this_second));

    if (!MDNS.begin(MDNS_NAME)) {
        Serial.println("Error setting up MDNS responder!");
        while(1) {
            delay(1000);
        }
    }
    Serial.println("mDNS responder started");

    // Start TCP (HTTP) server
    server.begin();
    Serial.println("TCP server started");

    // Add service to MDNS-SD
    MDNS.addService("http", "tcp", 80);

    server.on("/", HTTP_GET, [&]() {
            struct tm timeinfo;
            const char* time;
            if (!getLocalTime(&timeinfo)) {
                time = "Error";
            } else {
                time = asctime(&timeinfo);
            }
            char colorStr[32];
            colorToString(currentColor(), colorStr, 32);

            char napsBeforeStr[16];
            minuteToTimeStr(settings.napsBeforeTime, napsBeforeStr, 16);

            char sleepWarnStr[16];
            minuteToTimeStr(settings.sleepWarnTime, sleepWarnStr, 16);

            char sleepOKStr[16];
            minuteToTimeStr(settings.sleepOKTime, sleepOKStr, 16);


            writeTpl(server, 200, GET_ROOT_TPL,
                     time,
                     modeToString(mode),
                     colorStr,
                     settings.napLength,
                     napsBeforeStr,
                     sleepWarnStr,
                     sleepOKStr,
                     settings.brightness,
                     settings.brightness);
        });
    server.on("/start", HTTP_POST, [&]() {
            startSleep();
            server.send(200, "text/html", POST_START_TPL);
        });
    server.on("/editSettings", HTTP_POST, [&]() {
            bool err = false;
            char* msg;
            if (server.hasArg("brightness")){
                int bVal = server.arg("brightness").toInt();
                if (bVal < 0 || bVal > 255) {
                    err = true;
                    msg = "Brightness out of range";
                }
                settings.brightness = bVal;
                if (!writeSettings()) {
                    err = true;
                    msg = "Error updating settings";
                }
            }

            if (err) {
                writeTpl(server, 400, UPDATED_ERR_TPL, msg);
            } else {
                writeTpl(server, 200, UPDATED_TPL);
            }
        });
}

void setColor(uint8_t val, uint8_t channel){
    ledcWrite(channel, (val == 0) ? 256 : 255 - val);
}

void setColor(const RGB &color){
    setColor(color.r, RED_CHANNEL);
    setColor(color.g, GREEN_CHANNEL);
    setColor(color.b, BLUE_CHANNEL);
}

void setColor(const HSV &color){
    RGB rgb;
    color_HSV2RGB(&color, &rgb);
    setColor(rgb);
}


void pulse(const HSV &color) {
    pulseStart = millis();
    pulseColor = color;
    //pulseColor.v = floor((float)pulseColor.v * settings.brightness/255.0);
}

double ease(double p) {
    double f = 2*p - 1;
    if(p < 0.5) {
        return 1 - f*f*f*f;
    } else {
        return 1 - f*f;
    }
}

HSV calculatePulse() {
    double duration = millis() - pulseStart;
    if (duration > pulseDuration) {
        pulseStart = 0;
        return OFF_COLOR;
    }
    double p = (double)duration / (double)pulseDuration;
    HSV curr = pulseColor;
    curr.v = floor((double)curr.v * ease(p));
    return curr;
}

HSV currentColor() {
    if (pulseStart > 0) {
        return calculatePulse();
    }
    HSV result;
    switch(mode) {
        case OFF:
            return OFF_COLOR;
        case NAP:
            time_t now;
            time(&now);
            double duration;
            duration = difftime(now, sleepStart);
            if (duration/60 <= settings.napLength) {
                result = SLEEP_COLOR;
            } else if (duration >= settings.napLength * 60 + AUTO_OFF_TIME_SECONDS) {
                resetSleep();
                result = OFF_COLOR;
            } else {
                result = OK_COLOR;
            }
            break;
        case SLEEP:
            struct tm timeinfo;
            if (!getLocalTime(&timeinfo)){
                log_e("Couldn't get time");
                result = OFF_COLOR;
            } else {
                //This assumes midnight is a sleeping time
                if (isAfter(settings.napsBeforeTime, &timeinfo)) {
                    //This covers sleep time to midnight
                    result = SLEEP_COLOR;
                } else if (isBefore(settings.sleepWarnTime, &timeinfo)) {
                    result = SLEEP_COLOR;
                } else if (isAfter(settings.sleepWarnTime, &timeinfo) && isBefore(settings.sleepOKTime, &timeinfo)){
                    result = WARN_COLOR;
                } else if (isAfter(settings.sleepOKTime + AUTO_OFF_TIME_SECONDS / 60, &timeinfo)){
                    resetSleep();
                    result = OFF_COLOR;
                } else {
                    result = OK_COLOR;
                }
            }
    }
    result.v = floor((float) result.v * ((float)settings.brightness)/255.0);
    return result;
}

void startSleep() {
    Serial.print("Mode: ");
    Serial.println(modeToString(mode));
    if (mode != OFF) {
        return;
    }
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)){
        log_e("Couldn't get local time, aborting");
        pulse({0, 255, 255});
        return;
    }
    if (isBefore(settings.napsBeforeTime, &timeinfo) && isAfter(settings.sleepOKTime, &timeinfo)){
        mode = NAP;
        sleepStart = mktime(&timeinfo);
        Serial.print("Mode: ");
        Serial.println(modeToString(mode));
        pulse({42, 255, 255});
    } else {
        mode = SLEEP;
        sleepStart = mktime(&timeinfo);
        Serial.print("Mode: ");
        Serial.println(modeToString(mode));
        pulse({170, 255, 255});
    }
}

void resetSleep() {
    Serial.println("Reset");
    if (mode == OFF) {
        return;
    }
    mode = OFF;
}

const int64_t DEBOUNCE_DELAY = 35;
int64_t buttonDownSince = -1;
bool buttonWasPressed = false;

void handleButton() {
    int64_t buttonDownFor = buttonDownSince >= 0 ? millis() - buttonDownSince : 0;
    bool buttonReleased = false;
    bool buttonPressed = false;
    if (digitalRead(BUTTON_PIN) == HIGH) {
        if (buttonDownSince < 0) {
            buttonDownSince = millis();
        } else if (buttonDownFor >= DEBOUNCE_DELAY) {
            buttonPressed = true;
        }
    } else if (buttonDownFor >= DEBOUNCE_DELAY) {
        // button up
        buttonDownSince = -1;
        buttonReleased = true;
    }

    if (buttonPressed && !buttonWasPressed) {
        if (mode == OFF) {
            startSleep();
        } else {
            resetSleep();
        }
    }

    buttonWasPressed = buttonPressed;
}


void loop() {
    server.handleClient();

    handleButton();

    setColor(currentColor());
}
