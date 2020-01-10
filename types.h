struct RGB {
    uint8_t R;
    uint8_t G;
    uint8_t B;
};

bool operator==(const RGB& r1, const RGB& r2) {
    return r1.R == r2.R && r1.G == r2.G && r1.B == r2.B;
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
