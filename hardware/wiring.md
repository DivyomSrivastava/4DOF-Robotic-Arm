# Wiring

## Connections (from the firmware)

| ESP32 | PCA9685 | Purpose |
|---|---|---|
| GPIO 21 | SDA | I2C data |
| GPIO 22 | SCL | I2C clock (100 kHz) |
| 3V3 | VCC | Logic power (typical setup) |
| GND | GND | **Common ground** |
| (optional) GPIO of your choice | OE | Output enable; set `OE_PIN` in the sketch |

| PCA9685 channel | Servo |
|---|---|
| 0 | Base (yaw) |
| 1 | Shoulder |
| 2 | Elbow |
| 3 | Gripper |

## Power

- Power the servos through the PCA9685 **V+ terminal from an external supply**, not from the ESP32's pins. Servos draw current spikes that can brown out the board.
- Connect the **supply ground, PCA9685 ground and ESP32 ground together**.
- The I2C address is set by the PCA9685 address jumpers. The sketch uses `0x41`; change `PCA_ADDRESS` if yours differs. If you see `ERROR: PCA9685 not detected`, check SDA/SCL, power, ground and address.

## TODO

- [ ] Add a wiring diagram (e.g. exported from Fritzing or KiCad) as `wiring_diagram.png`.
- [ ] Note the supply voltage you used.
