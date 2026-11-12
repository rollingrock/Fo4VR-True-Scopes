# True Scopes calibration for FRIK

The optional companion adds a **True Scopes** button to FRIK's in-game
configuration menu. Install FRIK with its UI assets and a True Scopes build
that provides the [calibration API](../../docs/LIVE_CALIBRATION.md).

## Build the combined True Scopes package

Use CMake 4.2 or later, Visual Studio 2022 or later with the C++ desktop
workload, and a bootstrapped vcpkg checkout. Set `VCPKG_ROOT` to that checkout.
From the True Scopes repository root with its CommonLibF4 submodule initialized,
run these commands in PowerShell.

```powershell
git clone https://github.com/ArthurHub/F4VR-CommonFramework.git ../F4VR-CommonFramework
git -C ../F4VR-CommonFramework checkout 3f0bbc8c45c1193b16ca1c09b9f8ba2a97b95890
git -C ../F4VR-CommonFramework submodule update --init --recursive

$framework = (Resolve-Path ../F4VR-CommonFramework).Path
$env:VCPKG_INSTALLATION_ROOT = $env:VCPKG_ROOT
cmake --preset vs2022-windows-vcpkg-vr-package -A x64
cmake --build buildvr --config Release

cmake -S integrations/frik -B build-frik -A x64 "-DF4VR_COMMON_FRAMEWORK_PATH=$framework" -DF4CF_WITH_TRACY=OFF
cmake --build build-frik --config Release
cmake --install build-frik --config Release --prefix buildvr/zip
Compress-Archive -Path buildvr/zip/* -DestinationPath buildvr/TrueScopesVR.zip -Force
```

Install `buildvr/TrueScopesVR.zip` as one mod in MO2. With compatible FRIK
installed, open its configuration menu and select **True Scopes**.

See [asset credits](ASSET_CREDITS.md) for the launcher mesh and texture.
