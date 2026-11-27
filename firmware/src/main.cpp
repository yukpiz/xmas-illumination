// イルミネーション制御
//
// 40球のイルミは LED が半分ずつ逆向き（逆並列）につながっているため、
// 電気の向きを高速で入れ替えて両方の LED を光らせる。
// 3セットは同じ光り方にするので、1チャンネルにまとめてつなぐ。
//
// 今は ESP32 の GPIO 2本を H ブリッジの代わりにして、逆向きに並べた LED 2個で試す。
// TB6612FNG が届いたら、PIN_A / PIN_B を AIN1 / AIN2 につなぎ替え、
// PWMA と STBY を HIGH にすれば、同じ考え方でそのまま動く。
//
// シリアルモニター（115200bps）からの操作:
//   1: 全点灯  2: 交互点滅  3: 全点滅  4: ふわっと  5: 追いかけ
//   6: ろうそく  7: きらめき  8: ハートビート  9: フラッシュ
//   a: だんだん速く  b: 自動めぐり  0: 消灯
//   +: 明るく  -: 暗く
//
// ボタン（GPIO27 と GND の間のタクトスイッチ）を短く押すたびに、
// 全点灯 → … → だんだん速く → 自動めぐり → 全点灯 の順に切り替わる。
// 長押し（0.8秒）で停止、停止中に押すと最後のパターンで再開する。
// 最後に選んだパターンは保存され、次に電源を入れたときもそのパターンで光る。

#include <Arduino.h>
#include <Preferences.h>

const int PIN_A = 25;    // A→B の向きで LED1 が光る
const int PIN_B = 26;    // B→A の向きで LED2 が光る
const int PIN_BTN = 27;  // タクトスイッチ（押すと LOW）

// ボタンのチャタリング（押した瞬間の信号のばたつき）を無視する時間
const uint32_t DEBOUNCE_MS = 30;
// これ以上押し続けたら長押し（停止／再開）
const uint32_t LONG_PRESS_MS = 800;

// 向きを入れ替える周期（マイクロ秒）。5000us = 1秒に200回、ちらつきは見えない
const uint32_t CYCLE_US = 5000;
const uint32_t HALF_US = CYCLE_US / 2;

// 自動めぐりで1つのパターンを続ける時間
const uint32_t AUTO_INTERVAL_MS = 20000;

enum Mode {
  OFF = 0,
  ALL_ON,
  ALTERNATE,
  BLINK,
  BREATH,
  CHASE,
  CANDLE,
  SPARKLE,
  HEARTBEAT,
  FLASH,
  ACCEL,
  AUTO,
  MODE_COUNT
};
const char *MODE_NAMES[] = {"消灯",     "全点灯", "交互点滅",     "全点滅",     "ふわっと",     "追いかけ",
                            "ろうそく", "きらめき", "ハートビート", "フラッシュ", "だんだん速く", "自動めぐり"};
const char MODE_KEYS[] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b'};

Mode mode = ALL_ON;
int brightness = 100;  // 0〜100（%）

// 自動めぐりの状態
Mode autoCurrent = ALL_ON;
uint32_t autoStartMs = 0;

// 選んだパターンの保存先（ESP32 の中の、電源を切っても消えない領域）
Preferences prefs;

// 電気を流す向き: +1 = A→B、-1 = B→A、0 = 止める
void drive(int dir) {
  if (dir > 0) {
    digitalWrite(PIN_B, LOW);
    digitalWrite(PIN_A, HIGH);
  } else if (dir < 0) {
    digitalWrite(PIN_A, LOW);
    digitalWrite(PIN_B, HIGH);
  } else {
    digitalWrite(PIN_A, LOW);
    digitalWrite(PIN_B, LOW);
  }
}

// 0.0〜1.0 の乱数
float rand01() { return (float)esp_random() / (float)UINT32_MAX; }

// 0〜1 を行き来するなめらかな波（周期 periodMs）
float wave(uint32_t ms, uint32_t periodMs) {
  float t = (ms % periodMs) / (float)periodMs;
  return 0.5f - 0.5f * cosf(t * 2.0f * PI);
}

// ろうそく: 向きごとに、ゆらぐ目標値へ少しずつ近づける
void candle(uint32_t ms, float &a, float &b) {
  static float curA = 0.7f, curB = 0.7f, tgtA = 0.7f, tgtB = 0.7f;
  static uint32_t nextA = 0, nextB = 0, lastMs = 0;
  if (ms >= nextA) {
    tgtA = 0.35f + 0.65f * rand01();
    nextA = ms + 40 + (uint32_t)(rand01() * 120);
  }
  if (ms >= nextB) {
    tgtB = 0.35f + 0.65f * rand01();
    nextB = ms + 40 + (uint32_t)(rand01() * 120);
  }
  float k = min(1.0f, (ms - lastMs) / 60.0f);  // 60ms くらいで追いつく
  lastMs = ms;
  curA += (tgtA - curA) * k;
  curB += (tgtB - curB) * k;
  a = curA;
  b = curB;
}

// きらめき: うっすら光らせておき、ときどき片方がキラッと明るくなってすっと戻る
void sparkle(uint32_t ms, float &a, float &b) {
  static float boostA = 0, boostB = 0;
  static uint32_t lastMs = 0;
  float dt = (ms - lastMs) / 1000.0f;
  lastMs = ms;
  // 1秒あたり平均 1.5 回ずつ、それぞれの向きでキラッとさせる
  if (rand01() < 1.5f * dt) boostA = 1.0f;
  if (rand01() < 1.5f * dt) boostB = 1.0f;
  boostA *= expf(-dt / 0.15f);  // 0.15秒くらいで消える
  boostB *= expf(-dt / 0.15f);
  const float base = 0.15f;
  a = base + (1.0f - base) * boostA;
  b = base + (1.0f - base) * boostB;
}

// ハートビート: 1.2秒ごとに「ドクン、ドクン」と2回光る
void heartbeat(uint32_t ms, float &a, float &b) {
  uint32_t t = ms % 1200;
  float v = 0.0f;
  if (t < 300) {
    v = expf(-(float)t / 70.0f);  // 1回目（強め）
  } else if (t >= 300 && t < 700) {
    v = 0.7f * expf(-(float)(t - 300) / 90.0f);  // 2回目（少し弱め）
  }
  a = b = 0.05f + 0.95f * v;
}

// フラッシュ: 短くパッと光る。2回光ったら向きを入れ替える
void flash(uint32_t ms, float &a, float &b) {
  uint32_t t = ms % 1200;
  bool on = (t % 300) < 40;  // 300ms ごとに 40ms だけ光る
  bool first = t < 600;
  a = (on && first) ? 1.0f : 0.0f;
  b = (on && !first) ? 1.0f : 0.0f;
}

// だんだん速く: 交互点滅の間隔を 600ms → 70ms → 600ms と 12秒かけて変える
void accel(uint32_t ms, float &a, float &b) {
  static bool sideA = true;
  static uint32_t lastToggle = 0;
  float s = wave(ms, 12000);  // 0（ゆっくり）〜1（速い）
  uint32_t interval = (uint32_t)(600 - 530 * s);
  if (ms - lastToggle >= interval) {
    sideA = !sideA;
    lastToggle = ms;
  }
  a = sideA ? 1.0f : 0.0f;
  b = sideA ? 0.0f : 1.0f;
}

// 指定したパターンの、今の時刻での向きごとの明るさ（0.0〜1.0）
void pattern(Mode m, uint32_t ms, float &a, float &b) {
  switch (m) {
    case ALL_ON:
      a = b = 1.0f;
      break;
    case ALTERNATE: {  // 0.5秒ごとに片方ずつ
      bool first = (ms / 500) % 2 == 0;
      a = first ? 1.0f : 0.0f;
      b = first ? 0.0f : 1.0f;
      break;
    }
    case BLINK: {  // 全部いっしょに 0.5秒点灯・0.5秒消灯
      float v = (ms / 500) % 2 == 0 ? 1.0f : 0.0f;
      a = b = v;
      break;
    }
    case BREATH: {  // 3秒で明るくなって暗くなる
      float v = wave(ms, 3000);
      a = b = v * v;  // 目の感じ方に合わせて暗い側をなめらかに
      break;
    }
    case CHASE: {  // 片方がふわっと明るくなる間に、もう片方が暗くなる
      float v = wave(ms, 2000);
      a = v * v;
      b = (1.0f - v) * (1.0f - v);
      break;
    }
    case CANDLE:
      candle(ms, a, b);
      break;
    case SPARKLE:
      sparkle(ms, a, b);
      break;
    case HEARTBEAT:
      heartbeat(ms, a, b);
      break;
    case FLASH:
      flash(ms, a, b);
      break;
    case ACCEL:
      accel(ms, a, b);
      break;
    default:
      a = b = 0.0f;
  }
}

void printStatus() {
  if (mode == AUTO) {
    Serial.printf("モード: %s（いま: %s） / 明るさ: %d%%\n", MODE_NAMES[mode], MODE_NAMES[autoCurrent], brightness);
  } else {
    Serial.printf("モード: %s / 明るさ: %d%%\n", MODE_NAMES[mode], brightness);
  }
}

// 今の時刻での、向きごとの明るさ（明るさの設定も反映）
void levels(uint32_t ms, float &a, float &b) {
  Mode m = mode;
  if (mode == AUTO) {
    if (ms - autoStartMs >= AUTO_INTERVAL_MS) {
      // 全点灯〜だんだん速く を順番にめぐる
      autoCurrent = (Mode)(autoCurrent + 1);
      if (autoCurrent >= AUTO) autoCurrent = ALL_ON;
      autoStartMs = ms;
      printStatus();
    }
    m = autoCurrent;
  }
  pattern(m, ms, a, b);
  float scale = brightness / 100.0f;
  a *= scale;
  b *= scale;
}

void printHelp() {
  Serial.println("1:全点灯 2:交互点滅 3:全点滅 4:ふわっと 5:追いかけ");
  Serial.println("6:ろうそく 7:きらめき 8:ハートビート 9:フラッシュ");
  Serial.println("a:だんだん速く b:自動めぐり 0:消灯 +/-:明るさ");
}

// パターンを切り替える。消灯以外は保存して、次の起動でも使う
void setMode(Mode m) {
  mode = m;
  if (mode == AUTO) {
    autoCurrent = ALL_ON;
    autoStartMs = millis();
  }
  if (mode != OFF && prefs.getUChar("mode", 0) != (uint8_t)mode) {
    prefs.putUChar("mode", (uint8_t)mode);
  }
  printStatus();
}

void handleSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '+') {
      brightness = min(100, brightness + 10);
      printStatus();
      continue;
    }
    if (c == '-') {
      brightness = max(0, brightness - 10);
      printStatus();
      continue;
    }
    for (int i = 0; i < MODE_COUNT; i++) {
      if (c == MODE_KEYS[i]) setMode((Mode)i);
    }
  }
}

// 保存されている最後のパターンで光らせる（停止からの再開用）
void resume() {
  uint8_t saved = prefs.getUChar("mode", ALL_ON);
  setMode((saved > OFF && saved < MODE_COUNT) ? (Mode)saved : ALL_ON);
}

// 短く押して離す: 次のパターンへ（消灯はループに入れない）。停止中なら再開
// 長押し: 停止。停止中なら再開
void handleButton() {
  static bool stable = HIGH;       // ばたつきが収まったあとの状態
  static bool lastRead = HIGH;     // 前回読んだ値
  static uint32_t changedMs = 0;  // 値が最後に変わった時刻
  static uint32_t pressedMs = 0;  // 押し始めた時刻
  static bool longDone = false;    // 今回の押下で長押しの処理をしたか
  bool now = digitalRead(PIN_BTN);
  uint32_t ms = millis();
  if (now != lastRead) {
    lastRead = now;
    changedMs = ms;
  }
  if (now != stable && ms - changedMs >= DEBOUNCE_MS) {
    stable = now;
    if (stable == LOW) {  // 押した
      pressedMs = ms;
      longDone = false;
    } else if (!longDone) {  // 長押しにならずに離した
      if (mode == OFF) {
        resume();
      } else {
        Mode next = (Mode)(mode + 1);
        if (next >= MODE_COUNT || next == OFF) next = ALL_ON;
        setMode(next);
      }
    }
  }
  if (stable == LOW && !longDone && ms - pressedMs >= LONG_PRESS_MS) {
    longDone = true;
    if (mode == OFF) {
      resume();
    } else {
      setMode(OFF);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_A, OUTPUT);
  pinMode(PIN_B, OUTPUT);
  pinMode(PIN_BTN, INPUT_PULLUP);
  drive(0);

  prefs.begin("illumination", false);
  uint8_t saved = prefs.getUChar("mode", ALL_ON);
  mode = (saved > OFF && saved < MODE_COUNT) ? (Mode)saved : ALL_ON;
  if (mode == AUTO) autoStartMs = millis();

  delay(500);
  Serial.println("イルミネーション制御を開始しました");
  printHelp();
  printStatus();
}

void loop() {
  handleSerial();
  handleButton();

  // 1周期の前半は A→B、後半は B→A。それぞれの半分の中で、明るさの分だけ電気を流す
  float a, b;
  levels(millis(), a, b);
  uint32_t onA = (uint32_t)(constrain(a, 0.0f, 1.0f) * HALF_US);
  uint32_t onB = (uint32_t)(constrain(b, 0.0f, 1.0f) * HALF_US);

  uint32_t start = micros();
  uint32_t t;
  while ((t = micros() - start) < CYCLE_US) {
    if (t < HALF_US) {
      drive(t < onA ? 1 : 0);
    } else {
      drive(t - HALF_US < onB ? -1 : 0);
    }
  }
}
