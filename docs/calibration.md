# Calibration

The IK only works if the numbers in the firmware describe your arm. The web page repeats some of them, so keep both in sync.

| What | Sketch | Web page (`arm_studio.html`) |
|---|---|---|
| Link lengths | `BASE_HEIGHT`, `LINK1`, `LINK2` (cm) | `CFG = { Lb, L1, L2 }` (mm) |
| Angle offsets / directions | `SERVO_OFFSET`, `SERVO_DIRECTION` | `MAP.off`, `MAP.dir` |
| Joint limits | `MIN_ANGLE`, `MAX_ANGLE` | `MAP.min`, `MAP.max` |

Current values: base height 4.3 cm, link 1 6.4 cm, link 2 7.2 cm; limits base 0-180, shoulder 5-175, elbow 5-175, gripper 30-110.

## Steps

1. **Pulse range**: `MIN_PULSE_US` / `MAX_PULSE_US` (600-2300 µs) set what 0° and 180° mean for each servo. Adjust if a servo buzzes at the ends.
2. **Mechanical zero**: with `ANGLES 90 90 90 90`, the base should face forward, the upper arm horizontal and the forearm vertical. Re-mount horns or tweak `SERVO_OFFSET` until it matches.
3. **Limits**: move each joint with `<joint> <angle>` slowly towards its end stop and set `MIN_ANGLE`/`MAX_ANGLE` a few degrees inside where it physically stops.
4. **Gripper**: set `GRIP_OPEN` and `GRIP_CLOSE` to the angles where the fingers are open and just closed.
5. **Check the kinematics**: command `XYZ 10 0 8`, measure the real tip position, and compare with `WHERE`. Fix link lengths if they differ.
6. **Speeds**: tune `JOINT_PEAK_DPS`, `JOINT_LIVE_DPS` and `JOINT_ACCEL_DPS2` for how gentle you want it.
7. **I2C address**: set `PCA_ADDRESS` to match your PCA9685 jumpers.
