# Universal BLE HIL peripheral

This is the nRF52 firmware used by the `universal_ble` hardware-in-the-loop
baseline and fault injection tests (FIT).

The nRF52DK advertises as `UniversalBLE-HIL` and exposes one custom GATT service.
The tests control it through BLE. USB serial is only there for Zephyr logs.

## 1. Hardware and software

- Nordic nRF52 DK with an nRF52832
- Zephyr 4.4.2
- Zephyr SDK with the `arm-zephyr-eabi` toolchain
- Python 3.12 and West
- SEGGER J-Link software
- USB connection for flashing and serial logs
- A Windows computer with Bluetooth LE, or Chrome with Web Bluetooth

The first-time setup is a little long because Zephyr needs its own workspace,
SDK, and Python environment. It is documented in [`docs/setup.md`](docs/setup.md).

## 2. Build and flash

Activate the local Python environment and enter the Zephyr workspace:

```powershell
.\.venv\Scripts\Activate.ps1
cd .zephyr-workspace
```

Then build and flash:

```powershell
west build -d ..\build
west flash -d ..\build -r jlink
```

After boot, the nrf52DK prints the HIL contract revision and starts advertising.
The matching Flutter application is in `hil/` in the `universal_ble`
repository.

Use a pristine build after changing the board, Kconfig, or CMake files:

```powershell
west build `
  -p always `
  -b nrf52dk/nrf52832 `
  -d ..\build `
  ..
```

## 3. Implemented fixture behavior

The firmware has:

- a readable value that tests can replace
- writes with and without response
- independent read-only mirrors and accepted-write counters
- notification and indication characteristics
- sequenced notification bursts with retry on temporary Zephyr buffer pressure
- scripted sequences with deliberate gaps, duplicates, or reordering
- readable CCC subscription state and negotiated ATT MTU
- scheduled peripheral disconnects
- one-shot read and write plans with an ATT error, callback delay, and optional
  disconnect delay
- a dynamic auxiliary service for exercising the standard Service Changed path
- a reset command that restores the fixture state between tests

The complete byte-level contract is documented in
[`docs/protocol.md`](docs/protocol.md).

## 4. Fault injection semantics

A fault plan is written to the control characteristic and consumed by the next
matching read or write. The fixture can return an ATT error, delay the callback,
or disconnect while the operation is still pending. All timing happens on the
nRF52 instead of using a sleep on the host.

Reset clears values, counters, scheduled work, and fault plans. It keeps the
active BLE connection and current CCC state because changing either would also
change the test itself.

## 5. Scope and limitations

The firmware uses the stock Zephyr Bluetooth Host and Nordic SoftDevice
Controller. It can produce awkward timing and legal ATT errors, but it cannot
generate malformed ATT or link-layer packets.

Control also goes over BLE, which means the fixture cannot be configured while
it is disconnected. Advertising changes before connection, recovery from a
stuck fixture, bond deletion, and real power interruption need another control
path.

## 6. Design rules

- A successful control write is not used as proof that a data write worked.
- Data writes are verified through independent read-only mirrors and counters.
- Every one-shot fault is consumed by one matching operation.
- Each host test resets the fixture during setup and disconnects during
  teardown.
- The fixture accepts one BLE connection, so HIL tests run serially.
