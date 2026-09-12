# Third-Party Notices

## threadsafe-logger

BuffetAlligator statically links [threadsafe-logger](https://github.com/joshmorgan1000/threadsafe-logger), pinned to commit `52588cec8fda78ffa5af31b8479b4e97a9417de8`.

MIT License

Copyright (c) 2026 Josh Morgan

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Networking and Vulkan dependencies

The build pins the following upstream projects and includes their license files in the installation's `share/licenses/alligator` directory:

| Project | Version | License |
| --- | --- | --- |
| [libuv](https://github.com/libuv/libuv) | v1.52.1 | MIT, with upstream notices for included platform code |
| [libsodium](https://github.com/jedisct1/libsodium) | 1.0.22-RELEASE | ISC |
| [libfabric](https://github.com/ofiwg/libfabric) | v2.6.0 | BSD-2-Clause or GPL-2.0, as described in COPYING |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | v1.4.350 | Apache-2.0 or MIT |
| [Vulkan-Loader](https://github.com/KhronosGroup/Vulkan-Loader) | v1.4.350 | Apache-2.0 or MIT |
| [MoltenVK](https://github.com/KhronosGroup/MoltenVK) | v1.4.1 | Apache-2.0 with bundled third-party notices |

Vulkan-Loader is built on Linux; MoltenVK is built on macOS. OpenSSL is discovered from the system and is not vendored.
