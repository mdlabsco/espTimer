#include <Arduino.h>
#include <ESP32Encoder.h>
#include <MD_MAX72xx.h>
#include <SPI.h>

// ── Pins ─────────────────────────────────────────────────────────────────────
#define CLK_PIN   12
#define DATA_PIN  11
#define CS_PIN    10
#define ROT_CLK    1
#define ROT_DT     2
#define ROT_SW     3

// ── Display ───────────────────────────────────────────────────────────────────
#define MAX_DEVICES 8
MD_MAX72XX mx = MD_MAX72XX(MD_MAX72XX::GENERIC_HW, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);
ESP32Encoder encoder;

// ── State Machine ─────────────────────────────────────────────────────────────
enum State { SETTING, RUNNING, PAUSED, FINISHED };
State currentState = SETTING;

// ── Timer ─────────────────────────────────────────────────────────────────────
int           targetMinutes = 10;
long          startTime     = 0;
long          durationMs    = 0;
long          pauseOffset   = 0;
long          pausedAt      = 0;

// ── Adaptive Encoder ──────────────────────────────────────────────────────────
long          encPrevCount = 0;
unsigned long encPrevMs    = 0;

// ── Button ────────────────────────────────────────────────────────────────────
#define BTN_DEBOUNCE_MS 50
#define BTN_LONG_MS     800
bool          btnHeld      = false;
bool          btnLongFired = false;
unsigned long btnPressTime = 0;

// ── Finish Animation State (global so it can be reset) ────────────────────────
int           finWipeRow  = 0;
int           finWipePh   = 0;   // 0=fill rows, 1=clear rows
int           finWipeLps  = 0;
bool          finWipeDone = false;
unsigned long finLastMs   = 0;
int           finFlashVal = 8;
int           finFlashDir = 1;
unsigned long finFlashMs  = 0;

// ══ Low-level pixel helpers ══════════════════════════════════════════════════
void setPhysRow(int r, bool on) {
    int blk = r / 8, rem = r % 8;
    uint8_t v = on ? 0xFF : 0x00;
    mx.setRow(3 - blk, rem,     v);
    mx.setRow(4 + blk, 7 - rem, v);
}

void setPhysPx(int r, int c, bool on) {
    int blk = r / 8;
    if (c < 8) {
        mx.setPoint(r % 8,       (3 - blk) * 8 + (7 - c % 8), on);
    } else {
        mx.setPoint(7 - r % 8,   (4 + blk) * 8 + (c % 8),     on);
    }
}

// ══ 5×7 Font ════════════════════════════════════════════════════════════════
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

void drawDigit(int d, int sr, int sc) {
    for (int r = 0; r < 7; r++) {
        uint8_t b = FONT[d][r];
        for (int c = 0; c < 5; c++)
            setPhysPx(sr + r, sc + c, (b >> (4 - c)) & 1);
    }
}

// Supports 00–99
void drawNumber(int n) {
    for (int r = 0; r < 8; r++) setPhysRow(r, false);
    drawDigit((n / 10) % 10, 0, 1);
    drawDigit( n % 10,       0, 9);
}

// ══ Hourglass (rows 8–31, 24 rows) ══════════════════════════════════════════
// HG_HW[i] = half-pixel-width at row (8+i), centred at col 8.
// Top half (i=0–11): narrows toward neck. Bottom (i=12–23): widens from neck.
static const int8_t HG_HW[24] = {
    8, 7, 6, 5, 4, 4, 3, 2, 2, 1, 1, 0,  // top
    0, 1, 1, 2, 2, 3, 4, 4, 5, 6, 7, 8   // bottom
};

// progress: 0.0 = start (top full, bottom empty)
//           1.0 = done  (top empty, bottom full)
void drawHourglass(float progress) {
    progress = constrain(progress, 0.0f, 1.0f);
    int topFilled = lroundf((1.0f - progress) * 12.0f);
    int botFilled = lroundf(progress           * 12.0f);

    // TOP HALF (i=0..11, rows 8..19): sand level DROPS from the top.
    // Wide rows at the top empty first; narrow rows near the neck remain last.
    // lit = rows i=(12-topFilled)..11  →  as topFilled shrinks, top rows go empty.
    for (int i = 0; i < 12; i++) {
        int row = 8 + i;
        int hw  = HG_HW[i];
        bool lit = (i >= (12 - topFilled)) && (hw > 0);
        if (lit) {
            for (int c = 0; c < 16; c++)
                setPhysPx(row, c, c >= (8 - hw) && c <= (7 + hw));
        } else {
            setPhysRow(row, false);
        }
    }

    // BOTTOM HALF (i=12..23, rows 20..31): sand PILES UP from the floor.
    // Fill from i=23 (row 31) upward — like real sand accumulating at the bottom.
    for (int i = 12; i < 24; i++) {
        int row = 8 + i;
        int hw  = HG_HW[i];
        bool lit = (i >= (24 - botFilled)) && (hw > 0);
        if (lit) {
            for (int c = 0; c < 16; c++)
                setPhysPx(row, c, c >= (8 - hw) && c <= (7 + hw));
        } else {
            setPhysRow(row, false);
        }
    }

    // FALLING GRAIN: animated pixel drops from neck to the top of the sand pile.
    // Only shown while timer is actively running (progress > 0 and top has sand).
    if (progress > 0.0f && progress < 1.0f && topFilled > 0) {
        int neckRow    = 19;                 // bottom of top half (hw=0 rows)
        int pileFirst  = 24 - botFilled;     // i-index of first pile row from bottom
        int pileTopRow = 8 + pileFirst;      // physical row where pile starts
        int grainEnd   = pileTopRow - 1;     // last empty row above the pile
        grainEnd = constrain(grainEnd, neckRow, 31);

        int fallDist = grainEnd - neckRow + 1;
        if (fallDist > 1) {
            // Period scales with distance so grain appears faster when pile is full
            unsigned long period = (unsigned long)fallDist * 45 + 80;
            float frac    = (float)(millis() % period) / (float)period;
            int grainRow  = neckRow + (int)(frac * fallDist);
            grainRow      = constrain(grainRow, neckRow, grainEnd);
            // Draw 2-px wide grain through center columns (matches neck opening)
            setPhysPx(grainRow, 7, true);
            setPhysPx(grainRow, 8, true);
        }
    }
}

// ══ Finish Animation ════════════════════════════════════════════════════════
// Phase 1: Cascade wipe (fill all rows top→bottom, clear top→bottom) × 3
// Phase 2: Show "00" with pulsing brightness

void resetFinishAnim() {
    finWipeRow  = 0;
    finWipePh   = 0;
    finWipeLps  = 0;
    finWipeDone = false;
    finLastMs   = 0;
    finFlashVal = 8;
    finFlashDir = 1;
    finFlashMs  = 0;
    mx.control(MD_MAX72XX::INTENSITY, 15);
}

void tickFinishAnim() {
    unsigned long now = millis();

    if (!finWipeDone) {
        if (now - finLastMs < 40) return;
        finLastMs = now;

        mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
        if (finWipePh == 0) {
            setPhysRow(finWipeRow++, true);
            if (finWipeRow >= 32) { finWipeRow = 0; finWipePh = 1; }
        } else {
            setPhysRow(finWipeRow++, false);
            if (finWipeRow >= 32) {
                finWipeRow = 0; finWipePh = 0;
                if (++finWipeLps >= 3) {
                    finWipeDone = true;
                    mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
                    mx.clear();
                    drawNumber(0);
                    mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
                    mx.update();
                    mx.control(MD_MAX72XX::INTENSITY, 8);
                    return;
                }
            }
        }
        mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
        mx.update();
    } else {
        // Pulse brightness on "00"
        if (now - finFlashMs < 25) return;
        finFlashMs   = now;
        finFlashVal += finFlashDir;
        if (finFlashVal >= 14 || finFlashVal <= 2) finFlashDir = -finFlashDir;
        mx.control(MD_MAX72XX::INTENSITY, finFlashVal);
    }
}

// ══ Button (short / long press detection) ════════════════════════════════════
// Returns: 1 = short press released, 2 = long press fired, 0 = nothing
int readButton() {
    bool pressed = (digitalRead(ROT_SW) == LOW);
    unsigned long now = millis();

    if (pressed && !btnHeld) {
        btnHeld      = true;
        btnLongFired = false;
        btnPressTime = now;
    } else if (pressed && btnHeld && !btnLongFired) {
        if (now - btnPressTime >= BTN_LONG_MS) {
            btnLongFired = true;
            return 2;  // long press event
        }
    } else if (!pressed && btnHeld) {
        btnHeld = false;
        if (!btnLongFired && (now - btnPressTime >= BTN_DEBOUNCE_MS))
            return 1;  // short press event
    }
    return 0;
}

// ══ Combined draw ════════════════════════════════════════════════════════════
void drawUI(int number, float hgProgress) {
    mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::OFF);
    drawNumber(number);
    drawHourglass(hgProgress);
    mx.control(MD_MAX72XX::UPDATE, MD_MAX72XX::ON);
    mx.update();
}

// ══ Setup ════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);

    mx.begin();
    mx.control(MD_MAX72XX::INTENSITY, 8);
    mx.clear();

    encoder.attachHalfQuad(ROT_CLK, ROT_DT);
    encoder.setCount(targetMinutes * 2);
    encPrevCount = targetMinutes * 2;
    encPrevMs    = millis();

    pinMode(ROT_SW, INPUT_PULLUP);

    drawUI(targetMinutes, 0.0f);
    Serial.println("Ready | Spin=set | Press=start | Hold=reset");
}

// ══ Loop ═════════════════════════════════════════════════════════════════════
void loop() {
    int btn = readButton();

    // ── SETTING ──────────────────────────────────────────────────────────────
    if (currentState == SETTING) {

        // Adaptive speed via fixed 80 ms sampling window.
        // Accumulate counts over the window; x1 for a single detent, x5 for 2+.
        // encoder.setCount() fires once per window — never mid-detent.
        unsigned long nowMs = millis();
        if (nowMs - encPrevMs >= 50) {
            long curCount = encoder.getCount();
            long delta    = curCount - encPrevCount;
            int  detents  = abs((int)(delta / 2));   // full detents this window

            if (delta != 0) {
                int mult = (detents >= 3) ? 5 : 1;   // fast spin → x5, slow → x1

                int step    = (int)(delta / 2) * mult;
                int newMins = constrain(targetMinutes + step, 1, 99);

                if (newMins != targetMinutes) {
                    targetMinutes = newMins;
                    drawUI(targetMinutes, 0.0f);
                    Serial.printf("Set: %d min [x%d]\n", targetMinutes, mult);
                }
                encoder.setCount(targetMinutes * 2);
                encPrevCount = targetMinutes * 2;
            }
            encPrevMs = nowMs;
        }

        if (btn == 1) {  // short press → start
            durationMs   = (long)targetMinutes * 60000L;
            startTime    = millis();
            pauseOffset  = 0;
            currentState = RUNNING;
            Serial.printf("Started! %d min\n", targetMinutes);
        }
    }

    // ── RUNNING ───────────────────────────────────────────────────────────────
    else if (currentState == RUNNING) {
        long elapsed   = millis() - startTime - pauseOffset;
        long remaining = durationMs - elapsed;

        if (remaining <= 0) {
            currentState = FINISHED;
            resetFinishAnim();
            Serial.println("Time's up!");
        } else {
            // Show minutes until last 60 s, then switch to seconds
            bool showMins   = (remaining >= 60000);
            int  displayNum = showMins ? (int)(remaining / 60000) + 1
                                       : (int)(remaining / 1000);
            float progress  = 1.0f - (float)remaining / (float)durationMs;
            drawUI(displayNum, progress);
        }

        if (btn == 1) {   // short press → pause
            pausedAt     = millis();
            currentState = PAUSED;
            Serial.println("Paused");
        }
    }

    // ── PAUSED ────────────────────────────────────────────────────────────────
    else if (currentState == PAUSED) {
        // Pulse brightness to show frozen state
        static unsigned long pulseMs  = 0;
        static int           pulseVal = 8;
        static int           pulseDir = 1;
        if (millis() - pulseMs > 30) {
            pulseMs   = millis();
            pulseVal += pulseDir;
            if (pulseVal >= 12 || pulseVal <= 3) pulseDir = -pulseDir;
            mx.control(MD_MAX72XX::INTENSITY, pulseVal);
        }

        if (btn == 1) {   // short press → resume
            pauseOffset  += millis() - pausedAt;
            currentState  = RUNNING;
            mx.control(MD_MAX72XX::INTENSITY, 8);
            Serial.println("Resumed");
        }
        if (btn == 2) {   // long press → reset to SETTING
            currentState = SETTING;
            pauseOffset  = 0;
            encoder.setCount(targetMinutes * 2);
            encPrevCount = targetMinutes * 2;
            encPrevMs    = millis();
            mx.control(MD_MAX72XX::INTENSITY, 8);
            drawUI(targetMinutes, 0.0f);
            Serial.println("Reset");
        }
    }

    // ── FINISHED ─────────────────────────────────────────────────────────────
    else if (currentState == FINISHED) {
        tickFinishAnim();

        if (btn == 1) {   // short press → back to SETTING
            currentState = SETTING;
            encoder.setCount(targetMinutes * 2);
            encPrevCount = targetMinutes * 2;
            encPrevMs    = millis();
            mx.control(MD_MAX72XX::INTENSITY, 8);
            drawUI(targetMinutes, 0.0f);
            Serial.println("Reset to Setting");
        }
    }
}