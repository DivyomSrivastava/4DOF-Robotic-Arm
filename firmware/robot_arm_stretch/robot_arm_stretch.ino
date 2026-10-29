#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <math.h>
#include <stdio.h>

// =====================================================
// ESP32 + PCA9685 + 4-servo arm: base, shoulder, elbow, gripper
// Smooth eased motion, safe startup, XYZ control.
// Serial Monitor: 115200 baud, Newline
//
// MERGED VERSION + STARTUP STRETCH + NATURAL SPEEDS + PLAYFUL ROUTINES:
//   - web page commands ("?", "T x y z grip", "FK")
//   - routines (table driven, non-blocking), each ends by gliding to HOME:
//        STRETCH    - the "waking up" stretch (also runs at power-on)
//        STARTLED   - jumps back, shivers, creeps back warily
//        PLAY DEAD  - staggers, collapses, last twitch, lies limp, slowly revives
//        WAVE       - raises the arm and waves hello
//        NOD        - nods "yes"
//        DANCE      - sways, bobs and snaps the gripper to a beat, then ta-da
//        PEEKABOO   - ducks, slowly peeks, ducks again, then pops up "BOO!"
//        POUNCE     - stalks, wiggles, lunges, snaps shut, proudly lifts the catch
//   - routines can repeat a block of steps (loopFrom / loopTo / loops)
//   - per-joint peak speed limits, so no joint ever runs near full motor speed
//   - minimum-jerk easing (very soft start and stop)
//   - live mode (web page) uses acceleration-limited smoothing
//   - one master speed knob  (command: SPEED 0.6)
// =====================================================

#define SDA_PIN 21
#define SCL_PIN 22
#define PCA_ADDRESS 0x41          // your older sketch used 0x40 - match your board

// Optional: wire PCA9685 OE to this GPIO (+ 10k pull-up from OE to 3.3V) so outputs
// stay off while the ESP32 boots. Set to -1 if OE is not wired.
const int OE_PIN = -1;
const unsigned long STAGGER_MS = 250;   // used when OE is not wired

// Set to false if you want the old behaviour (go straight to HOME at power-on).
const bool STRETCH_ON_STARTUP = true;

Adafruit_PWMServoDriver pca = Adafruit_PWMServoDriver(PCA_ADDRESS);

// ---------------- Joints ----------------
enum { BASE = 0, SHOULDER = 1, ELBOW = 2, GRIPPER = 3 };
const int NUM_SERVOS = 4;
const int channels[NUM_SERVOS] = {0, 1, 2, 3};   // joint index -> PCA9685 channel
const int SERVO_FREQ = 50;

// ---------------- Servo calibration (from your old sketch) ----------------
const int MIN_PULSE_US[NUM_SERVOS] = {600, 600, 600, 600};
const int MAX_PULSE_US[NUM_SERVOS] = {2300, 2300, 2300, 2300};

// servo angle = SERVO_OFFSET + SERVO_DIRECTION * joint angle (degrees)
// Base:     joint angle = turn left of +x.          90 = pointing forward
// Shoulder: joint angle = upper arm above horizontal. 90 = upper arm horizontal
// Elbow:    joint angle = forearm bent up from the upper arm's line. 0 = fully straight
const float SERVO_OFFSET[NUM_SERVOS]   = {90, 90, 0, 90};
const int   SERVO_DIRECTION[NUM_SERVOS] = {1, 1, 1, 1};

const float GRIP_OPEN  = 40;
const float GRIP_CLOSE = 100;

// ---------------- Arm dimensions (cm) ----------------
// (the web page uses mm: Base 43, L1 64, L2 72 - keep them identical)
const float BASE_HEIGHT = 4.3;    // table to shoulder axis
const float LINK1 = 6.4;          // shoulder -> elbow
const float LINK2 = 7.2;          // elbow -> end of forearm (the point XYZ targets)

// ---------------- Poses & limits ----------------
// REST = pose the arm physically sits in at power-on (no pulse jump if it matches).
// HOME = pose it eases to after startup and on "home".
const float REST_ANGLE[NUM_SERVOS] = {90, 90, 90, 90};
const float HOME_ANGLE[NUM_SERVOS] = {90, 90, 90, 90};
// Safe servo range per joint - tighten these to where your arm really stops.
// (the web page uses the same limits in its MAP.min / MAP.max)
const float MIN_ANGLE[NUM_SERVOS] = {0,   5,   5,   30};
const float MAX_ANGLE[NUM_SERVOS] = {180, 175, 175, 110};

// ---------------- Motion feel ----------------
// PEAK speed each joint may ever reach in an eased move (degrees/second).
// The heavy joints (base, shoulder) are kept slower than the light ones (elbow, gripper).
// These are ceilings, nowhere near the servo's real top speed.
const float JOINT_PEAK_DPS[NUM_SERVOS]   = {60, 50, 60, 90};

// Speed used by normal commands (XYZ, ANGLES, HOME, GRIP...), as a fraction of the
// peak limits above. 1.0 = the limits, 0.5 = half as fast.
// A scale ABOVE 1.0 is a "quick reaction" (used by the playful routines): it goes
// faster than the normal limits and allows moves shorter than MIN_MOVE_MS.
const float DEFAULT_SPEED_SCALE = 0.8f;

// Minimum-jerk easing reaches a peak speed of 1.875 x the average speed.
const float PEAK_FACTOR = 1.875f;

const unsigned long MIN_MOVE_MS = 700;         // tiny moves are never snappy
const unsigned long QUICK_MIN_MOVE_MS = 200;   // ...unless the speed scale is above 1.0
const unsigned long MAX_MOVE_MS = 20000;       // safety only; long slow sweeps are allowed

// Live mode (web page "T" targets): smooth follower with a speed and acceleration limit.
const float JOINT_LIVE_DPS[NUM_SERVOS]    = {60, 50, 60, 90};      // deg/s
const float JOINT_ACCEL_DPS2[NUM_SERVOS]  = {160, 130, 160, 300};  // deg/s^2

const unsigned long UPDATE_MS = 20;        // 50 Hz

// Master speed knob (command: SPEED 0.6). Scales every movement, 0.2 .. 1.0
float masterSpeed = 1.0f;

// ---------------- Routines ----------------
// Each step: servo angles {base, shoulder, elbow, gripper}, speed scale (see above),
// pause after arriving (ms). Elbow 5 = (almost) fully straight (your minimum limit).
// Edit, add or remove steps freely. Steps that would push the arm into the table
// are skipped automatically (see poseSafe()).
struct SeqStep
{
  float angle[NUM_SERVOS];
  float speedScale;
  unsigned long pauseMs;
};

// A routine = a table of steps + the speed of the final glide to HOME.
// Optional repeat: steps loopFrom..loopTo (inclusive, 0-based) are played `loops`
// times in total. Use loops = 1 for "no repeat".
struct Routine
{
  const char *name;
  const SeqStep *steps;
  int count;
  float homeScale;
  int loopFrom;
  int loopTo;
  int loops;
};

// ---- STRETCH: reach in different directions like waking up ----
const SeqStep STRETCH[] = {
  //  base shoulder elbow grip    scale   pause
  { { 90,   90,     5,    40 },   0.55f,   700 },   // reach straight forward
  { { 90,  168,     5,    40 },   0.60f,  1000 },   // stretch up high
  { { 90,   95,     5,    40 },   0.60f,   300 },   // come back down to forward
  { { 165, 100,     5,    40 },   0.60f,   800 },   // reach out to the left
  { { 15,  100,     5,    40 },   0.75f,   800 },   // sweep across and reach right
  { { 90,   90,     5,    40 },   0.60f,   400 },   // back to centre, forward
  { { 90,  160,     5,   100 },   0.60f,   350 },   // up again, close the gripper (flex)
  { { 90,  160,     5,    40 },   0.70f,   400 },   // open the gripper
};

// ---- STARTLED: sudden jump back, shiver, frozen, then creeps back warily ----
const SeqStep STARTLED[] = {
  //  base shoulder elbow grip    scale   pause
  { { 120, 125,   105,   30 },   2.50f,   300 },   // jump back, gripper snaps wide open
  { { 112, 125,   105,   30 },   2.00f,     0 },   // shiver...
  { { 128, 125,   105,   30 },   2.00f,     0 },
  { { 113, 125,   105,   30 },   2.00f,     0 },
  { { 127, 125,   105,   30 },   2.00f,     0 },
  { { 120, 125,   105,   30 },   2.00f,   900 },   // frozen, tense
  { { 110, 110,    85,   36 },   0.50f,   800 },   // creeping back, wary
  { { 95,   95,    80,   40 },   0.40f,   500 },   // leans in to check what it was
};

// ---- PLAY DEAD: stagger, collapse, last twitch, lie limp, groggy revive ----
// (torque stays ON the whole time - the "limp" pose is just a low, slumped pose)
const SeqStep PLAYDEAD[] = {
  //  base shoulder elbow grip    scale   pause
  { { 90,  115,    70,   35 },   0.60f,   400 },   // rears back: "ugh..."
  { { 78,  105,    80,   45 },   0.60f,     0 },   // staggers left
  { { 102, 100,    85,   40 },   0.60f,     0 },   // staggers right
  { { 70,   60,    30,   70 },   1.80f,   300 },   // collapses
  { { 70,   60,    30,   50 },   1.50f,   150 },   // last twitch of the gripper
  { { 70,   60,    30,   70 },   1.50f,  2500 },   // limp. Dead.
  { { 85,   75,    50,   60 },   0.30f,   700 },   // groggy revive
  { { 90,   92,    80,   45 },   0.40f,   300 },   // shakes it off
};

// ---- WAVE: raise the arm, swing side to side (steps 1-2 repeat), settle ----
const SeqStep WAVE[] = {
  //  base shoulder elbow grip    scale   pause
  { { 90,  125,    55,   40 },   0.80f,   200 },   // 0 raise the arm, forearm upright
  { { 105, 125,    60,   40 },   1.60f,     0 },   // 1 swing left   (loop start)
  { { 75,  125,    50,   40 },   1.60f,     0 },   // 2 swing right  (loop end)
  { { 90,  125,    55,   40 },   1.00f,   300 },   // 3 centre
};

// ---- NOD: "yes" - forearm bobs forward and back (steps 0-1 repeat) ----
const SeqStep NOD[] = {
  //  base shoulder elbow grip    scale   pause
  { { 90,   90,    72,   40 },   1.50f,    60 },   // 0 nod down   (loop start)
  { { 90,   90,    98,   40 },   1.50f,    60 },   // 1 head up    (loop end)
  { { 90,   90,    90,   40 },   0.80f,   300 },   // 2 settle
};

// ---- DANCE: sway + bob + gripper snap on the beat (steps 1-4 repeat), ta-da ----
const SeqStep DANCE[] = {
  //  base shoulder elbow grip    scale   pause
  { { 90,  100,    80,   40 },   0.70f,   100 },   // 0 get ready
  { { 72,  108,    72,   40 },   1.50f,     0 },   // 1 left, up        (loop start)
  { { 90,   92,    88,   70 },   1.50f,     0 },   // 2 centre, down, snap
  { { 108, 108,    72,   40 },   1.50f,     0 },   // 3 right, up
  { { 90,   92,    88,   70 },   1.50f,     0 },   // 4 centre, down, snap (loop end)
  { { 90,  125,    60,   40 },   0.80f,   600 },   // 5 ta-da! arms up
};

// ---- PEEKABOO: hide, creep up to peek, duck again, then pop up "BOO!" ----
const SeqStep PEEKABOO[] = {
  //  base shoulder elbow grip    scale   pause
  { { 90,   62,    28,   45 },   1.30f,  1000 },   // 0 duck down, hide
  { { 90,   78,    72,   45 },   0.40f,   600 },   // 1 slowly rise to peek
  { { 90,   62,    28,   45 },   1.80f,   800 },   // 2 "oh!" ducks back down
  { { 90,  120,    60,   35 },   2.50f,   600 },   // 3 BOO! pops up
  { { 105, 120,    60,   35 },   1.60f,     0 },   // 4 happy wiggle
  { { 75,  120,    60,   35 },   1.60f,     0 },   // 5
  { { 90,  120,    60,   35 },   1.20f,   400 },   // 6
};

// ---- POUNCE: stalk, wiggle (steps 1-2 repeat), lunge, snap, lift the catch ----
const SeqStep POUNCE[] = {
  //  base shoulder elbow grip    scale   pause
  { { 90,   66,    50,   35 },   0.40f,   900 },   // 0 crouch slowly, stalking
  { { 80,   66,    50,   35 },   1.20f,     0 },   // 1 wiggle left   (loop start)
  { { 100,  66,    50,   35 },   1.20f,     0 },   // 2 wiggle right  (loop end)
  { { 90,   66,    50,   35 },   1.00f,   500 },   // 3 lock on target
  { { 90,   82,     5,   35 },   2.50f,     0 },   // 4 LUNGE forward
  { { 90,   82,     5,  100 },   2.50f,   900 },   // 5 snap the gripper shut!
  { { 90,  100,    60,  100 },   0.60f,   600 },   // 6 proudly lifts the catch
  { { 90,  100,    60,   40 },   1.00f,   300 },   // 7 drops it, bored
};

#define STEPS_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

//                                name                  steps     count               home   lf lt loops
const Routine R_STRETCH  = { "Stretching...",           STRETCH,  STEPS_OF(STRETCH),  0.50f, 0, 0, 1 };
const Routine R_STARTLED = { "Startled!",               STARTLED, STEPS_OF(STARTLED), 0.40f, 0, 0, 1 };
const Routine R_PLAYDEAD = { "Playing dead...",         PLAYDEAD, STEPS_OF(PLAYDEAD), 0.40f, 0, 0, 1 };
const Routine R_WAVE     = { "Waving hello!",           WAVE,     STEPS_OF(WAVE),     0.60f, 1, 2, 3 };
const Routine R_NOD      = { "Nodding yes.",            NOD,      STEPS_OF(NOD),      0.60f, 0, 1, 3 };
const Routine R_DANCE    = { "Dancing!",                DANCE,    STEPS_OF(DANCE),    0.60f, 1, 4, 4 };
const Routine R_PEEKABOO = { "Peekaboo...",             PEEKABOO, STEPS_OF(PEEKABOO), 0.50f, 0, 0, 1 };
const Routine R_POUNCE   = { "Stalking...",             POUNCE,   STEPS_OF(POUNCE),   0.50f, 1, 2, 3 };

// ---------------- State ----------------
float curAngle[NUM_SERVOS];
float startAngle[NUM_SERVOS];
float targetAngle[NUM_SERVOS];
unsigned long moveStartMs[NUM_SERVOS];
unsigned long moveDurMs[NUM_SERVOS];
bool moving[NUM_SERVOS];
int lastPulse[NUM_SERVOS];

bool busy = false;
unsigned long lastUpdateMs = 0;
String lineBuf;

// live (streamed) mode, used by the web page
bool live = false;
float liveTarget[NUM_SERVOS];
float liveVel[NUM_SERVOS];

// routine sequencer state
bool seqActive = false;
const Routine *seqRoutine = nullptr;
int seqIndex = 0;
int seqPhase = 0;                    // 0 = start next step, 1 = moving, 2 = pausing
int seqLoopsDone = 0;
unsigned long seqPauseUntil = 0;

// ---------------- Motion engine ----------------
// Minimum-jerk ("smootherstep") easing: zero speed AND zero acceleration at both ends.
float easeInOut(float t)
{
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

void writeAngle(int i, float angle)
{
  float frac = angle / 180.0f;
  int pulse = (int)(MIN_PULSE_US[i] + (MAX_PULSE_US[i] - MIN_PULSE_US[i]) * frac + 0.5f);
  if (pulse == lastPulse[i]) return;
  pca.writeMicroseconds(channels[i], pulse);
  lastPulse[i] = pulse;
}

// Start a move for the joints flagged in `involved`.
// All involved joints share one duration so they start and arrive together.
// The duration is chosen so that the slowest-limited joint never exceeds its
// peak speed (JOINT_PEAK_DPS * speedScale * masterSpeed).
void planMove(const float targets[], const bool involved[], float speedScale = DEFAULT_SPEED_SCALE)
{
  live = false;                        // a normal eased move cancels live streaming
  float maxDist = 0;
  float needSec = 0;
  float clamped[NUM_SERVOS];

  for (int i = 0; i < NUM_SERVOS; i++)
  {
    if (!involved[i]) continue;
    clamped[i] = constrain(targets[i], MIN_ANGLE[i], MAX_ANGLE[i]);
    if (fabsf(clamped[i] - targets[i]) > 0.01f)
    {
      Serial.print("Joint ");
      Serial.print(i);
      Serial.print(" limited to ");
      Serial.print(MIN_ANGLE[i], 0);
      Serial.print("-");
      Serial.print(MAX_ANGLE[i], 0);
      Serial.println(" (clamped)");
    }
    float d = fabsf(clamped[i] - curAngle[i]);
    if (d > maxDist) maxDist = d;

    float peak = JOINT_PEAK_DPS[i] * speedScale * masterSpeed;
    float sec = d * PEAK_FACTOR / peak;
    if (sec > needSec) needSec = sec;
  }

  if (maxDist < 0.5f)
  {
    if (!seqActive) Serial.println("Already there.");
    return;
  }

  unsigned long minMs = (speedScale > 1.0f) ? QUICK_MIN_MOVE_MS : MIN_MOVE_MS;
  unsigned long dur = (unsigned long)(needSec * 1000.0f);
  dur = constrain(dur, minMs, MAX_MOVE_MS);
  unsigned long now = millis();

  for (int i = 0; i < NUM_SERVOS; i++)
  {
    if (!involved[i]) continue;
    startAngle[i]  = curAngle[i];
    targetAngle[i] = clamped[i];
    moveStartMs[i] = now;
    moveDurMs[i]   = dur;
    moving[i]      = true;
  }
  busy = true;
}

void updateMotion()
{
  unsigned long now = millis();
  if (now - lastUpdateMs < UPDATE_MS) return;
  float dt = (float)(now - lastUpdateMs) / 1000.0f;
  if (dt > 0.05f) dt = 0.05f;
  lastUpdateMs = now;

  // live mode: every joint chases the latest streamed target with limited speed and
  // acceleration, and brakes early enough to stop smoothly on the target.
  if (live)
  {
    for (int i = 0; i < NUM_SERVOS; i++)
    {
      float err = liveTarget[i] - curAngle[i];
      float vmax = JOINT_LIVE_DPS[i] * masterSpeed;
      float amax = JOINT_ACCEL_DPS2[i] * masterSpeed;

      float vWant = sqrtf(2.0f * amax * fabsf(err));   // fastest speed that can still stop in time
      if (vWant > vmax) vWant = vmax;
      if (err < 0) vWant = -vWant;

      float dvMax = amax * dt;
      liveVel[i] += constrain(vWant - liveVel[i], -dvMax, dvMax);

      float next = curAngle[i] + liveVel[i] * dt;
      bool crossed = (err > 0 && next > liveTarget[i]) || (err < 0 && next < liveTarget[i]);
      if (crossed || (fabsf(err) < 0.05f && fabsf(liveVel[i]) < 1.0f))
      {
        next = liveTarget[i];
        liveVel[i] = 0;
      }

      if (next != curAngle[i])
      {
        curAngle[i] = next;
        writeAngle(i, curAngle[i]);
      }
    }
    return;
  }

  if (!busy) return;

  bool anyMoving = false;
  for (int i = 0; i < NUM_SERVOS; i++)
  {
    if (!moving[i]) continue;

    float t = (float)(now - moveStartMs[i]) / (float)moveDurMs[i];
    if (t >= 1.0f) { t = 1.0f; moving[i] = false; }
    else           { anyMoving = true; }

    curAngle[i] = startAngle[i] + (targetAngle[i] - startAngle[i]) * easeInOut(t);
    writeAngle(i, curAngle[i]);
  }

  if (!anyMoving)
  {
    busy = false;
    if (!seqActive) Serial.println("Motion complete.");
  }
}

void goHome(float speedScale = DEFAULT_SPEED_SCALE)
{
  Serial.println("Easing to HOME...");
  bool all[NUM_SERVOS];
  for (int i = 0; i < NUM_SERVOS; i++) all[i] = true;
  planMove(HOME_ANGLE, all, speedScale);
}

// ---------------- Routine sequencer ----------------
// True if the pose keeps both the elbow and the forearm tip above the table.
// (Only the forearm end is checked - the gripper's own length is not modelled,
// so leave a little margin in low poses.)
bool poseSafe(const float a[NUM_SERVOS])
{
  float a1 = (a[SHOULDER] - SERVO_OFFSET[SHOULDER]) * SERVO_DIRECTION[SHOULDER] * DEG_TO_RAD;
  float e  = (a[ELBOW]    - SERVO_OFFSET[ELBOW])    * SERVO_DIRECTION[ELBOW]    * DEG_TO_RAD;
  float elbowZ = BASE_HEIGHT + LINK1 * sinf(a1);
  float tipZ   = elbowZ + LINK2 * sinf(a1 + e);
  return elbowZ >= 0.5f && tipZ >= 0.5f;
}

void startRoutine(const Routine &r)
{
  live = false;
  seqRoutine = &r;
  seqActive = true;
  seqIndex = 0;
  seqPhase = 0;
  seqLoopsDone = 0;
  Serial.println(r.name);
}

void cancelSequence()
{
  if (!seqActive) return;
  seqActive = false;
  Serial.println("Routine cancelled.");
}

void finishRoutine()
{
  seqActive = false;
  Serial.println("Routine done.");
  goHome(seqRoutine->homeScale);
}

// Move on to the next step, or jump back to the start of the repeat block.
void advanceStep()
{
  if (seqRoutine->loops > 1 &&
      seqIndex == seqRoutine->loopTo &&
      seqLoopsDone < seqRoutine->loops - 1)
  {
    seqLoopsDone++;
    seqIndex = seqRoutine->loopFrom;
  }
  else
  {
    seqIndex++;
  }
}

// Runs from loop(); never blocks, so serial commands (STOP etc.) still work mid-routine.
void updateSequence()
{
  if (!seqActive) return;
  unsigned long now = millis();

  if (seqPhase == 1)                       // waiting for the current move to finish
  {
    if (busy) return;
    seqPhase = 2;
    seqPauseUntil = now + seqRoutine->steps[seqIndex].pauseMs;
    return;
  }

  if (seqPhase == 2)                       // holding the pose for a moment
  {
    if (now < seqPauseUntil) return;
    advanceStep();
    seqPhase = 0;
  }

  // phase 0: start the next step
  if (seqIndex >= seqRoutine->count)
  {
    finishRoutine();
    return;
  }

  const SeqStep &s = seqRoutine->steps[seqIndex];
  if (!poseSafe(s.angle))
  {
    Serial.print("Routine step ");
    Serial.print(seqIndex);
    Serial.println(" skipped (would hit the table).");
    advanceStep();
    return;
  }

  bool all[NUM_SERVOS] = {true, true, true, true};
  planMove(s.angle, all, s.speedScale);
  seqPhase = 1;
}

void stopAll()
{
  live = false;
  seqActive = false;
  for (int i = 0; i < NUM_SERVOS; i++)
  {
    moving[i] = false;
    liveVel[i] = 0;
    targetAngle[i] = curAngle[i];
  }
  busy = false;
  Serial.println("Stopped.");
}

void moveGripper(float angle)
{
  float targets[NUM_SERVOS] = {0, 0, 0, angle};
  bool involved[NUM_SERVOS] = {false, false, false, true};
  planMove(targets, involved);
}

void printPositions()
{
  const char *names[NUM_SERVOS] = {"Base", "Shoulder", "Elbow", "Gripper"};
  for (int i = 0; i < NUM_SERVOS; i++)
  {
    Serial.print(names[i]);
    Serial.print(": ");
    Serial.print(curAngle[i], 1);
    Serial.println(" deg");
  }
}

// ---------------- Kinematics ----------------
// Coordinates in cm. Origin = centre of the base on the table.
// x = forward, y = left, z = up. The target is the end of the forearm.

bool solveIK(float x, float y, float z, float out[3], bool verbose = true)
{
  float baseDeg = atan2f(y, x) * RAD_TO_DEG;
  float r = sqrtf(x * x + y * y);
  float h = z - BASE_HEIGHT;
  float d2 = r * r + h * h;
  float d = sqrtf(d2);

  float minReach = fabsf(LINK1 - LINK2);
  float maxReach = LINK1 + LINK2;

  if (d > maxReach || d < minReach || d < 0.001f)
  {
    if (verbose)
    {
      Serial.println("Can't reach that point - outside the arm's reach.");
      Serial.print("Distance from shoulder: ");
      Serial.print(d, 2);
      Serial.print(" cm, reachable range ");
      Serial.print(minReach, 2);
      Serial.print(" to ");
      Serial.print(maxReach, 2);
      Serial.println(" cm");
    }
    return false;
  }

  // Elbow bend: 0 = fully straight (elbow-down solution, as in your old sketch)
  float cosElbow = (d2 - LINK1 * LINK1 - LINK2 * LINK2) / (2.0f * LINK1 * LINK2);
  cosElbow = constrain(cosElbow, -1.0f, 1.0f);
  float elbow = acosf(cosElbow);

  float shoulder = atan2f(h, r) - atan2f(LINK2 * sinf(elbow), LINK1 + LINK2 * cosf(elbow));

  float elbowZ = BASE_HEIGHT + LINK1 * sinf(shoulder);
  if (z < 0.5f || elbowZ < 0.5f)
  {
    if (verbose) Serial.println("Can't reach that point - the arm would hit the table.");
    return false;
  }

  out[0] = SERVO_OFFSET[BASE]     + SERVO_DIRECTION[BASE]     * baseDeg;
  out[1] = SERVO_OFFSET[SHOULDER] + SERVO_DIRECTION[SHOULDER] * shoulder * RAD_TO_DEG;
  out[2] = SERVO_OFFSET[ELBOW]    + SERVO_DIRECTION[ELBOW]    * elbow * RAD_TO_DEG;

  for (int i = 0; i < 3; i++)
  {
    if (out[i] < MIN_ANGLE[i] || out[i] > MAX_ANGLE[i])
    {
      if (verbose)
      {
        Serial.print("Can't reach that point - joint ");
        Serial.print(i);
        Serial.print(" would need ");
        Serial.print(out[i], 1);
        Serial.print(" deg (limit ");
        Serial.print(MIN_ANGLE[i], 0);
        Serial.print("-");
        Serial.print(MAX_ANGLE[i], 0);
        Serial.println(").");
      }
      return false;
    }
  }
  return true;
}

void moveToXYZ(float x, float y, float z)
{
  float sol[3];
  if (!solveIK(x, y, z, sol)) return;

  Serial.print("Target (");
  Serial.print(x, 1); Serial.print(", ");
  Serial.print(y, 1); Serial.print(", ");
  Serial.print(z, 1);
  Serial.print(") cm -> servos ");
  Serial.print(sol[0], 0); Serial.print(" ");
  Serial.print(sol[1], 0); Serial.print(" ");
  Serial.println(sol[2], 0);

  float targets[NUM_SERVOS] = {sol[0], sol[1], sol[2], 0};
  bool involved[NUM_SERVOS] = {true, true, true, false};   // gripper keeps its position
  planMove(targets, involved);
}

// Web page command:  T x y z grip   (x y z in MILLIMETRES, grip 0 = open .. 100 = closed)
// Streamed several times per second while you drag the target, so it uses the
// smoothed live mode instead of a fresh eased move each time.
void liveToXYZ(float xmm, float ymm, float zmm, float gripPct)
{
  float sol[3];
  if (!solveIK(xmm / 10.0f, ymm / 10.0f, zmm / 10.0f, sol, false))
  {
    Serial.println("ERR unreachable");
    return;
  }

  seqActive = false;                   // the web page takes over from any routine

  if (!live)
  {
    for (int i = 0; i < NUM_SERVOS; i++)
    {
      liveTarget[i] = curAngle[i];
      liveVel[i] = 0;
      moving[i] = false;
    }
    busy = false;
    live = true;
  }

  for (int i = 0; i < 3; i++) liveTarget[i] = constrain(sol[i], MIN_ANGLE[i], MAX_ANGLE[i]);
  gripPct = constrain(gripPct, 0.0f, 100.0f);
  liveTarget[GRIPPER] = constrain(GRIP_OPEN + (GRIP_CLOSE - GRIP_OPEN) * gripPct / 100.0f,
                                  MIN_ANGLE[GRIPPER], MAX_ANGLE[GRIPPER]);
}

// Forward kinematics from current servo angles ("where")
void printTip()
{
  float baseRad = (curAngle[BASE] - SERVO_OFFSET[BASE]) * SERVO_DIRECTION[BASE] * DEG_TO_RAD;
  float a1 = (curAngle[SHOULDER] - SERVO_OFFSET[SHOULDER]) * SERVO_DIRECTION[SHOULDER] * DEG_TO_RAD;
  float e  = (curAngle[ELBOW] - SERVO_OFFSET[ELBOW]) * SERVO_DIRECTION[ELBOW] * DEG_TO_RAD;

  float r = LINK1 * cosf(a1) + LINK2 * cosf(a1 + e);
  float z = BASE_HEIGHT + LINK1 * sinf(a1) + LINK2 * sinf(a1 + e);

  Serial.print("Forearm end at x=");
  Serial.print(r * cosf(baseRad), 1);
  Serial.print(" y=");
  Serial.print(r * sinf(baseRad), 1);
  Serial.print(" z=");
  Serial.print(z, 1);
  Serial.println(" cm");
}

// ---------------- Commands ----------------
void printHelp()
{
  Serial.println();
  Serial.println("===== ROBOT ARM COMMANDS =====");
  Serial.println("  XYZ 10 0 8        move to x y z (cm)   (GO works too)");
  Serial.println("  WHERE             print where the forearm end is now (FK works too)");
  Serial.println("  HOME              glide to the 90/90/90/90 pose");
  Serial.println("  --- routines (each ends by gliding HOME) ---");
  Serial.println("  STRETCH           wake-up stretch in different directions");
  Serial.println("  WAVE              wave hello");
  Serial.println("  NOD               nod yes");
  Serial.println("  DANCE             sway, bob and snap to a beat");
  Serial.println("  PEEKABOO          duck, peek, duck, pop up BOO");
  Serial.println("  POUNCE            stalk, wiggle, lunge, snap");
  Serial.println("  STARTLED          jump back, shiver, creep back");
  Serial.println("  PLAY DEAD         collapse, lie limp, slowly revive");
  Serial.println("  ----------------------------------------");
  Serial.println("  SPEED 0.6         master speed 0.2-1.0 (SPEED alone prints it)");
  Serial.println("  ANGLES 90 90 90 90  set base shoulder elbow gripper");
  Serial.println("  2 45              move one joint (0 base,1 shoulder,2 elbow,3 gripper)");
  Serial.println("  GRIP OPEN / GRIP CLOSE");
  Serial.println("  STOP              freeze in place (also cancels a routine)");
  Serial.println("  POS               print joint angles");
  Serial.println("  T x y z grip      live target in mm, grip 0-100 (web page)");
  Serial.println("  ?                 link check (web page)");
  Serial.println("==============================");
}

void handleLine(String s)
{
  s.trim();
  if (s.length() == 0) return;

  String u = s;
  u.toUpperCase();

  if (u == "?")     { Serial.println("READY 4-servo arm (ESP32 + PCA9685)"); return; }
  if (u == "HELP")  { printHelp(); return; }
  if (u == "STOP")  { stopAll(); return; }
  if (u == "POS")   { printPositions(); return; }
  if (u == "WHERE" || u == "FK") { printTip(); return; }

  if (u == "STRETCH")  { startRoutine(R_STRETCH);  return; }
  if (u == "WAVE")     { startRoutine(R_WAVE);     return; }
  if (u == "NOD")      { startRoutine(R_NOD);      return; }
  if (u == "DANCE")    { startRoutine(R_DANCE);    return; }
  if (u == "PEEKABOO") { startRoutine(R_PEEKABOO); return; }
  if (u == "POUNCE")   { startRoutine(R_POUNCE);   return; }
  if (u == "STARTLED") { startRoutine(R_STARTLED); return; }
  if (u == "PLAY DEAD" || u == "PLAYDEAD") { startRoutine(R_PLAYDEAD); return; }

  if (u == "SPEED")
  {
    Serial.print("Master speed: ");
    Serial.println(masterSpeed, 2);
    return;
  }
  if (u.startsWith("SPEED "))
  {
    float v;
    char extra;
    if (sscanf(u.c_str() + 6, "%f %c", &v, &extra) == 1 && isfinite(v))
    {
      masterSpeed = constrain(v, 0.2f, 1.0f);
      Serial.print("Master speed set to ");
      Serial.println(masterSpeed, 2);
    }
    else
    {
      Serial.println("Usage: SPEED 0.6   (0.2 - 1.0)");
    }
    return;
  }

  if (u == "HOME")       { cancelSequence(); goHome(); return; }
  if (u == "GRIP OPEN")  { cancelSequence(); moveGripper(GRIP_OPEN);  return; }
  if (u == "GRIP CLOSE") { cancelSequence(); moveGripper(GRIP_CLOSE); return; }

  if (u.startsWith("T "))
  {
    float x, y, z, g;
    char extra;
    if (sscanf(u.c_str() + 2, "%f %f %f %f %c", &x, &y, &z, &g, &extra) == 4 &&
        isfinite(x) && isfinite(y) && isfinite(z) && isfinite(g))
    {
      liveToXYZ(x, y, z, g);
    }
    else
    {
      Serial.println("Usage: T x y z grip   (mm, grip 0-100)");
    }
    return;
  }

  if (u.startsWith("XYZ ") || u.startsWith("GO "))
  {
    const char *args = u.c_str() + (u.startsWith("XYZ ") ? 4 : 3);
    float x, y, z;
    char extra;
    if (sscanf(args, "%f %f %f %c", &x, &y, &z, &extra) == 3 &&
        isfinite(x) && isfinite(y) && isfinite(z))
    {
      cancelSequence();
      moveToXYZ(x, y, z);
    }
    else
    {
      Serial.println("Usage: XYZ x y z   e.g. XYZ 10 0 8");
    }
    return;
  }

  if (u.startsWith("ANGLES "))
  {
    float a[NUM_SERVOS];
    char extra;
    if (sscanf(u.c_str() + 7, "%f %f %f %f %c", &a[0], &a[1], &a[2], &a[3], &extra) == 4 &&
        isfinite(a[0]) && isfinite(a[1]) && isfinite(a[2]) && isfinite(a[3]))
    {
      cancelSequence();
      bool all[NUM_SERVOS] = {true, true, true, true};
      planMove(a, all);
    }
    else
    {
      Serial.println("Usage: ANGLES 90 90 90 90");
    }
    return;
  }

  int joint, angle;
  char extra;
  if (sscanf(u.c_str(), "%d %d %c", &joint, &angle, &extra) != 2)
  {
    Serial.println("Unknown command. Type HELP.");
    return;
  }
  if (joint < 0 || joint >= NUM_SERVOS)
  {
    Serial.println("Joint must be between 0 and 3");
    return;
  }

  cancelSequence();
  float targets[NUM_SERVOS] = {0, 0, 0, 0};
  bool involved[NUM_SERVOS] = {false, false, false, false};
  targets[joint] = angle;
  involved[joint] = true;
  planMove(targets, involved);
}

void readSerial()
{
  while (Serial.available())
  {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r')
    {
      if (lineBuf.length() > 0)
      {
        handleLine(lineBuf);
        lineBuf = "";
      }
    }
    else if (lineBuf.length() < 64)
    {
      lineBuf += c;
    }
  }
}

// ---------------- Arduino ----------------
void setup()
{
  if (OE_PIN >= 0)
  {
    pinMode(OE_PIN, OUTPUT);
    digitalWrite(OE_PIN, HIGH);   // HIGH = PCA9685 outputs off
  }

  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);

  Serial.println();
  Serial.println("ESP32 + PCA9685 4-servo arm controller");

  Wire.beginTransmission(PCA_ADDRESS);
  if (Wire.endTransmission() != 0)
  {
    Serial.println("ERROR: PCA9685 not detected. Check SDA, SCL, power, GND and I2C address.");
    while (true) delay(1000);
  }

  pca.begin();
  pca.setOscillatorFrequency(27000000);
  pca.setPWMFreq(SERVO_FREQ);

  // The PCA9685 keeps old pulses across an ESP32 reset, so clear every channel first.
  for (int ch = 0; ch < 16; ch++) pca.setPWM(ch, 0, 4096);
  delay(100);

  for (int i = 0; i < NUM_SERVOS; i++)
  {
    curAngle[i] = REST_ANGLE[i];
    startAngle[i] = REST_ANGLE[i];
    targetAngle[i] = REST_ANGLE[i];
    liveTarget[i] = REST_ANGLE[i];
    liveVel[i] = 0;
    moving[i] = false;
    lastPulse[i] = -1;
  }

  if (OE_PIN >= 0)
  {
    for (int i = 0; i < NUM_SERVOS; i++) writeAngle(i, curAngle[i]);
    delay(300);
    digitalWrite(OE_PIN, LOW);
  }
  else
  {
    for (int i = 0; i < NUM_SERVOS; i++)
    {
      writeAngle(i, curAngle[i]);
      delay(STAGGER_MS);
    }
  }
  delay(800);

  printHelp();

  if (STRETCH_ON_STARTUP) startRoutine(R_STRETCH);   // stretch first, then it goes HOME by itself
  else                    goHome();
}

void loop()
{
  readSerial();
  updateMotion();
  updateSequence();
}
