# Firmware development setup

This guide prepares a Windows development environment for building and
flashing the Universal BLE HIL firmware. The Python environment, Zephyr
workspace, and build output stay inside the firmware repository and are
excluded from Git.

## 1. Prerequisites

Install the following before starting:

- [SEGGER J-Link software](https://www.segger.com/downloads/jlink/)
- an [nRF52 DK](https://www.nordicsemi.com/Products/Development-hardware/nRF52-DK)
  connected through its onboard debugger USB port

Run all commands in PowerShell.

```powershell
winget install Kitware.CMake Ninja-build.Ninja oss-winget.gperf Python.Python.3.12 Git.Git oss-winget.dtc wget 7zip.7zip
```

## 2. Create the Python environment

Open the firmware repository:

```powershell
cd C:\path\to\universal_ble_hil_firmware
```

Create and activate a repository-local virtual environment:

```powershell
py -3.12 -m venv .venv
.\.venv\Scripts\Activate.ps1
```

If PowerShell blocks activation, allow locally created scripts for the current
user and activate the environment again:

```powershell
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
.\.venv\Scripts\Activate.ps1
```

Install West:

```powershell
python -m pip install --upgrade pip
python -m pip install west
west --version
```

## 3. Create the Zephyr workspace

From the firmware repository root, initialize the workspace with Zephyr
4.4.2:

```powershell
west init `
  -m https://github.com/zephyrproject-rtos/zephyr `
  --mr v4.4.2 `
  .zephyr-workspace
cd .zephyr-workspace
west update
```

Install the Python packages required by the checked-out Zephyr modules, then
export Zephyr's CMake package:

```powershell
python -m pip install @((west packages pip) -split ' ')
west zephyr-export
```

## 4. Install the Zephyr SDK

Enter the Zephyr repository and install only the GNU Arm toolchain required by
the nRF52832:

```powershell
cd zephyr
west sdk install --gnu-toolchains arm-zephyr-eabi
west sdk list
cd ..
```

## 5. Perform the first build

From `.zephyr-workspace`, create a pristine build for the nRF52832 target:

```powershell
west build `
  -p always `
  -b nrf52dk/nrf52832 `
  -d ..\build `
  ..
```

The firmware image is generated under `build\zephyr` in the repository root.

## 6. Flash the development kit

Connect the nRF52 DK through its onboard debugger USB port, then run from
`.zephyr-workspace`:

```powershell
west flash -d ..\build -r jlink
```

West rebuilds changed sources before invoking J-Link. A successful flash
resets the board, which then advertises as `UniversalBLE-HIL`.

## 7. Everyday workflow

For each new PowerShell session:

```powershell
cd C:\path\to\universal_ble_hil_firmware
.\.venv\Scripts\Activate.ps1
cd .zephyr-workspace
```

Build incrementally and flash:

```powershell
west build -d ..\build
west flash -d ..\build -r jlink
```

Use a pristine build after changing the board, Kconfig options, CMake files,
or workspace dependencies:

```powershell
west build `
  -p always `
  -b nrf52dk/nrf52832 `
  -d ..\build `
  ..
```

## 8. Local directory layout

The setup creates the following ignored directories:

```text
universal_ble_hil_firmware/
|-- .venv/              Python environment
|-- .zephyr-workspace/  West workspace, Zephyr, and modules
|-- build/              generated firmware and build state
|-- components/
|-- docs/
|-- src/
|-- CMakeLists.txt
|-- prj.conf
`-- README.md
```

The repository `.gitignore` excludes `.venv`, `.zephyr-workspace`, and
`build`, so generated files are not committed.

## 9. Troubleshooting

### 9.1. Zephyr SDK is not found

If CMake cannot find `Zephyr-sdkConfig.cmake`, verify the SDK installation:

```powershell
west sdk list
```

In a non-interactive shell that does not inherit SDK discovery, set the SDK
path explicitly before building:

```powershell
$env:ZEPHYR_SDK_INSTALL_DIR = "$HOME\zephyr-sdk-1.0.1"
west build -d ..\build
```

### 9.2. J-Link cannot flash the board

Check that:

- the DK is connected through the onboard debugger USB port;
- SEGGER J-Link is installed and available to West;
- no other application is using the debugger; and
- the build target is `nrf52dk/nrf52832`.
