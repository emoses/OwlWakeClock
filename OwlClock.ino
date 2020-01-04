#include "secrets.h"
#include <WiFi.h>
#include "time.h"

time_t this_second = 0;
time_t last_second = 0;

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



void setup() {
    Serial.begin(115200);
    delay(10);

    pinMode(2, OUTPUT);
    wifiConnect();
    configTime(-7 * 3600, 0, "pool.ntp.org");
    while(!this_second) {
        time(&this_second);
        Serial.print("-");
        delay(100);
    }
    Serial.print(ctime(&this_second));
}

void loop() {
  // put your main code here, to run repeatedly:
    digitalWrite(2, HIGH);
    time(&this_second); // until time is set, it remains 0
    if (this_second != last_second) {
        last_second = this_second;
        if ((this_second % 60) == 0) {
            Serial.print(ctime(&this_second));
        }
    }
    delay(500);
    digitalWrite(2, LOW);
    delay(500);
}
