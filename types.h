#include "color.h"

typedef color_ColorRGB RGB;
typedef color_ColorHSV HSV;

bool operator==(const RGB& r1, const RGB& r2) {
    return r1.r == r2.r && r1.g == r2.g && r1.b == r2.b;
}

bool operator==(const HSV& r1, const HSV& r2) {
    return r1.h == r2.h && r1.s == r2.s && r1.v == r2.v;
}

enum Mode_t {
    OFF,
    NAP,
    SLEEP
};

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
