#!/usr/bin/env bash
# Log test runner.   Usage (from the repository root):   bash tests/run.sh ./logl-linux
#                    bash tests/run.sh ./logl --update      (rewrites the .out files: review them with `git diff`!)
# Each case runs the program and compares "stdout + exit code" with tests/NAME.out.
# CR characters are removed so the same files work on Windows (text-mode stdout).
BIN="${1:?usage: bash tests/run.sh ./logl [--update]}"
UPDATE="${2:-}"
cd "$(dirname "$0")/.." || exit 2
pass=0; fail=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) HOST=windows;; *) HOST=unix;; esac

# case NAME "file" extra-flags...
run_case() {
  local name="$1" file="$2"; shift 2
  local got
  got="$( { "$BIN" "tests/$file" --no-window --plain --no-wait "$@" 2>&1 </dev/null; echo "rc=$?"; } | tr -d '\r')"
  if [ "$UPDATE" = "--update" ]; then printf '%s\n' "$got" > "tests/$name.out"; echo "updated $name"; return; fi
  if [ "$got" = "$(cat "tests/$name.out")" ]; then echo "PASS  $name"; pass=$((pass+1))
  else
    echo "FAIL  $name"; fail=$((fail+1))
    if command -v diff >/dev/null 2>&1; then diff <(printf '%s\n' "$got") "tests/$name.out" | head -8
    else echo "--- got:"; printf '%s\n' "$got" | head -8; echo "--- expected:"; head -8 "tests/$name.out"; fi   # MSYS2 has no diff
  fi
}

run_case t01_hello          t01_hello.log+
run_case t02_loop           t02_loop.log+
run_case t03_augassign      t03_augassign.log+
run_case t04_switch         t04_switch.log+
run_case t05_pin_virtual    t05_pin_virtual.log+
run_case t06_disk_inside    t06_disk_inside.log+
run_case t07_juego3d        t07_juego3d.log+
run_case t11_errors_nested  t11_errors_nested.log+
run_case e01_case_outside   e01_case_outside.log+ --check
run_case e02_undeclared_aug e02_undeclared_aug.log+
run_case t08_i2c_virtual    t08_i2c_virtual.log+
run_case t09_spi_loopback   t09_spi_loopback.log+
run_case t10_uart_loopback  t10_uart_loopback.log+
run_case e06_bus_badop      e06_bus_badop.log+ --check
run_case e03_disk_escape    e03_disk_escape.log+
run_case e03_disk_allowed   e03_disk_escape.log+ --allow-disk
# Pin virtual (no --hw) works everywhere; --hw tests only make sense where real GPIO code exists (Linux/macOS builds).
run_case e04_pin_virtual    e04_pin_noperm.log+
if [ "$HOST" = unix ]; then
  run_case e04_pin_denied    e04_pin_noperm.log+ --hw
  run_case e07_bus_noperm    e07_bus_noperm.log+ --hw
  run_case e05_admin_denied  e05_admin_run.log-  --hw
  run_case e05_admin_allowed e05_admin_run.log-  --hw --allow-raw
fi

[ "$UPDATE" = "--update" ] && exit 0
echo "---- $pass passed, $fail failed"
[ "$fail" -eq 0 ]
