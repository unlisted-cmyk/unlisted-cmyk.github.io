# logl 2.1 (parts 1A + 1B-buses)

Same language, same syntax. Programs written for 2.0 (including 3D games) keep working.
Verified: a 3D program renders a byte-identical PNG in 2.0 and 2.1.

## Compile and test locally

Linux (static glibc, quick local build):
    gcc -O2 -static -s -o logl logl.c -lm -lpthread
    bash tests/run.sh ./logl

Linux (static musl, same as the release; needs Docker):
    docker run --rm -v "$PWD":/src -w /src alpine:3.20 sh -c "apk add --no-cache gcc musl-dev && gcc -O2 -static -s -o logl logl.c -lm -lpthread"

macOS:
    clang -O2 -arch arm64 -arch x86_64 -o logl logl.c -lm -lpthread && strip -x logl
    bash tests/run.sh ./logl

Windows (MSYS2 UCRT64 shell):
    gcc -O2 -static -s -o logl.exe logl.c -lm -lpthread -lws2_32
    bash tests/run.sh ./logl.exe

`bash tests/run.sh ./logl --update` rewrites the expected `.out` files: review them with `git diff`.
Put `logl.c`, `tests/` and `.github/workflows/build.yml` in the repository (tests/e03 reads ../logl.c).

## New in 2.1-A

| Feature | Detail |
|---|---|
| `x += e`, `-=`, `*=`, `/=` | Rewritten to `Set:x("Value;x+(e)")`. Only lines that were syntax errors before are affected. Plain variables only (not `list[i]`). |
| `Switch(e){ Case(a,b){..} Default{..} }` | Compares with `==`. No fall-through. `Break`/`Continue` act on the enclosing loop. Works inside functions. |
| `Pin:N("Output")`, `Pin:N("Input;PullDown")` | Documented in chapter 6 but rejected by logl 2.0. Pull-up/down is accepted but NOT applied to real GPIO. |
| `pin(n)` in expressions | Documented in chapter 6 but missing in logl 2.0. |
| Permissions | See below. |

| `I2C:Write/Read/Scan/Close` | Same syntax as the simulator (`Addr`, `Byte`, `Bytes`, `Text`, `Into`) plus `Read`, `Bus`. |
| `SPI:Open/Transfer/Write/Read/Close` | `Bus`, `Cs`, `Speed`, `Mode`. |
| `UART:Open/Write/Read/Close` | `Port`, `Baud`, `Id`, `Count`, `Timeout`, `As;Text`. Up to 4 ports. |
| `n.toString(16)` | Radix support, as in the simulator (the official `i2c_scan` example needs it). |

### Buses
Without `--hw` they are virtual (as in the simulator): I2C has no devices (ack 0, empty scan, `Read` errors), SPI and UART
are loopbacks. With `--hw --allow-pins` they use `/dev/i2c-N`, `/dev/spidevB.C` and real serial ports.
Real I2C/SPI: Linux only. Real UART: Linux and macOS (not Windows yet). Example (real, on a Raspberry Pi):

    I2C:Write("Addr;0x27","Text;hi","Into;ack")     // run: logl prog.log++ --hw --allow-pins
    Print("answered: {ack}")

Status: virtual modes and the UART against a pseudo-terminal are tested. Real I2C and SPI are **NOT TESTED ON REAL HARDWARE**
(only the "device does not exist" errors were exercised). Check wiring, 3.3 V levels and pull-up resistors (I2C needs them).

## Permissions

| Flag | Allows |
|---|---|
| (none) | Files only inside `--root` (default: the program's folder), virtual pins, the local window. |
| `--allow-disk` | Files anywhere (`Host:`, `Mem:Load/Dump`, `G3D:Load/Save`, `Admin:Chmod`). |
| `--hw` | Enables the real-hardware layer (Reg, Port, Ram, Cpu, Pci, Dev, Admin, Pin). |
| `--allow-pins` | Real `Pin` access (needs `--hw` too). |
| `--allow-raw` | Writing to devices (`Dev:`) and `Admin:Run` (system commands). |
| `--allow-net` | Reserved for the network layer (part 1C). Accepted, does nothing yet. |
| `--allow-all` | disk + pins + net + raw. It does not imply `--hw`. |

A missing permission gives e.g. `Pin: permission denied (run logl with --allow-pins)`.

### Incompatible changes (intentional)
1. A program that reads/writes outside its folder now needs `--allow-disk`.
2. Real `Pin` with only `--hw` now also needs `--allow-pins`.
3. `Admin:Run` needs `--allow-raw`.

### Known holes (not closed yet)
`Use(...)`, `Exec(...)` and `Import(...)` still load source files from outside the folder. The disk check is
not atomic (a file could be swapped between check and use). On Windows symlinks/junctions are not resolved.

## Tests
`tests/run.sh` runs 20 cases (and `tests/uart_pty.py` tests the real UART code). They compare stdout + exit code. All pass on Linux x86-64 (glibc static),
also under AddressSanitizer + UBSan. They run unchanged in the workflow on every OS.

## What has NOT been verified
* Windows, macOS, musl/Alpine and ARM64 builds, and the GitHub workflow: never executed by the author.
* Real GPIO: **NOT TESTED ON REAL HARDWARE**. Only the permission check was exercised.

## Runtime dependencies
Linux: none (static). Windows: UCRT (Win 10/11) or msvcrt with the MINGW64 variant. macOS: libSystem only.
Programs with graphics or events open a window in the user's web browser, so they need one installed.

## Safety: motors, relays and other loads
A GPIO pin supplies a few milliamps (about 16 mA on a Raspberry Pi pin, 50 mA total) at 3.3 V. It cannot drive a
motor, relay coil or a strong LED directly. Use a logic-level N-MOSFET (or a transistor / driver board) with the
load on its own power supply, a flyback diode across any motor or relay coil, and a common ground between the
supply and the board. For reversing a motor use an H-bridge driver. Never connect a pin to mains voltage;
use a certified relay module and an enclosure. A program crash can leave a pin high: the watchdog (planned) is not implemented yet.
