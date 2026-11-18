# PCSX2 MCP companion

The companion runs over MCP stdio and launches a local, MCP-enabled build of PCSX2 on demand. It provides EE and IOP debugger operations, guest memory search and snapshots, game metadata, and save states.

## Build and register

Build PCSX2 normally. Keep the complete emulator build output together. A standard development build needs the generated `translations/` and `resources/` directories beside `pcsx2-qt`. If you place the companion elsewhere, use `--emulator` to point to the emulator in its original build directory.

Build the companion separately from this directory with Go 1.25 or newer.

Linux:

```sh
CGO_ENABLED=0 go build -o /absolute/path/to/pcsx2/bin/pcsx2-mcp .
./setup.sh /absolute/path/to/pcsx2/bin/pcsx2-mcp
```

Windows (PowerShell):

```powershell
$env:CGO_ENABLED = "0"
go build -o "C:\path\to\pcsx2\pcsx2-mcp.exe" .
codex mcp add pcsx2 -- "C:\path\to\pcsx2\pcsx2-mcp.exe"
```

For any stdio MCP client, set its command to the companion's absolute path. Use `--emulator <absolute-path>` if the emulator has a custom executable name or is an AppImage. Arguments after `--` go to the emulator. For example, use `-- /path/to/game.iso` to boot a title at launch.

## Use

Call `pcsx2_connect`, then use `pcsx2_mount_disc` with an absolute image path to boot a disc or swap the active disc without resetting the VM. Call `pcsx2_status` to see whether a VM is active and paused. Addresses and register values use hexadecimal strings. Use `cpu: "iop"` for IOP tools that accept a CPU argument. Pause the VM before stepping, walking a stack, or editing memory and registers. Save-state slots range from 0 to 9.

Disconnecting the MCP client leaves PCSX2 open. To establish a new connection, close the previous PCSX2 window before calling `pcsx2_connect` again. The companion uses a private loopback connection and does not require PINE to be enabled.
