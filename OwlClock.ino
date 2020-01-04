#include "secrets.h"
#include <WiFi.h>
#include "time.h"
#include <ESPmDNS.h>
#include <WiFiClient.h>
#include <WebServer.h>

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

const RGB OFF_COLOR = {0, 0, 0};
const RGB SLEEP_COLOR = {30, 0, 0};
const RGB WARN_COLOR = {30, 30, 0};
const RGB OK_COLOR = {0, 30, 0};

WakeTime_t wakeTime = {0, 0};

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

void setup() {
    Serial.begin(115200);
    delay(10);

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    setupLedPins();
    setColor(OFF_COLOR);

    wifiConnect();
    configTime(-7 * 3600, 0, "pool.ntp.org");
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
            server.send(200, "text/html", "<html><head><title>Hoo!</title></head>"
                        "<body><h1>Hoo!</h1><form action=\"start\" method=\"post\">"
                        "<input type=\"submit\" value=\"Start (1 hour)\" />"
                        "</body></html>");
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
