# ledctl

Direct AppleSMC control of the front white LED.
No network, setuid helper, or resident process.

Tested on **Mac mini Mac18,5 (Apple M6)** with macOS **27.0 (26A425)**
and **27.2.1 (26B131)**. Other models are blocked unless explicitly overridden.

## Build and install

Requires Apple's Command Line Tools; no third-party dependencies.

```sh
make
sudo ./ledctl --install
```

`make clean` removes only the local binary; the installed CLI and startup setting remain.

Installation copies the CLI to `/usr/local/bin`
PATH. It survives deletion of the build folder and does not change brightness
or enable startup control. Use `./ledctl` directly if you prefer not to install.

## Use

```sh
ledctl status
sudo ledctl 0       # off
sudo ledctl 20      # 20%
sudo ledctl 100     # full brightness
```

Whole percentages, 0–100. `off`, `on`, and `brightness 20` also work.
`on` means 100%. Reading needs no sudo; writing does.

## Other models

Only `Mac18,5` is verified. To explicitly opt into an unverified model:

```sh
sudo ledctl 20 --force-model
```

**Risk:** the flag does not prove that `LS0C` controls the LED on another model.
It does not bypass key type/length checks, or write verification.
Combine it with `--persistent` only if you also want this override saved for boot.

## Save a startup setting

```sh
sudo ledctl 20 --persistent
```

Sets 20% now, installs the CLI if needed, and creates a one-shot Root LaunchDaemon.
No process stays running. Disable other LED controllers and their login items
to prevent them overwriting the setting.

```sh
sudo ledctl 40 --persistent # replace the startup value
sudo ledctl 10              # change now, keep the startup value
sudo ledctl --unpersist     # remove startup; keep current brightness
```

The flag accepts numeric, `off`, `on`, and `brightness` commands.
It is not valid with `status`, `snapshot`, or `restore`.
If installation fails after setting the LED, that brightness remains applied.

Installed files:
- `/usr/local/bin/ledctl` — root-owned CLI on PATH.
- `/usr/local/share/doc/ledctl/README.txt` — automatically installed English guide.
- `/Library/LaunchDaemons/local.ledctl.plist` — startup configuration.
- `/var/db/ledctl` — root-only startup logs.

`--unpersist` leaves the binary, guide, and logs in place.
Persistence reapplies the value at boot, not continuously or on wake.

## Optional manual backup

```sh
ledctl snapshot original.led
sudo ledctl restore original.led
```

No automatic snapshot files are created. Every write is read back; a failure
triggers one attempt to restore the previous value held in memory.
That value is not retained after the process exits. Use `snapshot` explicitly
if you want a durable recovery file. Snapshots are private and never overwritten.

Direct control and startup persistence were verified on this Mac, including a
real reboot.

