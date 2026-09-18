# Bounded CivetWeb WebSocket reader

The C11 WebSocket server requires CivetWeb 1.16 with
[`websocket-limits.patch`](websocket-limits.patch). Stock 1.16 reads and allocates
a whole frame before the application callback, hides the masking bit, and retries
partial-frame timeouts indefinitely. Application callback checks cannot fix those
three boundaries. CMake checks both the extension header and its linked symbol;
HTTP-only builds still support unmodified CivetWeb.

The patch adds `mg_set_websocket_limits(connection, max_frame_size, timeout_ms)`.
The C11 connect callback enables it before accepting the upgrade. For those
connections only, the reader:

- rejects missing client masking or reserved bits with Close 1002;
- rejects oversized declared payloads with Close 1009 before allocation/read;
- uses a monotonic idle/whole-frame deadline, including partial headers, masks
  and payloads; partial progress does not renew it;
- terminates a timed-out connection and releases its worker and frame buffer.

The existing accumulated-message bound still checks continuation frames. The
frame deadline resets after each complete frame, including control frames; it is
not an application handler deadline or a total fragmented-message deadline.
Unconfigured CivetWeb users retain upstream behavior. The extension does not
change public structures. Rebuild both the header and library together.

## Linux or standalone CMake

Starting from an unmodified CivetWeb **v1.16** source tree, run from this repository
root (choose source, build and install paths outside this repository):

```sh
git -C /path/to/civetweb-1.16 apply "$PWD/lib/c11/dependencies/civetweb/websocket-limits.patch"
cmake -S /path/to/civetweb-1.16 -B /path/to/civetweb-build \
  -DCMAKE_INSTALL_PREFIX=/path/to/civetweb-install \
  -DCIVETWEB_BUILD_TESTING=OFF -DCIVETWEB_ENABLE_ASAN=OFF \
  -DCIVETWEB_ENABLE_DEBUG_TOOLS=OFF -DCIVETWEB_ENABLE_WEBSOCKETS=ON \
  -DCIVETWEB_ENABLE_SERVER_EXECUTABLE=OFF -DCIVETWEB_ENABLE_SSL=OFF
cmake --build /path/to/civetweb-build --parallel
cmake --install /path/to/civetweb-build
cmake -S lib/c11 -B out/c11 \
  -Dcivetweb_DIR=/path/to/civetweb-install/lib/cmake/civetweb \
  -DTHRIFT_C11_WEBSOCKET_SERVER=ON
```

`git apply` also works in a source archive without Git metadata. Installed C11
consumers must resolve this same patched CivetWeb package. Use a private prefix;
do not overwrite a system CivetWeb installation.

## Windows / vcpkg

The included overlay pins CivetWeb 1.16 and retains the official vcpkg port's
platform fixes and features. Its additional patch is shared with the standalone
build. Run from the repository root:

```powershell
C:/work/vcpkg/vcpkg.exe install civetweb:x64-windows-static `
  --overlay-ports=lib/c11/dependencies/civetweb/overlay
```

If an unpatched classic-mode package is already installed, `install` leaves it
unchanged. Run the explicit upgrade with the same overlay and install-root options:

```powershell
C:/work/vcpkg/vcpkg.exe upgrade civetweb:x64-windows-static --no-dry-run `
  --overlay-ports=lib/c11/dependencies/civetweb/overlay
```

For manifest builds, pass
`-DVCPKG_OVERLAY_PORTS=<repository>/lib/c11/dependencies/civetweb/overlay` when
configuring with the vcpkg toolchain. Keep the original curl WebSocket features
and zstd dependencies. Match `CMAKE_MSVC_RUNTIME_LIBRARY` to the selected triplet.

The overlay is adapted from vcpkg's CivetWeb 1.16#2 port. Its original files are
covered by the accompanying vcpkg MIT license; CivetWeb retains its upstream
license. The bounded-reader patch is Apache-2.0, like this C11 runtime.
