# Serial command reference

Connect at **115200 baud**, line ending **Newline**. Commands are case-insensitive. Type `HELP` for the list on the device.

## Motion

| Command | Description |
|---|---|
| `XYZ x y z` (or `GO x y z`) | Move the forearm tip to a point, in **cm**. Origin is the base centre on the table; x forward, y left, z up |
| `WHERE` (or `FK`) | Print the current tip position (forward kinematics) |
| `HOME` | Glide to the 90/90/90/90 pose |
| `ANGLES b s e g` | Set base, shoulder, elbow, gripper servo angles |
| `<joint> <angle>` | Move one joint: 0 base, 1 shoulder, 2 elbow, 3 gripper (e.g. `2 45`) |
| `GRIP OPEN` / `GRIP CLOSE` | Open or close the gripper |
| `STOP` | Freeze in place; also cancels a routine |
| `POS` | Print joint angles |
| `SPEED v` | Master speed, 0.2-1.0 (`SPEED` alone prints it) |

## Routines

Each one ends by gliding back to HOME. `STRETCH` also runs at power-on (`STRETCH_ON_STARTUP`).

`STRETCH` · `WAVE` · `NOD` · `DANCE` · `PEEKABOO` · `POUNCE` · `STARTLED` · `PLAY DEAD`

Routines are tables of steps in the sketch (angles, speed scale, pause). Edit or add steps freely; unsafe steps are skipped automatically.

## Web page protocol

| Command | Description |
|---|---|
| `?` | Link check; replies `READY 4-servo arm (ESP32 + PCA9685)` |
| `T x y z grip` | Live target in **mm**, grip 0 (open) to 100 (closed). Streamed by ARM / STUDIO a few times per second |

Replies of `ERR unreachable` mean the target is outside the reachable workspace.
