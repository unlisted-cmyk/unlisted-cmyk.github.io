# A computer that starts in Log

Goal: power on a Raspberry Pi and land directly in the **Log terminal**, with no desktop, and optionally a program of yours (`autorun.log+`) starting by itself.

| | Path A: Raspberry Pi OS Lite + installer | Path B: Buildroot minimal image |
|---|---|---|
| Effort | 10 minutes | an afternoon, a Linux PC, about 20 GB free |
| Result | normal Linux with Log instead of the login prompt | tiny image (kernel + BusyBox + Log), nothing else |
| Updates | `sudo ./setup-log-os.sh` again | rebuild the image |
| Maintenance | SSH works, `apt` works | no SSH, no package manager (a shell once via the `logl-shell` file) |
| Status | installer and scripts tested on a PC; **NOT TESTED ON A REAL RASPBERRY PI** | file handling tested; **the Buildroot build and the boot were NEVER run** |

Start with Path A.

## What you get, and what you do not

* The Log terminal on the screen and keyboard (HDMI + USB), restarted automatically if you type `:quit` or if it ends.
* `autorun.log+` (or `.log++`, `.log-`, `.logall`) in the boot partition starts before the terminal. If it has event handlers (`On:Pin`, `On:Net`...) it keeps running; press Ctrl+C to get the terminal.
* Full hardware access: `--hw --allow-all` by default (edit `/etc/logl.conf` to restrict it).
* **No graphics.** logl draws windows in a web browser (`Screen`, `G3D`, `UI`, `Sound`), and this image has none, so those programs run with `--no-window` and show nothing.
  Text, pins, buses, network and events work. Showing graphics on the HDMI output would need a browser in kiosk mode: not provided.
* **Raspberry Pi 64-bit only:** Pi 3, 4, 5 (see the note below), 400, CM4, Zero 2 W with a 64-bit OS. The Pi Zero, Pi 1 and Pi 2 are 32-bit ARM and there is no logl build for them yet.
* **Raspberry Pi 5:** the operating system part works, but logl's real GPIO uses the BCM283x registers of Pi 0-4 and **does not work on the Pi 5** (it needs the kernel GPIO interface).
  I2C, SPI, UART and the network do not depend on that.
* The first program that runs after the kernel's own startup is not literally Log: systemd (Path A) or BusyBox init (Path B) starts first and launches Log on the console.
  I have not measured boot times.

## Path A: Raspberry Pi OS Lite

1. With Raspberry Pi Imager, write **Raspberry Pi OS Lite (64-bit)** to the SD card. In the advanced options set a user name and password, and enable SSH (you will want a way in for maintenance).
2. Boot the Pi, log in, and get this repository's `image` folder onto it (`git clone`, or copy it with `scp`).
3. Run:
   ```
   cd image/pi-os
   sudo bash setup-log-os.sh
   sudo reboot
   ```
   It downloads `logl-linux-arm64` from the latest release **and checks its SHA-256 against `SHA256SUMS.txt`**; if they differ nothing is installed.
   It installs `logl`, the session script and a systemd service, turns on I2C, SPI and the serial port, silences the boot messages, and replaces the login prompt on the first console.
   To use a binary you already have: `sudo bash setup-log-os.sh /path/to/logl-linux-arm64`. (Use `bash script`: files uploaded through the GitHub web page lose their executable bit.)
4. After the reboot the Pi starts in the Log terminal.

Put your program at `/boot/firmware/autorun.log+` (Raspberry Pi OS Bookworm and newer; `/boot/autorun.log+` on older versions). You can also edit files in `/home/log`.
Run `sudo bash setup-log-os.sh` again at any time to update logl.

Get a shell back: connect by SSH, or create an empty file named `logl-shell` in the boot folder (from another computer, with the SD card in it): the next start gives you a shell once.
Undo everything: `sudo systemctl disable logl-console && sudo systemctl enable getty@tty1`.

## Path B: Buildroot (a minimal image)

Requirements: a Linux PC, a Buildroot checkout (2024.02 or newer), and a release of this project with `logl-linux-arm64` and `SHA256SUMS.txt`.
```
git clone https://github.com/buildroot/buildroot && cd buildroot
cd ../image/buildroot
bash build.sh /path/to/buildroot         # downloads and verifies logl, configures and builds
```
The result is `output/images/sdcard.img` (Raspberry Pi 4, 400, CM4). Write it to an SD card.
`logl` is **statically linked with musl**, so the image needs no C library for it: the whole root file system is BusyBox plus Log.
What the scripts do: use Buildroot's `raspberrypi4_64_defconfig`, add an overlay with a BusyBox `inittab` that runs `logl-session` on the first console, build the I2C/SPI/UART
drivers into the kernel (there is no module loader), and append `dtparam=i2c_arm=on`, `dtparam=spi=on`, `enable_uart=1` to the firmware `config.txt`.
**Not tested.** Buildroot option names change between versions, the overlay is assumed to be applied after Buildroot generates its own `inittab` (if the image boots to a login prompt
this is why), and the kernel fragment may need adjusting. Treat `build.sh` as a starting point and read `logl.fragment` and `kernel.fragment` before running it.

## Settings: `/etc/logl.conf`

```
LOGL_FLAGS="--hw --allow-all --no-window"   # what every program may do
LOGL_HOME=/home/log                          # where your programs live
LOGL_AUTORUN_DIR=/boot/firmware              # where autorun.log* is looked for
```

## Safety notes

* `--allow-all` means any program can read and write any file, drive any pin and use the network. That is reasonable for a computer that only you use. If it is reachable
  from a network (an `On:Net` server, SSH), restrict it: `LOGL_FLAGS="--hw --allow-pins --no-window"` plus only the permissions the program needs.
* Cutting the power while the SD card is being written can corrupt it. For an appliance that is simply switched off, consider making the root file system read-only
  (Raspberry Pi OS: `sudo raspi-config nonint do_overlayfs 0`; untested here) and keep your programs in `/boot/firmware`.
* Motors, relays and heaters: read `docs/LOADS.md` first. A pin keeps its last level if the computer stops, so use `Pin:N("Safe;Low")`, `Watchdog` and a pull-down resistor.

## Tests that exist

`tests/session_test.py` runs `image/common/logl-session` in a pseudo-terminal (autorun, terminal, `:quit`, maintenance shell, missing binary).
`tests/image_test.py` runs the installer against a fake release (installation, SHA-256 rejection of a modified binary, running it twice, dry run that does not touch the system).
Both run on Linux in the CI workflow.
