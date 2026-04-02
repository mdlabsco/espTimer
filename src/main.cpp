#include <Arduino.h>
#include <ESP32Encoder.h>
#include <MD_MAX72xx.h>
#include <SPI.h>

// --- Pins ---
#define CLK_PIN   12
#define DATA_PIN  11
#define CS_PIN    10
#define ROT_CLK    1
#define ROT_DT     2
#define ROT_SW     3

// --- Display ---
// Physical layout: 32 rows (tall) x 16 cols (wide)
// Two 4-in-1 MAX7219 strips, side by side, daisy-chained → 8 devices total.
//
// Wiring: ESP32 → Bottom-Left → up → Top-Left → Top-Right → down → Bottom-Right
// MD_MAX72XX device 0 = LAST in chain = Bottom-Right
//
// Empirical mapping (derived from display symptoms):
//   LEFT  physical (cols 0–7 ): devices 3,2,1,0  top→bottom  dev = 3 - block
//   RIGHT physical (cols 8–15): devices 4,5,6,7  top→bottom  dev = 4 + block
//                                BUT rows are inverted (module physically upside-down)
//                                so row_in_dev = 7 - (phys_row % 8) for right strip

#define MAX_DEVICES  8
// Left strip  top→bottom: device = 4 + block
// Right strip top→bottom: device = 3 - block

MD_MAX72XX mx = MD_MAX72XX(MD_MAX72XX::GENERIC_HW, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
ESP32Encoder encoder;

// --- State ---
enum State { SETTING, RUNNING, FINISHED };
State currentState = SETTING;
int  targetMinutes = 5;
long startTime     = 0;
long durationMs    = 0;

// ── Low-level helpers ────────────────────────────────────────────────────────

// Fill (or clear) all 16 columns of one physical row.
void setPhysRow(int phys_row, bool on) {
    int block      = phys_row / 8;          // which 8-row band (0=top … 3=bottom)
    int row_l      = phys_row % 8;          // left  strip: correct orientation
    int row_r      = 7 - (phys_row % 8);   // right strip: rows inverted (module upside-down)
    uint8_t val    = on ? 0xFF : 0x00;
    mx.setRow(3 - block, row_l, val);       // left  physical: devices 3,2,1,0 top→bottom
    mx.setRow(4 + block, row_r, val);       // right physical: devices 4,5,6,7 top→bottom (inverted rows)
}

// Set one physical pixel (row 0–31, col 0–15).
void setPhysPx(int phys_row, int phys_col, bool on) {
    int block      = phys_row / 8;
    if (phys_col < 8) {
        // Left physical: hardware mirror → reverse col within device
        int col_in_dev = 7 - (phys_col % 8);
        int dev        = 3 - block;
        mx.setPoint(phys_row % 8,       dev * 8 + col_in_dev, on);
    } else {
        // Right physical: module is 180° rotated → rows inverted (already handled),
        // columns are also naturally flipped by rotation → no extra reversal needed
        int col_in_dev = phys_col % 8;
        int dev        = 4 + block;
        mx.setPoint(7 - (phys_row % 8), dev * 8 + col_in_dev, on);
    }
}

// ── 5×7 digit font ──────────────────────────────────────────────────────────
// Each row: 5 bits, bit4 = leftmost column, bit0 = rightmost.
static const uint8_t FONT[10][7] = {
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, // 0
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, // 1
    {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}, // 2
    {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}, // 3
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, // 4
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, // 5
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, // 6
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, // 7
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, // 8
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, // 9
};

// Draw one digit (5w × 7h) at physical (start_row, start_col).
void drawDigit(int d, int start_row, int start_col) {
    for (int r = 0; r < 7; r++) {
        uint8_t bits = FONT[d][r];
        for (int c = 0; c < 5; c++) {
            setPhysPx(start_row + r, start_col + c, (bits >> (4 - c)) & 1);
        }
    }
}

// Draw a 2-digit number in the top 8 rows.
// Layout (16 cols): [1][tens:5w][1][gap][1][units:5w][1]
//   tens  → cols 1–5   units → cols 9–13
void drawNumber(int num) {
    for (int r = 0; r < 8; r++) setPhysRow(r, false); // clear top area
    drawDigit((num / 10) % 10, 0, 1);
    drawDigit( num % 10,       0, 9);
}

// ── Bar animation (rows 8–31 = 24 rows) ─────────────────────────────────────
// val / max_val → bar fills proportionally from bottom up.
void drawBar(long val, long max_val) {
    int h = (max_val > 0) ? (int)map(val, 0, max_val, 0, 24) : 0;
    h = constrain(h, 0, 24);
    for (int r = 8; r <= 31; r++) {
        setPhysRow(r, r > (31 - h));
    }
}

// ── Combined redraw ──────────────────────────────────────────────────────────
void drawUI(int number, long bar_val, long bar_max, bool show_number) {
    mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
    if (show_number) drawNumber(number);
    else             for (int r = 0; r < 8; r++) setPhysRow(r, false);
    drawBar(bar_val, bar_max);
    mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
    mx.update();
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    mx.begin();
    mx.control(MD_MAX72XX::INTENSITY, 8); // 0–15
    mx.clear();

    encoder.attachHalfQuad(ROT_CLK, ROT_DT);
    encoder.setCount(targetMinutes * 2);

    pinMode(ROT_SW, INPUT_PULLUP);

    drawUI(targetMinutes, targetMinutes, 60, true);
    Serial.println("Timer Ready");
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
    if (currentState == SETTING) {
        int newMins = (int)(encoder.getCount() / 2);
        if (newMins < 1)  { encoder.setCount(2);   newMins = 1; }
        if (newMins > 60) { encoder.setCount(120);  newMins = 60; }

        if (newMins != targetMinutes) {
            targetMinutes = newMins;
            drawUI(targetMinutes, targetMinutes, 60, true);
            Serial.printf("Set to: %d min\n", targetMinutes);
        }

        if (digitalRead(ROT_SW) == LOW) {
            durationMs   = (long)targetMinutes * 60000L;
            startTime    = millis();
            currentState = RUNNING;
            Serial.println("Timer Started!");
            delay(300);
        }
    }
    else if (currentState == RUNNING) {
        long elapsed   = millis() - startTime;
        long remaining = durationMs - elapsed;

        if (remaining <= 0) {
            currentState = FINISHED;
        } else {
            // Show minutes while >= 60 sec remain; switch to seconds in final minute.
            bool showMins  = (remaining >= 60000);
            int  displayNum = showMins ? (int)(remaining / 60000) + 1
                                       : (int)(remaining / 1000);
            drawUI(displayNum, remaining, durationMs, true);
        }
    }
    else if (currentState == FINISHED) {
        static bool flash = false;
        mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
        mx.clear();
        if (flash) {
            for (int r = 0; r <= 31; r++) setPhysRow(r, true);
        }
        mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
        mx.update();
        flash = !flash;
        delay(500);

        if (digitalRead(ROT_SW) == LOW) {
            currentState = SETTING;
            encoder.setCount(targetMinutes * 2);
            drawUI(targetMinutes, targetMinutes, 60, true);
            delay(300);
        }
    }
}