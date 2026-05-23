// Smart cane sketch for the T5-E1-Touch-AMOLED-1.75.
// Buzzes when the onboard IMU detects free fall or the HC-SR04 sees <= 10 cm.
#include <Arduino.h>
#include <Wire.h>
#include <math.h>

extern "C" {
  uint64_t bk_aon_rtc_get_us(void);
  void bk_timer_delay_us(uint32_t us);
}

#if defined(ARDUINO_ARCH_AVR)
#error "Wrong board selected. This sketch is for the Tuya T5-E1 Touch AMOLED board, not Arduino Uno. Install Tuya Open and select TUYA_T5AI_BOARD or T5 from Tools > Board."
#endif

// The Touch AMOLED board routes the onboard QMI8658 IMU to IO20/IO21.
// Tuya's Arduino variant defines those as SCL/SDA.
const uint8_t I2C_SDA_PIN = SDA;
const uint8_t I2C_SCL_PIN = SCL;

// Back header viewed left-to-right: [47] [17] [16] [15] [14] [3.3V] [GND] [5V]
const uint8_t BUZZER_PIN = 47;
const uint8_t ULTRASONIC_ECHO_PIN = 15;
const uint8_t ULTRASONIC_TRIG_PIN = 14;

const uint8_t QMI8658_ADDR_LOW = 0x6A;
const uint8_t QMI8658_ADDR_HIGH = 0x6B;
const uint8_t QMI8658_WHO_AM_I_VALUE = 0x05;

const uint8_t QMI8658_REG_WHO_AM_I = 0x00;
const uint8_t QMI8658_REG_CTRL1 = 0x02;
const uint8_t QMI8658_REG_CTRL2 = 0x03;
const uint8_t QMI8658_REG_CTRL7 = 0x08;
const uint8_t QMI8658_REG_STATUS0 = 0x2E;
const uint8_t QMI8658_REG_AX_L = 0x35;

const uint8_t QMI8658_ENABLE_ACCEL = 0x01;

// CTRL2 = accel range + output data rate.
// Range +/-8g -> 4096 LSB/g. ODR 125Hz is fast enough for fall/drop detection.
const uint8_t QMI8658_ACCEL_RANGE_8G = 0x02;
const uint8_t QMI8658_ACCEL_ODR_125HZ = 0x06;
const uint8_t QMI8658_ACCEL_CTRL2 =
  (QMI8658_ACCEL_RANGE_8G << 4) | QMI8658_ACCEL_ODR_125HZ;
const float QMI8658_ACCEL_LSB_PER_G = 4096.0f;

const float FREE_FALL_G_THRESHOLD = 0.65f;
const float OBSTACLE_ALERT_CM = 10.0f;
const float ULTRASONIC_MIN_VALID_CM = 2.0f;
const float ULTRASONIC_MAX_VALID_CM = 400.0f;

const unsigned long SAMPLE_INTERVAL_MS = 25;
const unsigned long ULTRASONIC_INTERVAL_MS = 100;
const unsigned long SERIAL_INTERVAL_MS = 250;
const unsigned long FREE_FALL_BUZZ_HOLD_MS = 1500;
const unsigned long ULTRASONIC_TIMEOUT_US = 30000;
const unsigned int BUZZER_FREQUENCY_HZ = 2300;
const unsigned int BUZZER_BURST_MS = 80;
const unsigned long BUZZER_REPEAT_MS = 130;
const uint8_t ULTRASONIC_SAMPLE_COUNT = 5;
const bool BUZZER_SELF_TEST_ON_BOOT = true;

struct AccelSample {
  int16_t x;
  int16_t y;
  int16_t z;
};

uint8_t qmiAddress = 0;
bool freeFallActive = false;
bool obstacleTooClose = false;
unsigned long freeFallBuzzUntil = 0;
unsigned long lastSampleAt = 0;
unsigned long lastUltrasonicAt = 0;
unsigned long lastSerialAt = 0;
unsigned long lastReconnectAttemptAt = 0;
unsigned long lastBuzzerAt = 0;
unsigned long lastFreeFallAlertPrintAt = 0;
unsigned long lastObstacleAlertPrintAt = 0;
bool hasAccelSample = false;
bool hasDistanceSample = false;
bool ultrasonicTimedOut = false;
float lastAxG = 0.0f;
float lastAyG = 0.0f;
float lastAzG = 0.0f;
float lastMagnitudeG = 0.0f;
float lastDistanceCm = -1.0f;
unsigned long lastEchoPulseUs = 0;

uint64_t preciseMicros() {
  return bk_aon_rtc_get_us();
}

void preciseDelayMicroseconds(uint32_t us) {
  bk_timer_delay_us(us);
}

bool writeRegister(uint8_t address, uint8_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool readRegisters(uint8_t address, uint8_t startReg, uint8_t *buffer, size_t length) {
  Wire.beginTransmission(address);
  Wire.write(startReg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  size_t received = Wire.requestFrom(address, length, true);
  if (received != length) {
    return false;
  }

  for (size_t i = 0; i < length && Wire.available(); ++i) {
    buffer[i] = Wire.read();
  }

  return true;
}

bool readRegister(uint8_t address, uint8_t reg, uint8_t &value) {
  return readRegisters(address, reg, &value, 1);
}

bool probeQmi8658(uint8_t address) {
  uint8_t whoAmI = 0;
  if (!readRegister(address, QMI8658_REG_WHO_AM_I, whoAmI)) {
    return false;
  }

  return whoAmI == QMI8658_WHO_AM_I_VALUE;
}

bool setupQmi8658() {
  if (probeQmi8658(QMI8658_ADDR_HIGH)) {
    qmiAddress = QMI8658_ADDR_HIGH;
  } else if (probeQmi8658(QMI8658_ADDR_LOW)) {
    qmiAddress = QMI8658_ADDR_LOW;
  } else {
    qmiAddress = 0;
    return false;
  }

  bool configured = true;

  // Enable address auto-increment, then configure and enable accelerometer only.
  configured &= writeRegister(qmiAddress, QMI8658_REG_CTRL1, 0x60);
  configured &= writeRegister(qmiAddress, QMI8658_REG_CTRL7, 0x00);
  configured &= writeRegister(qmiAddress, QMI8658_REG_CTRL2, QMI8658_ACCEL_CTRL2);
  configured &= writeRegister(qmiAddress, QMI8658_REG_CTRL7, QMI8658_ENABLE_ACCEL);
  delay(50);

  return configured;
}

bool isAccelDataReady() {
  uint8_t status = 0;
  if (!readRegister(qmiAddress, QMI8658_REG_STATUS0, status)) {
    return false;
  }

  return (status & 0x01) != 0;
}

bool readAcceleration(AccelSample &sample) {
  uint8_t raw[6];
  if (!readRegisters(qmiAddress, QMI8658_REG_AX_L, raw, sizeof(raw))) {
    return false;
  }

  sample.x = (int16_t)((raw[1] << 8) | raw[0]);
  sample.y = (int16_t)((raw[3] << 8) | raw[2]);
  sample.z = (int16_t)((raw[5] << 8) | raw[4]);
  return true;
}

void stopBuzzer() {
  digitalWrite(BUZZER_PIN, LOW);
}

void playManualTone(unsigned int frequency, unsigned int durationMs) {
  if (frequency == 0 || durationMs == 0) {
    delay(durationMs);
    return;
  }

  unsigned long halfPeriodUs = 1000000UL / (frequency * 2UL);
  unsigned long cycles = (durationMs * 1000UL) / (halfPeriodUs * 2UL);

  for (unsigned long i = 0; i < cycles; ++i) {
    digitalWrite(BUZZER_PIN, HIGH);
    preciseDelayMicroseconds(halfPeriodUs);
    digitalWrite(BUZZER_PIN, LOW);
    preciseDelayMicroseconds(halfPeriodUs);
  }

  stopBuzzer();
}

void playStartupBuzzerTest() {
  if (!BUZZER_SELF_TEST_ON_BOOT) {
    return;
  }

  Serial.println("Buzzer self-test");
  for (int i = 0; i < 3; ++i) {
    playManualTone(BUZZER_FREQUENCY_HZ, 120);
    delay(90);
  }
}

void updateFreeFallDetection(float magnitudeG) {
  unsigned long now = millis();

  if (magnitudeG < FREE_FALL_G_THRESHOLD) {
    if (!freeFallActive) {
      freeFallActive = true;
      Serial.println("free-fall candidate");
    }

    freeFallBuzzUntil = now + FREE_FALL_BUZZ_HOLD_MS;

    if (now - lastFreeFallAlertPrintAt >= FREE_FALL_BUZZ_HOLD_MS) {
      lastFreeFallAlertPrintAt = now;
      Serial.print("ALERT free-fall magnitude=");
      Serial.print(magnitudeG, 2);
      Serial.println("g");
    }

    return;
  }

  freeFallActive = false;
}

unsigned long readHighPulseUs(uint8_t pin, unsigned long timeoutUs) {
  uint64_t waitStartedAt = preciseMicros();
  while (digitalRead(pin) == LOW) {
    if (preciseMicros() - waitStartedAt >= timeoutUs) {
      return 0;
    }
  }

  uint64_t pulseStartedAt = preciseMicros();
  while (digitalRead(pin) == HIGH) {
    if (preciseMicros() - pulseStartedAt >= timeoutUs) {
      return 0;
    }
  }

  return (unsigned long)(preciseMicros() - pulseStartedAt);
}

float readUltrasonicDistanceCm() {
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  preciseDelayMicroseconds(2);
  digitalWrite(ULTRASONIC_TRIG_PIN, HIGH);
  preciseDelayMicroseconds(10);
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);

  unsigned long durationUs = readHighPulseUs(ULTRASONIC_ECHO_PIN, ULTRASONIC_TIMEOUT_US);
  lastEchoPulseUs = durationUs;
  ultrasonicTimedOut = durationUs == 0;
  if (durationUs == 0) {
    return -1.0f;
  }

  float distanceCm = durationUs * 0.0343f / 2.0f;
  if (distanceCm < ULTRASONIC_MIN_VALID_CM || distanceCm > ULTRASONIC_MAX_VALID_CM) {
    return -1.0f;
  }

  return distanceCm;
}

void sortDistances(float *values, uint8_t count) {
  for (uint8_t i = 1; i < count; ++i) {
    float current = values[i];
    int j = i - 1;

    while (j >= 0 && values[j] > current) {
      values[j + 1] = values[j];
      --j;
    }

    values[j + 1] = current;
  }
}

float readStableUltrasonicDistanceCm() {
  float validDistances[ULTRASONIC_SAMPLE_COUNT];
  uint8_t validCount = 0;

  for (uint8_t i = 0; i < ULTRASONIC_SAMPLE_COUNT; ++i) {
    float distanceCm = readUltrasonicDistanceCm();
    if (distanceCm > 0.0f) {
      validDistances[validCount] = distanceCm;
      ++validCount;
    }
    delay(8);
  }

  if (validCount == 0) {
    return -1.0f;
  }

  sortDistances(validDistances, validCount);
  return validDistances[validCount / 2];
}

void updateUltrasonic(unsigned long now) {
  if (now - lastUltrasonicAt < ULTRASONIC_INTERVAL_MS) {
    return;
  }
  lastUltrasonicAt = now;

  lastDistanceCm = readStableUltrasonicDistanceCm();
  hasDistanceSample = lastDistanceCm > 0.0f;
  obstacleTooClose = hasDistanceSample && lastDistanceCm <= OBSTACLE_ALERT_CM;

  if (obstacleTooClose && now - lastObstacleAlertPrintAt >= 500) {
    lastObstacleAlertPrintAt = now;
    Serial.print("ALERT obstacle distance=");
    Serial.print(lastDistanceCm, 1);
    Serial.println("cm");
  }
}

void updateBuzzer(unsigned long now) {
  bool freeFallBuzzActive = now < freeFallBuzzUntil;
  bool shouldBuzz = freeFallBuzzActive || obstacleTooClose;

  if (!shouldBuzz) {
    stopBuzzer();
    return;
  }

  if (now - lastBuzzerAt >= BUZZER_REPEAT_MS) {
    lastBuzzerAt = now;
    playManualTone(BUZZER_FREQUENCY_HZ, BUZZER_BURST_MS);
  }
}

void printDebug(unsigned long now) {
  if (now - lastSerialAt < SERIAL_INTERVAL_MS) {
    return;
  }
  lastSerialAt = now;

  if (hasAccelSample) {
    Serial.print("ax=");
    Serial.print(lastAxG, 2);
    Serial.print("g ay=");
    Serial.print(lastAyG, 2);
    Serial.print("g az=");
    Serial.print(lastAzG, 2);
    Serial.print("g mag=");
    Serial.print(lastMagnitudeG, 2);
    Serial.print("g");
  } else {
    Serial.print("accel=waiting");
  }

  Serial.print(" distance=");
  if (hasDistanceSample) {
    Serial.print(lastDistanceCm, 1);
    Serial.print("cm");
  } else {
    Serial.print("--");
  }

  Serial.print(" echoUs=");
  Serial.print(lastEchoPulseUs);
  Serial.print(" ultrasonic=");
  if (ultrasonicTimedOut) {
    Serial.print("timeout");
  } else if (obstacleTooClose) {
    Serial.print("too-close");
  } else if (hasDistanceSample) {
    Serial.print("valid");
  } else {
    Serial.print("out-of-range");
  }
  Serial.print(" buzzer=");
  Serial.println((obstacleTooClose || millis() < freeFallBuzzUntil) ? "on" : "off");
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(BUZZER_PIN, OUTPUT);
  stopBuzzer();
  pinMode(ULTRASONIC_TRIG_PIN, OUTPUT);
  pinMode(ULTRASONIC_ECHO_PIN, INPUT);
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  playStartupBuzzerTest();

  Wire.setSDA(I2C_SDA_PIN);
  Wire.setSCL(I2C_SCL_PIN);
  Wire.begin();
  Wire.setClock(400000);

  Serial.println("Smart cane onboard accelerometer starting");
  Serial.print("Internal I2C SDA=");
  Serial.print(I2C_SDA_PIN);
  Serial.print(" SCL=");
  Serial.println(I2C_SCL_PIN);
  Serial.print("Ultrasonic TRIG=IO");
  Serial.print(ULTRASONIC_TRIG_PIN);
  Serial.print(" ECHO=IO");
  Serial.print(ULTRASONIC_ECHO_PIN);
  Serial.print(" buzzer=IO");
  Serial.println(BUZZER_PIN);
  Serial.print("Buzzer trigger: free fall or distance <= ");
  Serial.print(OBSTACLE_ALERT_CM, 1);
  Serial.println("cm");

  if (setupQmi8658()) {
    Serial.print("QMI8658 connected at 0x");
    Serial.println(qmiAddress, HEX);
  } else {
    Serial.println("QMI8658 not found. Make sure the T5-E1 Touch AMOLED board is selected.");
  }
}

void loop() {
  unsigned long now = millis();

  if (qmiAddress == 0) {
    if (now - lastReconnectAttemptAt >= 1000) {
      lastReconnectAttemptAt = now;
      if (setupQmi8658()) {
        Serial.print("QMI8658 connected at 0x");
        Serial.println(qmiAddress, HEX);
      } else {
        Serial.println("Waiting for onboard QMI8658...");
      }
    }
  }

  updateUltrasonic(now);
  updateBuzzer(now);

  if (qmiAddress == 0) {
    printDebug(now);
    return;
  }

  if (now - lastSampleAt < SAMPLE_INTERVAL_MS) {
    printDebug(now);
    return;
  }
  lastSampleAt = now;

  if (!isAccelDataReady()) {
    printDebug(now);
    return;
  }

  AccelSample sample;
  if (!readAcceleration(sample)) {
    Serial.println("QMI8658 read failed. Re-scanning I2C bus...");
    qmiAddress = 0;
    freeFallActive = false;
    hasAccelSample = false;
    return;
  }

  lastAxG = sample.x / QMI8658_ACCEL_LSB_PER_G;
  lastAyG = sample.y / QMI8658_ACCEL_LSB_PER_G;
  lastAzG = sample.z / QMI8658_ACCEL_LSB_PER_G;
  lastMagnitudeG = sqrt((lastAxG * lastAxG) + (lastAyG * lastAyG) + (lastAzG * lastAzG));
  hasAccelSample = true;

  printDebug(now);
  updateFreeFallDetection(lastMagnitudeG);
  updateBuzzer(millis());
}
