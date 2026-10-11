#include <Wire.h>

#define SDA_PIN 26
#define SCL_PIN 27

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Wire1.setSDA(SDA_PIN);
  Wire1.setSCL(SCL_PIN);
  Wire1.begin();
  Wire1.setClock(100000);

  Serial.println("\nI2C Scanner");
}

void loop() {
  int count = 0;
  Serial.println("Scanning...");

  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire1.beginTransmission(addr);
    uint8_t err = Wire1.endTransmission();

    if (err == 0) {
      Serial.print("Found device at 0x");
      if (addr < 16) Serial.print("0");
      Serial.println(addr, HEX);
      count++;
    }
  }

  if (count == 0) Serial.println("No I2C devices found");
  else {
    Serial.print("Done, ");
    Serial.print(count);
    Serial.println(" device(s)");
  }

  delay(3000);
}