# Bill of materials

Items marked **(from firmware)** are confirmed by the code. Fill in the `TODO` fields with the exact parts you used.

| Qty | Part | Notes |
|---|---|---|
| 1 | ESP32 dev board | **(from firmware)** I2C on GPIO 21 (SDA) / 22 (SCL), USB serial at 115200 |
| 1 | PCA9685 16-channel PWM/servo driver | **(from firmware)** address `0x41`, 50 Hz; channels 0-3 used |
| 4 | Hobby servos | **(from firmware)** base, shoulder, elbow, gripper; pulse 600-2300 µs. TODO: exact models (an MG90S-type micro servo is visible in the build photo) |
| 1 | External servo power supply / battery | TODO: voltage, current rating, model |
| 1 | USB cable | ESP32 to computer (power, upload, web control) |
| - | Dupont jumper wires | I2C and power |
| - | 3D-printed parts | See [`../cad/`](../cad/). TODO: material, infill, print settings |
| - | M2/M3 screws and servo horns | TODO: sizes used |

## Optional

| Part | Notes |
|---|---|
| 10 kΩ pull-up + wire from PCA9685 `OE` to a GPIO | Keeps servo outputs off while the ESP32 boots. Set `OE_PIN` in the sketch (default `-1` = not wired) |
| Large capacitor across servo supply | Smooths current spikes (typical practice, not required by the code) |
