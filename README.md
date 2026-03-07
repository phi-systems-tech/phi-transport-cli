# phi-transport-cli

## Overview

CLI transport plugin for `phi-core`.

## Supported Protocols / Endpoints

- Local command-line transport (`cli`).
- Server-side plugin used by `phi-core`.

## Network Exposure

- No direct network listener.
- Local process / terminal usage only.

## Authentication & Security

- Authentication is validated by `phi-core`.
- Tokens and session handling follow the shared transport contract.

## Known Issues

- Initial scaffold only; implementation details may still change.

## License

See `LICENSE`.

---

## Developer Documentation

### Purpose

Provide a CLI transport boundary while keeping `phi-core` as the single backend authority.

### Features

- Command envelope parsing
- Sync/async core call routing
- ACK/response rendering
- Structured error output

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

### Runtime Requirements

- `phi-core` with transport plugin loading enabled.

### Build Requirements

- CMake 3.21+
- Qt 6 Core
- C++20 compiler

### Configuration

- Configuration keys are transport-specific and currently minimal.

### Build

```bash
cmake -S . -B ../build/phi-transport-cli/release-ninja -G Ninja
cmake --build ../build/phi-transport-cli/release-ninja --parallel
```

### Installation

- Output shared library: `libphi_transport_cli.so`
- Deployment location: `/opt/phi/plugins/transports/` (or distro equivalent)

### Observability

- Logging category names will be documented with implementation.

### Troubleshooting

- If plugin loading fails, verify IID compatibility with `phi-transport-api`.

### Maintainers

- Phi Systems Tech team

### Issue Tracker

- `https://github.com/phi-systems-tech/phi-transport-cli/issues`

### Releases / Changelog

- Releases: `https://github.com/phi-systems-tech/phi-transport-cli/releases`
- Tags: `https://github.com/phi-systems-tech/phi-transport-cli/tags`
