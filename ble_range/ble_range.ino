// BLE long range test: the same sketch on both boards. Each advertises and scans; the one with the higher address
// connects, then both ask for LE Coded PHY S=8 (125 kbps, BLE's long range mode) and send each other a packet
// 10 times a second, each acked and resent by the link layer. Every packet carries the dBm we last heard the other
// at, so each side knows both directions. LED: fast blink while searching, then the weaker direction: 50 % = 50 %
// PWM, dark when gone. Same output as ../espnow_range, at the same 15 dBm, to compare the two.
// The C3 core's NimBLE is built without extended advertising, so coded PHY needs a connection: it is made on 1M,
// which reaches less far, then holds on coded. Connect the boards near each other, then walk.
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEClient.h>
#include <host/ble_gap.h>  // coded PHY and connection RSSI: not in the wrapper

const char *SERVICE = "8d3f0001-5c2e-4b7a-9d61-2f0b7e4a1c55";
const char *CHAR = "8d3f0002-5c2e-4b7a-9d61-2f0b7e4a1c55";
constexpr int PIN_LED = 8;                                // on-board, active low
constexpr esp_power_level_t TX_POWER = ESP_PWR_LVL_P15;  // 15 dBm, as espnow_range
constexpr uint32_t SEND_MS = 100;
constexpr uint32_t GONE_MS = 500;                // nothing heard this long: LED off
constexpr int RSSI_FULL = -40, RSSI_NONE = -90;  // dBm for 100 % and 0 %: tune to the place you test in
constexpr uint16_t NONE = BLE_HS_CONN_HANDLE_NONE;

struct Packet {
  int8_t heardYou;  // dBm the sender last heard the receiver at, 0 not yet
};

volatile int8_t theyHear;
volatile uint32_t heardMs, heardCount;
volatile uint16_t conn = NONE;
BLEAdvertisedDevice *volatile found;  // the board to connect to, from the scan
bool central;                         // we connected: we write, it notifies
BLEClient *client;
BLERemoteCharacteristic *remote;
BLECharacteristic *local;

// All callbacks run in the BLE host task: only note it down.
void onPacket(const uint8_t *data, size_t len) {
  if (len != sizeof(Packet)) return;
  theyHear = ((const Packet *)data)->heardYou;
  heardMs = millis();
  heardCount++;
}

struct Peripheral : BLEServerCallbacks, BLECharacteristicCallbacks {
  void onConnect(BLEServer *, ble_gap_conn_desc *desc) override { conn = desc->conn_handle; }
  void onDisconnect(BLEServer *, ble_gap_conn_desc *) override { conn = NONE; }
  void onWrite(BLECharacteristic *c, ble_gap_conn_desc *) override { onPacket(c->getData(), c->getLength()); }
};

struct Central : BLEClientCallbacks {
  void onConnect(BLEClient *) override {}
  void onDisconnect(BLEClient *) override { conn = NONE; }
};

struct Finder : BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice d) override {
    if (conn != NONE || found || !d.isAdvertisingService(BLEUUID(SERVICE))) return;
    if (BLEDevice::getAddress() > d.getAddress()) found = new BLEAdvertisedDevice(d);  // else it connects to us
  }
};

void search() {
  central = false;
  remote = nullptr;
  BLEDevice::startAdvertising();
  BLEDevice::getScan()->start(0, nullptr, false);
  Serial.println("searching: start the other board too");
}

// Central only: the connect and service lookup block for up to a few seconds.
bool connect() {
  BLEDevice::getScan()->stop();
  BLEDevice::stopAdvertising();
  Serial.printf("found %s, connecting\n", found->getAddress().toString().c_str());
  bool ok = client->connect(found);
  BLERemoteService *service = ok ? client->getService(SERVICE) : nullptr;
  remote = service ? service->getCharacteristic(CHAR) : nullptr;
  ok = remote && remote->subscribe(true, [](BLERemoteCharacteristic *, uint8_t *data, size_t len, bool) { onPacket(data, len); });
  delete found;
  found = nullptr;
  if (!ok) {
    Serial.println("connect failed");
    if (client->isConnected()) client->disconnect();
    return false;
  }
  central = true;
  conn = client->getConnId();
  return true;
}

const char *phyName(uint8_t phy) {
  return phy == BLE_GAP_LE_PHY_1M ? "1M" : phy == BLE_GAP_LE_PHY_2M ? "2M" : phy == BLE_GAP_LE_PHY_CODED ? "coded" : "?";
}

// 0..100 % of the RSSI_NONE..RSSI_FULL span.
int percent(int rssi) {
  return rssi ? constrain(map(rssi, RSSI_NONE, RSSI_FULL, 0, 100), 0, 100) : 0;
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never block on a closed monitor
  ledcAttach(PIN_LED, 5000, 8);
  ledcOutputInvert(PIN_LED, true);  // active low: duty is plain brightness

  BLEDevice::init("range");
  esp_ble_tx_power_set_enhanced(ESP_BLE_ENHANCED_PWR_TYPE_DEFAULT, 0, TX_POWER);
  static Peripheral peripheral;
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(&peripheral);
  local = server->createService(SERVICE)->createCharacteristic(
    CHAR, BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);
  local->setCallbacks(&peripheral);
  server->getServiceByUUID(SERVICE)->start();
  BLEDevice::getAdvertising()->addServiceUUID(SERVICE);

  static Central centralCb;
  client = BLEDevice::createClient();
  client->setClientCallbacks(&centralCb);
  static Finder finder;
  BLEDevice::getScan()->setAdvertisedDeviceCallbacks(&finder, true);
  BLEDevice::getScan()->setActiveScan(false);

  Serial.printf("BLE range test, address %s\n", BLEDevice::getAddress().toString().c_str());
  search();
}

void loop() {
  static uint32_t sentMs, printedMs, heardThen;
  static uint16_t linked = NONE;
  uint32_t now = millis();

  if (found && !connect()) search();
  if (conn != linked) {  // connected or lost
    linked = conn;
    if (linked == NONE) {
      Serial.println("LOST the other board");
      search();
    } else {
      BLEDevice::getScan()->stop();
      ble_gap_set_prefered_le_phy(linked, BLE_GAP_LE_PHY_CODED_MASK, BLE_GAP_LE_PHY_CODED_MASK, BLE_GAP_LE_PHY_CODED_S8);
      esp_ble_tx_power_set_enhanced(ESP_BLE_ENHANCED_PWR_TYPE_CONN, linked, TX_POWER);
      heardMs = now;
      int level = esp_ble_tx_power_get_enhanced(ESP_BLE_ENHANCED_PWR_TYPE_CONN, linked);
      Serial.printf("connected as %s, TX %d dBm, asking for coded PHY\n", central ? "central" : "peripheral",
                    level >= ESP_PWR_LVL_P20 ? 20 : level * 3 - 24);
    }
  }
  if (linked == NONE) {
    ledcWrite(PIN_LED, now / 100 % 2 * 255);
    delay(10);
    return;
  }

  int8_t weHear = 0;
  if (ble_gap_conn_rssi(linked, &weHear) || weHear == 127) weHear = 0;  // 127: not known yet
  bool gone = now - heardMs > GONE_MS;
  if (gone) weHear = theyHear = 0;

  if (now - sentMs >= SEND_MS) {
    sentMs = now;
    Packet p{weHear};
    if (central && remote) remote->writeValue((uint8_t *)&p, sizeof p, false);
    else if (!central) {
      local->setValue((uint8_t *)&p, sizeof p);
      local->notify();
    }
  }

  int weakest = min(percent(weHear), percent(theyHear));
  ledcWrite(PIN_LED, weakest * 255 / 100);

  if (now - printedMs >= 1000) {  // once a second: both directions, packets heard, the PHY in use
    printedMs = now;
    uint32_t heard = heardCount, per = 1000 / SEND_MS;
    uint8_t tx = 0, rx = 0;
    ble_gap_read_le_phy(linked, &tx, &rx);
    if (gone) Serial.printf("no signal  phy %s/%s\n", phyName(tx), phyName(rx));
    else Serial.printf("we hear %d dBm %3d %%  they hear %d dBm %3d %%  heard %u/%u  phy %s/%s\n",
                       weHear, percent(weHear), theyHear, percent(theyHear), heard - heardThen, per, phyName(tx), phyName(rx));
    heardThen = heard;
  }
  delay(10);
}
