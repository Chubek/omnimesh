# OmniMesh scaffold

This is a buildable architecture scaffold, not a production runtime. It uses C++17 and has no required third-party dependencies.

```sh
cmake -S . -B /mnt/data/work/omnimesh-build -G Ninja -DOMNIMESH_BUILD_TESTS=ON
cmake --build /mnt/data/work/omnimesh-build
ctest --test-dir /mnt/data/work/omnimesh-build --output-on-failure
/mnt/data/work/omnimesh-build/tools/omnimesh --version
```

Vendored libraries are represented as optional submodules; CMake never downloads them. OCI execution, distributed coordination, security, persistence, schema validation, VMM, Lua, and real monitoring remain explicitly unsupported.

## Repository conventions

Manifest examples and schemas live under `manifests/`; paths in documentation and tooling are relative to the repository root. The offline scaffold builds only the sources present under `src/`, with optional vendored dependencies under `third_party/` not required for the default build.
