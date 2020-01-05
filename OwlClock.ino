#include "secrets.h"
#include <WiFi.h>
#include "time.h"
#include <ESPmDNS.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <Preferences.h>

time_t this_second = 0;
time_t last_second = 0;

const char* MDNS_NAME = "owlclock";

#define BUTTON_PIN 34

#define RED_PIN 27
#define GREEN_PIN 26
#define BLUE_PIN 25
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

struct WakeTime_t {
    time_t warn;
    time_t OK;
};

struct RGB {
    uint8_t R;
    uint8_t G;
    uint8_t B;
};

typedef enum {
    OFF,
    NAP,
    SLEEP
} Mode_t;

typedef struct {
    //How long a nap is, in minutes
    uint16_t napLength;
    //If it's before this time of day, it's a nap
    uint16_t napsBeforeTime;
    //This time of day is when we go from sleep to warn
    uint16_t sleepWarnTime;
    //This time of day is when we go from warn to OK
    uint16_t sleepOKTime;
    //Brightness
    uint8_t brightness;
} settings_t  __attribute__ ((packed));

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
    uint16_t minute = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    return minuteOfDay < minute;
}

bool isAfter(uint16_t minuteOfDay, struct tm* timeinfo) {
    uint16_t minute = timeinfo.tm_hour * 60 + timeinfo.tm_min;
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

//Globals
WakeTime_t wakeTime = {0, 0};
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

void setup() {
    Serial.begin(115200);
    delay(10);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    setupLedPins();
    setColor(OFF_COLOR);

    wifiConnect();
    configTzTime(TZ_STR, "pool.ntp.org");
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
            time_t now;
            time(&now);
            wakeTime.warn = now + 60;
            wakeTime.OK = wakeTime.warn + 60;
            String content = "<html<head><title>Timer set</title></head>";
            content += "<body><h1>OK!</h1><p>Timer set!</p><p>Warn time: ";
            content += ctime(&wakeTime.warn);
            content += "<br/>Wake time: ";
            content += ctime(&wakeTime.OK);
            content += "</p></body></html>";
            server.send(200, "text/html", content);
        });

}

void setColor(uint8_t val, uint8_t pin, uint8_t channel){
    ledcWrite(channel, (val == 0) ? 256 : 255 - val);
}

void setColor(RGB color){
    setColor(color.R, RED_PIN, RED_CHANNEL);
    setColor(color.G, GREEN_PIN, GREEN_CHANNEL);
    setColor(color.B, BLUE_PIN, BLUE_CHANNEL);
}

void startSleep() {
    if (mode != OFF) {
        return;
    }
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)){
        log_e("Couldn't get local time, aborting");
        //TODO: Flash red
    }
    if (isBefore(settings.napsBeforeTime, &timeinfo)){
        mode = NAP;
        //set waketime.OK
    } else {
        mode = SLEEP;
    }
}


void loop() {
    server.handleClient();

    time(&this_second);
    if (wakeTime.OK && this_second > wakeTime.OK) {
        setColor(OK_COLOR);
    } else if (wakeTime.warn && this_second > wakeTime.warn) {
        setColor(WARN_COLOR);
    } else if (wakeTime.warn || wakeTime.OK) {
        setColor(SLEEP_COLOR);
    } else {
        setColor(OFF_COLOR);
    }
    //Don't need to be a space heater
    delay(25);
}
