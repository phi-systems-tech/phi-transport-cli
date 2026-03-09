# phi-transport-cli

## Overview

CLI transport plugin for `phi-core`.

## Supported Protocols / Endpoints

- Local command-line transport (`cli`).
- Server-side plugin used by `phi-core`.
- Unix domain socket endpoint (default: `/var/lib/phi/@1/cli.sock`).

## Network Exposure

- No direct network listener.
- Local process / terminal usage only.

## Authentication & Security

- Authentication is validated by `phi-core`.
- Tokens and session handling follow the shared transport contract.
- The CLI socket is a local Unix socket (`/var/lib/phi/@1/cli.sock`) with permissions
  `srw-rw----` owned by `phi:phi` by default; access is therefore limited to user
  `phi` and members of group `phi`.
- When invoked as root, `phi-cli` drops privileges to the `phi` user before
  attempting to connect, and executes with the reduced `phi` process identity.

## Known Issues

- Initial scaffold only; implementation details may still change.

## License

See `LICENSE`.
Copyright (c) 2026 Phisys Ltd. (Switzerland).

---

## Developer Documentation

### Purpose

Provide a CLI transport boundary while keeping `phi-core` as the single backend authority.

### Features

- Command envelope parsing
- Sync/async core call routing
- ACK/response rendering
- Structured error output
- Local CLI client (`phi-cli`) with adapter list/start/stop/restart/reload
- Local CLI client (`phi-cli`) with transport start/stop/restart/reload

### Runtime Model

- Runs as `TransportInterface` Qt plugin.
- Exactly one plugin instance per transport plugin type.

### Core Integration Contract

- All core calls go through `callCoreSync` / `callCoreAsync`.
- Do not access core registries/managers directly.
- Auth is validated in `phi-core`.

### Protocol Contract

- Canonical contract: `phi-transport-api/PROTOCOLL.md`
- Transport-specific behavior should be documented in this repository.
- Every `phi-transport-cli` implementation must follow the canonical transport
  contract in `phi-transport-api/PROTOCOLL.md`.
- Do not introduce transport-local protocol features that are not defined in
  the canonical contract.
- If a required capability is missing in the transport API contract, define it
  in `phi-transport-api/PROTOCOLL.md` first, align `phi-core`/transport API as
  needed, and only then implement it in `phi-transport-cli`.

### Runtime Requirements

- `phi-core` with transport plugin loading enabled.
- Invoking `phi-cli` as root is supported; it automatically drops to user `phi` before socket connect.
- For deployment checks, use `scripts/check-phi-cli-runtime.sh` to verify the CLI socket path,
  mode, and ownership (`phi:phi`, mode `660`).

### Build Requirements

- CMake 3.21+
- Qt 6 Core
- C++20 compiler

### Configuration

- Runtime config is passed in by `phi-core`.
- `phi-core` resolves that transport config in two layers:
  - `/etc/phi/@1/transports/cli.json` as the default base config
  - `/var/lib/phi/@1/transports/cli/current/config.json` as the runtime override
- The `phi-transport-cli` Debian package provides `/etc/phi/@1/transports/cli.json`
  with the default Unix socket path.

### Build

```bash
cmake -S . -B ../build/phi-transport-cli/release-ninja -G Ninja
cmake --build ../build/phi-transport-cli/release-ninja --parallel
```

### Example

```bash
phi-cli adapter list
phi-cli adapter list --tenant 2
phi-cli adapter list --json
phi-cli adapter list --socket /var/lib/phi/@1/cli.sock
phi-cli adapter restart --id 42
phi-cli adapter restart --external-id hue-bridge-main
phi-cli adapter restart --name "Living Room"
phi-cli adapter restart hue --all
phi-cli adapter reload hue
phi-cli transport restart ws
phi-cli transport reload cli
scripts/check-phi-cli-runtime.sh /var/lib/phi/@1/cli.sock
```

Selector rules (aligned to `phi-transport-api/PROTOCOLL.md`):

- `cmd.adapter.start|stop|restart` are executed with resolved `adapterId`.
- `--id`, `--external-id`, and `--name` resolve to exactly one adapter instance.
- `--name` must resolve uniquely, otherwise request is rejected as ambiguous.
- `<plugin-type>` is allowed only together with `--all` for instance operations
  (fan-out over all resolved adapter ids).
- Process/plugin-level reload uses `cmd.adapter.reload <plugin-type>`.
- Transport lifecycle uses `cmd.transport.start|stop|restart|reload <plugin-type>`.
- Transport `restart` keeps the already loaded plugin binary and performs stop/start.
- Transport `reload` performs a full plugin unload/load before start.

### Installation

- Output shared library: `libphi_transport_cli.so`
- Deployment location: `/usr/lib/phi/plugins/transports/` (or distro equivalent)
- CLI binary: `/usr/bin/phi-cli`

### Observability

- Logging category names will be documented with implementation.

### Troubleshooting

- If plugin loading fails, verify IID compatibility with `phi-transport-api`.

### Maintainers

- Phisys Ltd. (Switzerland)

### Issue Tracker

- `https://github.com/phi-systems-tech/phi-transport-cli/issues`

### Releases / Changelog

- Releases: `https://github.com/phi-systems-tech/phi-transport-cli/releases`
- Tags: `https://github.com/phi-systems-tech/phi-transport-cli/tags`
