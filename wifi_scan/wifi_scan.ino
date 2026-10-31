// Wi-Fi scan: lists the networks around every second on Serial (115200).
#include <WiFi.h>

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_STA);
}

void loop() {
  uint32_t start = millis();
  int n = WiFi.scanNetworks(false, true, false, 60);  // 60 ms per channel: all 13 in under a second
  Serial.printf("\n%d networks\n", max(n, 0));
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    Serial.printf("%4d dBm  ch %2d  %s\n", WiFi.RSSI(i), WiFi.channel(i), ssid.length() ? ssid.c_str() : "(hidden)");
  }
  WiFi.scanDelete();
  while (millis() - start < 1000) delay(10);
}
