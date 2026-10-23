# ac-api-v1

An API module for [AzerothCore](https://www.azerothcore.org/).

## Status

The repository contains the initial AzerothCore module scaffold. API behavior will be added incrementally through the repository issue tracker.

## Installation

Clone the repository into the `modules` directory of an AzerothCore checkout:

```bash
git clone <repository-url> modules/ac-api-v1
```

Reconfigure and rebuild AzerothCore. The module is discovered automatically by the core module build system.

## Development

The module follows the official [AzerothCore module structure](https://www.azerothcore.org/wiki/The-Modular-Structure):

- `src/` contains C++ sources and the module loader.
- `conf/` contains module configuration templates.
- `data/sql/` is reserved for auth, characters, and world database migrations.
- `apps/` is reserved for module lifecycle tooling.

For stricter include validation, compile AzerothCore with precompiled headers disabled by passing `-DNOPCH=1` to CMake.

## License

This project is licensed under the MIT License. See [LICENSE](LICENSE).
