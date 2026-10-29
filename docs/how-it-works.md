# How it works

## Coordinate frame

Origin on the table at the base centre. **x forward, y left, z up.** The target is the end of the forearm. The firmware works in cm; the web page in mm.

## Inverse kinematics (`solveIK` in the sketch)

```text
base     = atan2(y, x)
r        = sqrt(x² + y²)           horizontal distance
h        = z - BASE_HEIGHT         height above the shoulder axis
d²       = r² + h²
cos(e)   = (d² - L1² - L2²) / (2·L1·L2)       elbow bend, 0 = straight
shoulder = atan2(h, r) - atan2(L2·sin(e), L1 + L2·cos(e))
```

The target is rejected if it is outside `|L1-L2|..L1+L2`, would put the elbow or tip into the table, or needs a joint outside its limits. Servo angle = `SERVO_OFFSET + SERVO_DIRECTION × joint angle`.

## Forward kinematics (`WHERE`)

Joint angles back to the tip position, used to verify the arm and the calibration.

## Motion engine

- **Eased moves** (`XYZ`, `ANGLES`, `HOME`, routines): minimum-jerk easing `t³(6t² - 15t + 10)` with zero speed and acceleration at both ends. The duration is chosen so the slowest-limited joint stays under its peak speed (`JOINT_PEAK_DPS × speed scale × master speed`), and all joints share it so they arrive together.
- **Live mode** (`T x y z grip` from the web page): every joint chases the latest target with limited speed and acceleration, braking early enough to stop on the target.
- **Routines**: table-driven, non-blocking sequencer with optional repeat blocks; `STOP` works at any time.
- **Update rate**: 50 Hz.

## Web interface

`arm_studio.html` is a single file: Three.js for the 3D view and gizmo, the same IK maths in JavaScript, and the Web Serial API for the USB link. It streams `T x y z grip` at roughly 14 Hz while you drag, and shows the resulting joint and servo angles. Unticking "Send commands to the arm" turns it into a pure simulator.
