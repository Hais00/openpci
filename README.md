# OpenPCI

OpenPCI is a macOS PCIe transport for supported external devices. Its DriverKit system extension manages device ownership, PCI configuration, BAR access, DMA and interrupts. Its C SDK exposes these operations to host applications such as [Ready](https://github.com/Hais00/ready).

## Build

Use macOS with Xcode, CMake 3.24+ and Python 3. Full Xcode with the DriverKit SDK is required for the extension. The SDK can be built independently.

```sh
git clone https://github.com/Hais00/openpci.git
cd openpci
cmake -S . -B build/sdk -DCMAKE_BUILD_TYPE=Release
cmake --build build/sdk --parallel
ctest --test-dir build/sdk --output-on-failure
cmake --install build/sdk --prefix "$PWD/build/install"
python3 tools/build_driver.py --unsigned-build --build-root build/driver \
  --receipt build/driver/preflight.json
```

The SDK installs `libextgpu`, headers, a CMake package and the `extgpu-info` inventory utility. The Xcode build produces the installer app and its embedded DEXT. An unsigned build is for compilation checks. Activation requires a signed app and DEXT, appropriate Apple entitlements and provisioning profiles, and user approval. Run `python3 tools/build_driver.py --help` for signing options.

For Ready deployments that enable direct BAR mapping, generate a local signing identity **before** building the DEXT:

```sh
mkdir -p "$HOME/.config/openpci"
swift tools/create_direct_bar_key.swift \
  "$HOME/.config/openpci/direct-bar.key" driver/DirectBARPublicKey.local.hpp
```

See [SDK usage](sdk/README.md) for device ownership and release requirements.

## License

The OpenPCI DriverKit extension, installer and SDK in this repository are available under the [PolyForm Noncommercial License 1.0.0](LICENSE). Commercial use requires separate permission from the copyright holder. See [NOTICE](NOTICE) and the [historical license grants](LICENSES/History.md).
