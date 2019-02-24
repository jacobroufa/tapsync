/**
 * Attribution:
 *  http://fuzzysynth.blogspot.com/2015/06/digitech-jam-man.html
 *  https://github.com/Calde-github/Looperino/blob/master/Looper.ino
 *  http://freestompboxes.org/viewtopic.php?p=271340#p271340
 *
 * Huge thanks to the above for the invaluable resource of decoding the JamSync signal
 */

#define DEBUG false

#define OPEN HIGH
#define CLOSED LOW

#define ON HIGH
#define OFF LOW

// JamSync signal packets
const byte JM_LINK[] {0xF0, 0x00, 0x00, 0x10, 0x7F, 0x62, 0x01, 0x00, 0x01, 0x01, 0xF7};
const byte JM_SYNC[] {0xF0, 0x00, 0x00, 0x10, 0x7F, 0x62, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x02, 0x04, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7};
const byte JM_STOP[] {0x00, 0xF0, 0x00, 0x00, 0x10, 0x7F, 0x62, 0x01, 0x4A, 0x01, 0x04, 0x46, 0x30, 0x42, 0x02, 0x04, 0x40, 0x03, 0x4E, 0x46, 0x2E, 0x40, 0x04, 0x5C, 0xF7};

// Config
const HardwareSerial *IO = &Serial;
const int BUTTON_PIN = 2;
const int LED_PIN = 3;

const int DEBOUNCE = 40;

const int ACTIVE_THRESHOLD = 2000;
const int CLEAR_THRESHOLD = 4000;

const int MIN_TAPS = 2;

const int MAX_TAP = 2000;

const int LED_DURATION = 80;

const int JM_LINK_PERIOD = 400;

const long MS_PER_MINUTE = 60000; // 1000ms/s, 60s/min

const int BEATS_PER_MEASURE = 4;

volatile bool active = false;
volatile bool activePressed = false;
volatile bool clearedOnce = false;
volatile bool previousButtonState = false;

volatile int lit = 0;
volatile byte pressed = OPEN;

volatile long beatMs = 0;
volatile long bpm = 0;
volatile int quarterCount = 0;

volatile int tapCount = 0;

volatile long holdTime = 0;
volatile long lastTapTime = 0;
volatile long prevBpmLoop = 0;
volatile long prevLinkLoop = 0;
volatile long prevTapLoop = 0;

// Utility Methods
// Starts and stops a loop
void activate() {
  active = !active;
  activePressed = true;

  // was just deactivated, we should stop the loop
  if (!active && !DEBUG) {
    IO->write(JM_STOP, sizeof(JM_STOP));
  }
}

// Clears the currently set BPM and deactivates the loop
void clear() {
  active = false;
  beatMs = 0;
  bpm = 0;
  clearedOnce = true;
  quarterCount = 0;
  tapCount = 0;

  if (!DEBUG) {
    IO->write(JM_STOP, sizeof(JM_STOP));
  }
}

bool getLongHold() {
  return pressed == CLOSED && holdTime >= CLEAR_THRESHOLD;
}

bool getShortHold() {
  return pressed == CLOSED && holdTime > ACTIVE_THRESHOLD && holdTime < CLEAR_THRESHOLD;
}

void syncSend() {
   if (DEBUG) {
     IO->println("sending sync packet with new bpm: " + String(bpm));
     return;
   }

   // Copy base JM sync packet
   int syncPacketSize = sizeof(JM_SYNC);
   byte syncPacket[syncPacketSize];
   for (int i = 0; i < syncPacketSize; i++) syncPacket[i] = JM_SYNC[i];

   // BPM
   syncPacket[7] = 66 + 8 * ((63 < bpm) && (bpm < 128) || bpm > 191) ;
   syncPacket[11] = (4 * bpm > 127 && 4 * bpm < 256) * (4 * bpm - 128) +
                    (2 * bpm > 127 && 2 * bpm < 256) * (2 * bpm - 128) +
                    (bpm > 127 && bpm < 256) * (bpm - 128);
   syncPacket[12] = 1 * (bpm > 127) + 66;

   // Measure length
   unsigned long loopTime = beatMs * BEATS_PER_MEASURE;
   int x = floor(log(loopTime / 2000.0) / log(4.0));
   int b163 = (loopTime / (2000.0 * pow(4.0, x))) > 2;
   int y = 2 * pow(2, b163) * pow(4, x);
   int w = floor(loopTime / y);
   syncPacket[15] = 64 + 8 * b163;
   syncPacket[20] = 64 + x;
   syncPacket[19] = 128 * (0.001 * w - 1);
   syncPacket[18] = pow(128.0, 2) * (0.001 * w - 1 - syncPacket[19] / 128.0);
   syncPacket[17] = pow(128.0, 3) * (0.001 * w - 1 - syncPacket[19] / 128.0 - syncPacket[18] / pow(128.0, 2));

   // Command (SYNC)
   syncPacket[21] = 5;

   // Checksum XOR
   byte z = 0;
   for (int i = 7; i < 22; i++) z = z ^ syncPacket[i];
   syncPacket[22] = z;

   IO->write(syncPacket, syncPacketSize);
}

// Assigns a new BPM value
void setBpm(long lastTime, long currentTime) {
  const long beatMillis = currentTime - lastTime;
  const long newBpm = floor(MS_PER_MINUTE / beatMillis);

  beatMs = beatMillis;
  bpm = newBpm;

  if (!active) {
    active = true;
  }

  syncSend();
}

// Lifecycle
void blinkLED() {
  if (active && (millis() - prevBpmLoop) < LED_DURATION) {
    if (quarterCount == 0) {
      lit = 255;
    } else {
      lit = 10;
    }
  } else if (pressed == CLOSED) {
    lit = 255;
  } else if (getShortHold()) {
    lit = 40;
  } else if (getLongHold()) {
    lit = 20;
  } else {
    lit = 0;
  }

  analogWrite(LED_PIN, lit);
}

void bpmSend() {
  if (!active) return;

  const long now = millis();

  if (now - prevBpmLoop < beatMs) return;

  quarterCount = (quarterCount + 1) % BEATS_PER_MEASURE;

  if (quarterCount == 0) syncSend();

  prevBpmLoop = now;
}

void handleButtonInput() {
  const long now = millis();

  if (now - prevTapLoop < DEBOUNCE) return;

  const bool currentButtonState = digitalRead(BUTTON_PIN);

  pressed = currentButtonState;
  prevTapLoop = now;

  if (previousButtonState != currentButtonState) {
    activePressed = false;
    clearedOnce = false;
    previousButtonState = currentButtonState;

    if (pressed == CLOSED) {
      const bool resetTaps = now - lastTapTime > MAX_TAP && tapCount > 0;

      tapCount = resetTaps ? 0 : tapCount + 1;

      if (tapCount > MIN_TAPS) {
        setBpm(lastTapTime, now);
      }

      lastTapTime = now;
    }
  } else {
    if (pressed == CLOSED) {
      holdTime = now - lastTapTime;

      if (getShortHold() && !activePressed) {
        if (DEBUG) IO->println("activating/deactivating");

        activate();
      }

      if (getLongHold() && !clearedOnce) {
        if (DEBUG) IO->println("clearing");

        clear();
      }
    }
  }
}

void linkMaintain() {
  const long now = millis();

  if (now - prevLinkLoop < JM_LINK_PERIOD) return;

  if (DEBUG) {
    IO->println("maintaining JM link");
  } else {
    IO->write(JM_LINK, sizeof(JM_LINK));
  }

  prevLinkLoop = millis();
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  analogWrite(LED_PIN, lit);

  pinMode(BUTTON_PIN, INPUT);

  IO->begin(DEBUG
      ? 9600 // USB serial debug rate
      : 31250); // JamSync serial rate

  linkMaintain();
}

void loop() {
  // handle raw button input, debounced to ~100ms
  handleButtonInput();

  // blink LEDs with bpm
  blinkLED();

  // send a JamSync signal link every ~400ms
  linkMaintain();

  // send sync packet with BPM every measure (4x bpm)
  bpmSend();
}
