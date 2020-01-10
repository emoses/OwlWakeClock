#include "secrets.h"
#include "types.h"
#include <WiFi.h>
#include "time.h"
#include <ESPmDNS.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <Preferences.h>


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

String minuteToTimeStr(uint16_t minuteOfDay) {
    struct tm t;
    t.tm_hour = (int)minuteOfDay / 60;
    t.tm_min = minuteOfDay % 60;

    char buf[10];
    strftime(buf, 9, "%I:%M %p", &t);
    return String(buf);
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

const RGB OFF_COLOR = {0, 0, 0};
const RGB SLEEP_COLOR = {30, 0, 0};
const RGB WARN_COLOR = {30, 30, 0};
const RGB OK_COLOR = {0, 30, 0};

const int AUTO_OFF_TIME_SECONDS = 60 * 60;

//Globals
time_t sleepStart = 0;
long pulseStart = 0;
RGB pulseColor;
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

void writeSettings() {
    size_t result = preferences.putBytes("settings", &settings, sizeof(settings));
    if (result != sizeof(settings)){
        log_e("Error writing settings");
    }
}

void loadSettings() {
    preferences.begin("owlclock");
    size_t settingsLen = preferences.getBytesLength("settings");
    if (settingsLen == 0){
        settings = DEFAULT_SETTINGS;
        writeSettings();
    } else if (settingsLen != sizeof(settings)) {
        preferences.remove("settings");
        writeSettings();
    } else {
        size_t result = preferences.getBytes("settings", &settings, sizeof(settings));
        if (result != sizeof(settings)){
            log_e("Error reading settings, resuming defaults");
            settings = DEFAULT_SETTINGS;
            writeSettings();
        }
    }
}

String modeToString(enum Mode_t mode) {
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

String colorToString(const RGB& color) {
    if (color == OFF_COLOR) {
        return "Off";
    } else if (color == SLEEP_COLOR) {
        return "Sleepy Red";
    } else if (color == WARN_COLOR) {
        return "Getting up yellow";
    } else if (color == OK_COLOR) {
        return "Fine to wake green";
    } else {
        char colorStr[20];
        sprintf(colorStr, "(%d, %d, %d)", color.R, color.G, color.B);
        return String(colorStr);
    }
}



void setup() {
    Serial.begin(115200);
    delay(10);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    setupLedPins();
    setColor(OFF_COLOR);

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
            String time;
            if (!getLocalTime(&timeinfo)) {
                time = "Error";
            }
            time = String(asctime(&timeinfo));
            String content = "<html><head><title>Hoo!</title></head>";
            content += "<body><h1>Hoo!</h1><p>It's currently " + time + "</p>";
            content += "<h2>Mode: " + String(modeToString(mode)) + "</h2>";
            content += "<p>Current color: " + colorToString(currentColor()) + "</p>";
            content += "<h2>Settings</h2><div>Nap length: " + String(settings.napLength) + " minutes<br/>";
            content += "It's a nap if it starts before " + minuteToTimeStr(settings.napsBeforeTime) + "<br/>";
            content += "It's warning if it's after " + minuteToTimeStr(settings.sleepWarnTime) + "<br/>";
            content += "It's OK to get up after " + minuteToTimeStr(settings.sleepOKTime) + "<br/>";
            content += "Brighness (0 - 255): " + String(settings.brightness) + "</div>";
            content += "<form action=\"start\" method=\"post\">"
                    "<input type=\"submit\" value=\"Start (1 hour)\" />"
                    "</body></html>";
            server.send(200, "text/html", content);
        });
    server.on("/start", HTTP_POST, [&]() {
            startSleep();
            String content = "<html><head><title>Timer set</title></head>";
            content += "<body><h1>OK!</h1><p>Timer set!</p><a href=\"/\">Back</a>";
            content += "</body></html>";
            server.send(200, "text/html", content);
        });

}

void setColor(uint8_t val, uint8_t channel){
    ledcWrite(channel, (val == 0) ? 256 : 255 - val);
}

void setColor(RGB color){
    setColor(color.R, RED_CHANNEL);
    setColor(color.G, GREEN_CHANNEL);
    setColor(color.B, BLUE_CHANNEL);
}

void pulse(RGB color) {
    //TODO
}

RGB calculatePulse() {
    return {0, 0, 0};
}

RGB currentColor() {
    if (pulseStart > 0) {
        return calculatePulse();
    }
    switch(mode) {
        case OFF:
            return OFF_COLOR;
        case NAP:
            time_t now;
            time(&now);
            double duration;
            duration = difftime(now, sleepStart);
            if (duration/60 <= settings.napLength) {
                return SLEEP_COLOR;
            } else if (duration >= settings.napLength * 60 + AUTO_OFF_TIME_SECONDS) {
                resetSleep();
                return OFF_COLOR;
            } else {
                return OK_COLOR;
            }
            break;
        case SLEEP:
            struct tm timeinfo;
            if (!getLocalTime(&timeinfo)){
                log_e("Couldn't get time");
                return OFF_COLOR;
            }
            //This assumes midnight is a sleeping time
            if (isAfter(settings.napsBeforeTime, &timeinfo)) {
                //This covers sleep time to midnight
                return SLEEP_COLOR;
            } else if (isBefore(settings.sleepWarnTime, &timeinfo)) {
                return SLEEP_COLOR;
            } else if (isAfter(settings.sleepWarnTime, &timeinfo) && isBefore(settings.sleepOKTime, &timeinfo)){
                return WARN_COLOR;
            } else if (isAfter(settings.sleepOKTime + AUTO_OFF_TIME_SECONDS / 60, &timeinfo)){
                resetSleep();
                return OFF_COLOR;
            } else {
                return OK_COLOR;
            }
    }
}

void startSleep() {
    Serial.println("Mode" + modeToString(mode));
    if (mode != OFF) {
        return;
    }
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)){
        log_e("Couldn't get local time, aborting");
        pulse({50, 0, 0});
        return;
    }
    if (isBefore(settings.napsBeforeTime, &timeinfo)){
        mode = NAP;
        sleepStart = mktime(&timeinfo)
        Serial.println("Mode" + modeToString(mode));
        pulse({40, 40, 0});
    } else {
        mode = SLEEP;
        sleepStart = mktime(&timeinfo)
        Serial.println("Mode" + modeToString(mode));
        pulse({0, 0, 50});
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
