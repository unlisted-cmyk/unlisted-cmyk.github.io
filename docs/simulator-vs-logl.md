# Simulator and logl 2.1: what is the same and what is not

The rule: **a program that uses only portable instructions prints the same in both.** `tests/sim_parity.py` runs the test programs in the simulator (headless Chromium)
and compares them with what logl printed: **23 programs, same output**. New instructions keep the same names, options and error messages (the first line of an error is identical;
the simulator adds the source line and a hint under it).

## What is simulated, and how

| Instruction | logl 2.1 (real) | Simulator (virtual) |
|---|---|---|
| `Pin:N("Set/Toggle/Read")`, `pin(n)` | virtual variables; real GPIO with `--hw --allow-pins` (Raspberry Pi 0-4) | pin bar and Circuits mode. Both print `[pin N] = v` |
| `Pin:N("PWM;f","Duty;d")` | virtual: `pin(n)` follows the clock; real: software PWM thread, up to 1000 Hz | `pin(n)` follows the clock. Circuits mode shows the **average brightness / current** (the frequency is not visible) |
| `Pin:N("Analog;v")`, `Pin:N("ADC;bits","Into;x")` | virtual: same conversion; real: Linux IIO driver counts | virtual only |
| `On:Pin(N)("Rising"\|"Falling"\|"Change")` | edges from `Set`/`Toggle` (virtual) or polling every ~50 us (real) | edges from `Set`/`Toggle` and from a **button of the Circuits mode** |
| `Pin:N("Safe;..")`, `Watchdog(...)` | same messages; also applied on `kill`, Ctrl+C and crashes | same messages; applied at the end, on an error, on Stop and when the watchdog fires |
| `I2C:Write/Scan` | no devices (ack 0, empty scan); real `/dev/i2c-N` with `--hw` | devices of the Circuits mode (LCD 0x27, OLED 0x3C) |
| `I2C:Read` | error `no answer from address 0x..` when virtual | the same error; if a Circuits-mode device answers at that address it returns zeros |
| `SPI:`, `UART:` | loopback (what you send comes back); real `/dev/spidev`, serial ports with `--hw` | loopback, identical |
| `Net:Tcp`, `Net:Udp` | **real sockets** (loopback or LAN), needs `--allow-net` | **closed world**: only sockets of the same program, plus the TCP client of the panel. Other hosts: `could not connect to host:port` |
| `Http:Get/Post`, `Net:Http`, `Net:Respond`, `On:Net("Request")` | real HTTP | the program's own server and the panel's "HTTP request" form. Other hosts cannot be reached |
| `Net:Ws`, `On:Net("WsOpen"/"WsMessage"/"WsClose")` | real WebSocket clients (browsers) | **simulated browsers** in the panel "Network (simulated)" |
| `Mqtt:`, `On:Net("Message")` | real MQTT 3.1.1 broker | **one virtual broker** for every host name (wildcards `+` `#`, retained messages, publish from the panel). No QoS, no keepalive, no disconnection |

## Other differences

| Aspect | Simulator | logl |
|---|---|---|
| Permissions (`--allow-disk/pins/net/raw`, `--hw`) | do not exist: everything works inside its virtual world | explicit; without them: `permission denied (run logl with --allow-net)` |
| Files, `Host:`, `Admin:Run` | virtual disk, kernel mode | real, confined to `--root` unless `--allow-disk` |
| Warnings | also warns at compile time about unknown options (`WARNING l.N`) | never warns about options |
| Compile message | `Compiled without errors (N warning(s)). Running...` | `Compiled with no errors (N warning(s)).` |
| Event handlers | run one at a time, in order, on the browser thread | one at a time, in order, on their own thread (tens of microseconds, soft real time) |
| TCP data | one event per `Send` | chunks of up to 4096 bytes |
| Ephemeral ports (`Port;0`) | from 49152 upwards | chosen by the operating system |
| Output of `Print` | the same text; a text with a line break is one entry in the terminal | the same text |
| 3D, UI, Sound, `Screen:` | in the browser; `pkg install` needed for some extensions | built in; windows open in your browser |

## What has not been verified
* **Nothing was tested in Safari or Firefox** (only Chromium). The simulator uses standard features, but check it on your devices.
* The network panel was tested through its programming interface and with a few clicks; the layout on a small screen was only made scrollable and collapsible, not reviewed on a phone.
* The Circuits mode shows PWM as an average: it is not a waveform. Frequencies are not simulated.
