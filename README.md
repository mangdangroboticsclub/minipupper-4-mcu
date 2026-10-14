# factorytest - Mini Pupper factory test firmware

This folder is only the factory test. Everything is in `main/`:

| File | What it is |
|---|---|
| `main/main.c` | the whole factory firmware (692 lines, 11 numbered sections - list at the top) |
| `main/driver_board.c/.h` | talks to the 4 AT32 servo boards over SPI |
| `main/factory.html` | the factory page, served at `/` |

## Build / flash
    cd C:\esp6\projects\factorytest
    idf.py build flash

Board number: `TESTBOARD_NUM` at the top of `main/main.c`
(1 = hotspot "testboard 1" at http://192.168.3.1). It also joins the router
"Mangdang" / "mangdang" (`WIFI_SSID` / `WIFI_PASS`, same place).
Servo board wiring variant: `SERVO_BOARD` in `main/driver_board.h`.

## A servo in main.c
    servo_go(id, 155.5, 300)          go to 155.5 deg, at most 300 mA
    servo_angle(id)                   where is it now, in degrees
    servo_limp(id)                    let go

## What the page calls
| Page | Address | main.c section |
|---|---|---|
| Go | `/api/direct` | 5 |
| 6 angle buttons (Edit buttons -> Save keeps them in the ESP32's flash) | `/api/presets` | 5 |
| RPM test (1/2/5/10 cycles) | `/api/speed` (twice per cycle) | 6 |
| Spin test (like Dynamixel no-load speed) | `/api/spin` (up, then down) | 7 |
| Torque test (reads the angle, moves 100 deg at 1400 mA, holds 2 s, back gently, then limp) | `/api/hold` | 8 |
| Live angle (in the torque test, about 3 times a second) | `/api/angle` | 8 |
| Left / Stop | `/api/walk?go=left` / `?go=stop` | 4 |

## CLI mode and walking
The robot starts in CLI mode: nothing moves at power-on; the page and the
tests move single servos. Left leaves CLI mode and walks (same leg maths and
calibration offsets as minipupperesp). Stop stands the robot up and goes back
to CLI mode. While it walks, single-servo commands are refused.
