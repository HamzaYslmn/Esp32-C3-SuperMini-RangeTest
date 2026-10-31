// ESP-NOW pairing and range test: the same sketch on both boards. They find each other by broadcast
// and pair once each hears the other, then send to each other's MAC 10 times a second, each send acked by the
// other's radio. Every packet carries the dBm we last heard the other at, so each side knows both directions.
// LED: fast blink while pairing, then the weaker direction: 50 % = 50 % PWM, dark when gone.
// Type pair or press BOOT to pair again; the other board joins in by itself.
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

enum Kind : uint8_t { HELLO = 1, DATA };
constexpr int PIN_LED = 8;                                  // on-board, active low
constexpr int PIN_BOOT = 9;                                 // low while pressed
// The real sketches' radio: channel 1, long range 250 kbps, 15 dBm.
constexpr uint8_t CHANNEL = 1;
// Quarter dBm. Measured on the desk, 8 pairings each: 60 paired in 0.55-0.63 s every time. At 66 (16.5 dBm) the
// 7C:4F board's broadcasts reached the B8:1F board 0 of 20 a second; pairing took 1.6-6 s or never finished.
// Acked sends still got through (the radio resends them), so the range test alone doesn't show it.
constexpr int8_t TX_POWER = 60;
constexpr uint32_t SEND_MS = 100;
constexpr uint32_t HELLO_MS = 50;
constexpr uint32_t SETTLE_MS = 500;              // hellos after both hear each other, so the other finishes too
constexpr uint32_t GONE_MS = 500;                // nothing heard this long: LED off
constexpr int RSSI_FULL = -40, RSSI_NONE = -90;  // dBm for 100 % and 0 %: tune to the place you test in
const uint8_t EVERYONE[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

struct Packet {
  uint8_t kind;
  int8_t heardYou;  // dBm the sender last heard the receiver at, 0 not yet
};

uint8_t peer[6], found[6];  // found: the board pairing answered us, EVERYONE until then
volatile bool pairing, rejoin;
volatile int8_t weHear, theyHear;  // dBm, 0 not yet
volatile uint32_t heardMs, heardCount, ackedCount, hellosHeard;

// Both run in the Wi-Fi task: only note it down.
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(Packet)) return;
  Packet p;
  memcpy(&p, data, sizeof p);
  if (p.kind == HELLO) {
    hellosHeard++;
    if (!pairing) {
      rejoin |= !p.heardYou;  // it just started pairing: join in. Its last hellos after pairing say they hear us
      return;
    }
    if (!memcmp(found, EVERYONE, 6)) memcpy(found, info->src_addr, 6);  // ponytail: the first one heard, the real sketches take the strongest
    if (memcmp(info->src_addr, found, 6)) return;
  } else if (p.kind != DATA || pairing || memcmp(info->src_addr, peer, 6)) {
    return;
  } else {
    heardMs = millis();
    heardCount++;
  }
  weHear = info->rx_ctrl->rssi;
  theyHear = p.heardYou;
}

void onSent(const esp_now_send_info_t *info, esp_now_send_status_t status) {
  if (status == ESP_NOW_SEND_SUCCESS && memcmp(info->des_addr, EVERYONE, 6)) ackedCount++;  // a hello is never acked
}

void addPeer(const uint8_t mac[6]) {
  if (esp_now_is_peer_exist(mac)) return;
  esp_now_peer_info_t info{};
  memcpy(info.peer_addr, mac, 6);
  info.channel = CHANNEL;
  info.ifidx = WIFI_IF_STA;
  esp_now_add_peer(&info);
  esp_now_rate_config_t rate{};
  rate.phymode = WIFI_PHY_MODE_LR;
  rate.rate = WIFI_PHY_RATE_LORA_250K;
  esp_now_set_peer_rate_config(mac, &rate);
}

uint32_t pairStartMs, confirmedMs, hellosSent, hellosPrintedMs;
void startPairing() {
  memcpy(found, EVERYONE, 6);
  weHear = theyHear = 0;
  confirmedMs = hellosSent = hellosHeard = 0;
  pairStartMs = hellosPrintedMs = millis();
  pairing = true;
  Serial.println("pairing: looking for the other board, start it too");
}

// 0..100 % of the RSSI_NONE..RSSI_FULL span.
int percent(int rssi) {
  return rssi ? constrain(map(rssi, RSSI_NONE, RSSI_FULL, 0, 100), 0, 100) : 0;
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never block on a closed monitor
  Serial.setTimeout(20);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  ledcAttach(PIN_LED, 5000, 8);
  ledcOutputInvert(PIN_LED, true);  // active low: duty is plain brightness

  WiFi.mode(WIFI_STA);
  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
  esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_max_tx_power(TX_POWER);
  esp_wifi_set_ps(WIFI_PS_NONE);  // always listening

  esp_now_init();
  esp_now_register_recv_cb(onReceive);
  esp_now_register_send_cb(onSent);
  addPeer(EVERYONE);

  int8_t power;
  esp_wifi_get_max_tx_power(&power);
  Serial.printf("pairing and range test, MAC %s, TX %.2f dBm\n", WiFi.macAddress().c_str(), power / 4.0);
  startPairing();
}

void pairStep(uint32_t now) {
  static uint32_t helloMs;
  if (now - hellosPrintedMs >= 1000) {  // a hello is a broadcast: never acked, never resent, so this is the raw loss
    hellosPrintedMs = now;
    Serial.printf("pairing: sent %u hellos, heard %u\n", hellosSent, hellosHeard);
    hellosSent = hellosHeard = 0;
  }
  if (theyHear && !confirmedMs) {
    confirmedMs = now;
    Serial.printf("pairing: found %02X:%02X:%02X:%02X:%02X:%02X, we hear it at %d dBm, it hears us at %d dBm\n",
                  found[0], found[1], found[2], found[3], found[4], found[5], weHear, theyHear);
  }
  if (confirmedMs && now - confirmedMs > SETTLE_MS) {
    memcpy(peer, found, 6);
    addPeer(peer);
    heardMs = now;
    pairing = false;
    Serial.printf("pairing: done in %u ms\n", now - pairStartMs);
  } else if (now - helloMs >= HELLO_MS) {
    helloMs = now;
    Packet p{HELLO, weHear};
    hellosSent += esp_now_send(EVERYONE, (const uint8_t *)&p, sizeof p) == ESP_OK;
  }
  ledcWrite(PIN_LED, now / 100 % 2 * 255);
}

void loop() {
  static uint32_t sentMs, printedMs, heardThen, ackedThen;
  static bool bootWas;
  uint32_t now = millis();

  bool boot = !digitalRead(PIN_BOOT);
  bool typed = Serial.available() && Serial.readStringUntil('\n').indexOf("pair") >= 0;
  if (rejoin || typed || (boot && !bootWas)) {
    rejoin = false;
    startPairing();
  }
  bootWas = boot;
  if (pairing) {
    pairStep(now);
    delay(10);
    return;
  }

  bool gone = now - heardMs > GONE_MS;
  if (gone) weHear = theyHear = 0;

  if (now - sentMs >= SEND_MS) {
    sentMs = now;
    Packet p{DATA, weHear};
    esp_now_send(peer, (const uint8_t *)&p, sizeof p);
  }

  int weakest = min(percent(weHear), percent(theyHear));
  ledcWrite(PIN_LED, weakest * 255 / 100);

  if (now - printedMs >= 1000) {  // once a second: both directions, packets heard and sends acked
    printedMs = now;
    uint32_t heard = heardCount, acked = ackedCount, per = 1000 / SEND_MS;
    if (gone) Serial.printf("no signal  acked %u/%u\n", acked - ackedThen, per);
    else Serial.printf("we hear %d dBm %3d %%  they hear %d dBm %3d %%  heard %u/%u  acked %u/%u\n",
                       weHear, percent(weHear), theyHear, percent(theyHear), heard - heardThen, per, acked - ackedThen, per);
    heardThen = heard;
    ackedThen = acked;
  }
  delay(10);
}
