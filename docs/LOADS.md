# Driving motors, relays and other loads from a Log computer

**Read this before connecting anything bigger than an LED.**
This guide was written without access to hardware. The circuits are standard textbook designs, but **none of them was built or measured by the author**:
check every value against the datasheets of the parts you actually buy. The Log programs in `docs/examples/` compile and run in the virtual and
`--fake-gpio` modes; they were never run on a real board.

## The rules that matter

1. **A GPIO pin carries a signal, not power.** A Raspberry Pi pin is 3.3 V and should supply only a few milliamps (treat 16 mA per pin and 50 mA for all pins together as the
   ceiling). A motor, a relay coil, a solenoid, a pump, a fan, a heater or an LED strip must never be connected directly to a pin. A pin must never move a large motor.
2. **The load gets its own power supply**, sized for the load's *worst* current (a stalled motor draws several times its running current).
   Never power a motor from the 5 V or 3.3 V pins of the board.
3. **Common ground.** The negative of the load supply and the GND of the board must be connected, or the control signal has no reference.
4. **Inductive loads need a flyback diode** (motors, relay coils, solenoids). When the current is cut they produce a voltage spike that can destroy the transistor and the board.
5. **The load must be OFF when the program is not in control**: while the board boots, if the program crashes, if a wire comes loose. Use a pull-down resistor on the
   control line (hardware) **and** `Pin:N("Safe;Low")` plus `Watchdog` (software). A pin keeps its last level after the program dies.
6. **Mains voltage (110/230 V) can kill.** Do not wire it unless you are qualified. Never connect a GPIO pin to it. Use only certified, enclosed modules, with a fuse and
   an RCD/GFCI, and keep it out of reach. If in doubt, switch the *low-voltage* side of a certified smart plug or contactor instead.
7. **Start low.** First test with a lower supply voltage and a small load, check polarity twice, keep an emergency switch that cuts the load supply, and watch for heat.

What Log does for you, and what it cannot do: `Safe;Low` and `Watchdog` drive the marked pins to a safe level when the program ends, fails, is stopped with Ctrl+C / `kill`,
or stops calling `Watchdog("Feed")`. They **cannot** help if the whole computer freezes, loses power, or the process is killed with `kill -9`. Only the hardware (the pull-down,
a fuse, a thermal cutoff, a physical emergency stop) covers those cases.

## 1. LED (the only load you may connect to a pin directly)

```
 G13 ───[ 330 Ω ]───▶|───── GND          (LED: long leg toward the resistor)
```
Resistor = (3.3 V − LED voltage) / current. A red LED (about 2 V) at 5 mA: (3.3 − 2) / 0.005 = 260 Ω, so 330 Ω is a safe standard value.

## 2. Motor, fan, pump, solenoid or relay coil: N-MOSFET switch (low side)

```
   +V (load supply, for example 6-12 V)
     │
     ├──────────────┐
     │              │
  [ LOAD ]       ──▶|──   flyback diode, across the load, stripe (cathode) toward +V
     │              │
     ├──────────────┘
     │ Drain
     │
 G18 ──[ 220 Ω ]──┬── Gate    N-channel MOSFET
                  │    │ Source
               [10 kΩ] │
                  │    │
 GND (supply −) ──┴────┴────── Pi GND   (common ground)
```
* **220 Ω** in series with the gate limits the current spike when the gate charges. **10 kΩ gate-to-source** keeps the MOSFET OFF while the pin floats (boot, crash).
* **Diode:** 1N4007 or a Schottky such as 1N5819 for small loads; it must be rated above the load current.
* **Choosing the MOSFET:** "logic level" is not enough. The datasheet must give **Rds(on) at Vgs = 3.3 V or lower** (many "logic level" parts only specify it at 4.5 V or 5 V).
  Vds rating at least twice the supply, current at least twice the load, and check its heat at that current (Rds(on) × I²). For loads up to a few amps, a ready-made
  MOSFET driver module is easier and safer than building it.
* **PWM:** with `Pin:18("PWM;500","Duty;60")` the MOSFET gives 60 % of the power: motor speed, LED strip brightness, heater power. Software PWM on real pins
  (up to 1000 Hz) has jitter of tens or hundreds of microseconds: fine for motors, fans and LEDs.
* Example: `docs/examples/motor_mosfet.log++`.

## 3. Relay module

Use a **ready-made relay module** (with a driver transistor and, better, an optocoupler), not a bare relay on a pin: the coil needs tens of milliamps.
Supply the module's coil from its own 5 V source if the datasheet asks for it; many modules are **active-low** (the relay closes when the pin is LOW):
test it with no load first and write the program for the behaviour you measured, with `Safe` set to the level that releases the relay.
Relays are for slow switching (a few times a minute), never for PWM.
The contacts side is the dangerous one: see rule 6 if it carries mains.

## 4. Reversing a motor: H-bridge driver board

```
              ┌────────────────────────┐
  G18  ─────▶ │ PWM / EN        OUT A  │──┐
  G23  ─────▶ │ IN1                    │  ├── motor
  G24  ─────▶ │ IN2             OUT B  │──┘
  3.3 V ────▶ │ logic supply (VCC)     │
  GND  ─────▶ │ GND                    │
  Motor + ──▶ │ VM (motor supply)      │
  Motor − ──▶ │ GND ◀── also to Pi GND │
              └────────────────────────┘
```
Names differ between boards (TB6612, DRV8833, L298N, BTS7960 for big currents): follow your board's datasheet. Typical logic: IN1 high + IN2 low = forward,
IN1 low + IN2 high = backward, both low = stop; speed = PWM on the enable pin. Put **all** the control pins `Safe;Low`, and check what "all low" does on *your* board
(coast or brake). Choose a driver whose continuous current is at least twice the motor's stall current, add a **100 µF capacitor across the motor supply near the driver**
and a 100 nF ceramic capacitor across the motor terminals. Example: `docs/examples/hbridge.log++`.

## 5. Hobby servo

A servo needs a **separate 5 V (or the voltage on its label) supply** that can deliver an amp or more, with common ground; the signal wire goes to a pin
(the signal is 3.3 V, which most servos accept). Position = pulse width every 20 ms: 1 ms, 1.5 ms and 2 ms are duty 5 %, 7.5 % and 10 % at 50 Hz.
**Software PWM jitters, so the servo will tremble or twitch** (`docs/examples/servo.log++`). For a steady servo use a hardware PWM output or, better, a PCA9685 board
(it speaks I2C: `I2C:Write`); Log has no ready-made PCA9685 instruction yet.

## 6. Power and wiring

* Supply rated at least twice the largest current you expect, with a **fuse** in the + line close to the supply.
* Wire thick enough for the current (a motor drawing 5 A needs far more than jumper wire) and short where it carries PWM.
* Join the grounds at one point; keep motor wiring away from signal wires.
* Mount power parts so they can lose their heat; touch-test with a finger only on low-voltage circuits, briefly, and after switching the supply off.

## Before you switch it on

- [ ] The load supply is not connected to any 3.3 V / 5 V pin of the board.
- [ ] Common ground is connected.
- [ ] Flyback diode present and the right way round (stripe toward +).
- [ ] Pull-down on every control line; `Safe` set in the program; `Watchdog` armed.
- [ ] Polarity of supply, diode and capacitors double-checked. Fuse fitted.
- [ ] The first test is at low voltage with the load disconnected or unloaded; an emergency switch cuts the load supply.
- [ ] Nothing carrying mains voltage is exposed.
