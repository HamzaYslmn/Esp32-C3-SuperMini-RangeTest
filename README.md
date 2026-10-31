# ESP32-C3 SuperMini Range Test

ESP-NOW vs BLE range between two ESP32-C3 SuperMini boards, both at 15 dBm.

## Results

| Link | Line of sight | Through a brick (briket) wall |
|------|--------------:|------------------------------:|
| ESP-NOW (long range, 250 kbps) | **60 m** | **20 m** |
| BLE (LE Coded PHY S=8, 125 kbps) | 15 m | 10 m |

ESP-NOW reaches about 4x farther in the open and 2x farther through a wall.

## Sketches

Flash the same sketch to both boards. They pair by themselves, then send each other a packet 10 times a second.
Each packet carries the dBm it last heard the other board at, so both directions show on Serial (115200).
LED (GPIO 8): fast blink while pairing, then brightness follows the weaker direction, dark when the link is gone.

- [`espnow_range/`](espnow_range) ESP-NOW, channel 1, long range mode. Type `pair` or press BOOT to pair again.
- [`ble_range/`](ble_range) BLE, connects on 1M then switches to Coded PHY. Pair the boards close together, then walk.

## Build

Each folder has a `sketch.yaml` (board, core 3.3.12), so arduino-cli needs no flags:

```bash
arduino-cli compile --upload -p COM13 espnow_range
```
