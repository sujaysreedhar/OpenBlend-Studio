# Third-party notices and corresponding source

OpenBlend Studio (built as Projection Studio): GPL-3.0-only, full text in `LICENSE` and project notice in `COPYRIGHT`. Third-party components listed below retain their own copyrights and licenses. No proprietary reference-product code or assets are included.

| Dependency | Version | License / source |
|---|---|---|
| Qt Core, GUI, Widgets, Network, WebSockets and deployed plugins | 6.8.3 | Qt open-source licenses (LGPL-3.0 / GPL alternatives; individual bundled components retain their notices). [Qt source archives](https://download.qt.io/archive/qt/6.8/6.8.3/submodules/). Dynamically linked; replaceable DLLs are included. |
| OpenCV | 4.11.0 | Apache-2.0; [source](https://github.com/opencv/opencv/tree/4.11.0), including its third-party notices. Minimal modules built without FFmpeg, IPP, OpenCL, OpenEXR, proprietary codecs or nonfree algorithms. |
| nlohmann/json | 3.11.3 | MIT; [source](https://github.com/nlohmann/json/tree/v3.11.3) |
| MinHook and included HDE | 1.3.4 | BSD-style licenses in MinHook's LICENSE.txt; [source](https://github.com/TsudaKageyu/minhook/tree/v1.3.4) |
| MinGW-w64 / winpthreads | Qt MinGW 13.1 toolchain | Component licenses included under `licenses/toolchain`; [MinGW-w64](https://www.mingw-w64.org/). |
| GCC libstdc++ / libgcc runtime | 13.1.0 | GPLv3 with GCC Runtime Library Exception; [GCC sources](https://gcc.gnu.org/releases.html). Exception and license text included with the toolchain notices. |
| Windows / Direct3D system components | OS-supplied | Windows SDK/API interfaces; OS libraries are not copied into the source tree. D3D compiler runtime redistribution, if supplied by Qt deployment, retains Microsoft's terms. |

Build tools are CMake 3.31.6, Ninja 1.11.1.3 and aqtinstall 3.2.1. They are not part of the application runtime. Windows capture ABI signatures are derived from public Windows metadata as exposed by Microsoft's Windows bindings; no proprietary implementation is copied.

The package script includes the application's complete source, build/install scripts, dependency licenses and cached dependency source archives when present. Before any public binary release, provide the exact corresponding dependency sources and build information alongside it; `docs/FEATURES.md` does not mark public distribution qualification complete. The workspace package is an unsigned development artifact. Keep all license notices with redistributed binaries, including the source-archive notices for bundled libraries such as Qt's image/font/compression dependencies. Source packages are not a substitute for testing installation/removal or documenting modifications.
