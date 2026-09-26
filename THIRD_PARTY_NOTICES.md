# Third-Party Notices

## threadsafe-logger

BuffetAlligator statically links [threadsafe-logger](https://github.com/joshmorgan1000/threadsafe-logger).

MIT License

Copyright (c) 2026 Josh Morgan

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## xxHash

The `buffetalligator::xxh3_128` namespace in `include/alligator.hpp` contains an
adaptation of [xxHash](https://github.com/Cyan4973/xxHash)'s XXH3 128-bit
implementation, licensed under BSD-2-Clause.

Local modifications place the implementation in a C++ namespace, use
`__uint128_t` for multiplication, and retain only the short and medium input
paths, rejecting inputs longer than 240 bytes.

The original import revision was not recorded; the notice below is reproduced
from [upstream `xxhash.h` at v0.8.3](https://github.com/Cyan4973/xxHash/blob/v0.8.3/xxhash.h).

```text
xxHash - Extremely Fast Hash algorithm
Header File
Copyright (C) 2012-2023 Yann Collet

BSD 2-Clause License (https://www.opensource.org/licenses/bsd-license.php)

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:

   * Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above
     copyright notice, this list of conditions and the following disclaimer
     in the documentation and/or other materials provided with the
     distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

You can contact the author at:
  - xxHash homepage: https://www.xxhash.com
  - xxHash source repository: https://github.com/Cyan4973/xxHash
```

## Linked and installed dependencies

The build pins the following upstream projects, links or installs them with the library, and copies their license files into the installation's `share/licenses/alligator` directory:

| Project | Version | License | How it ships |
| --- | --- | --- | --- |
| [libuv](https://github.com/libuv/libuv) | v1.52.1 | MIT, with upstream notices for included platform code | static library |
| [libsodium](https://github.com/jedisct1/libsodium) | 1.0.22-RELEASE | ISC | static library |
| [libfabric](https://github.com/ofiwg/libfabric) | v2.6.0 | BSD-2-Clause or GPL-2.0, as described in COPYING | static library |
| [concurrentqueue](https://github.com/cameron314/concurrentqueue) | v1.0.5 | Simplified BSD; the blocking queue's semaphore is zlib | headers installed beside `alligator.hpp` |
| [readerwriterqueue](https://github.com/cameron314/readerwriterqueue) | v1.0.7 | Simplified BSD; the blocking queue's semaphore is zlib | headers installed beside `alligator.hpp` |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | v1.4.350 | Apache-2.0 or MIT | headers installed |
| [Vulkan-Loader](https://github.com/KhronosGroup/Vulkan-Loader) | v1.4.350 | Apache-2.0 or MIT | shared runtime, Linux |
| [MoltenVK](https://github.com/KhronosGroup/MoltenVK) | v1.4.1 | Apache-2.0 with bundled third-party notices | shared runtime, macOS |
| [shaderc](https://github.com/google/shaderc) | v2026.2 | Apache-2.0 | `libshaderc_combined` linked statically and privately |
| [Folly fork](https://github.com/joshmorgan1000/folly) | 839090a6 | Apache-2.0 | static libraries under `deps/folly`, linked publicly; consumers resolve it through the exported package config |

`libshaderc_combined` bundles the following, whose notices are installed under `share/licenses/alligator/shaderc`:

| Project | License |
| --- | --- |
| [glslang](https://github.com/KhronosGroup/glslang) | BSD-3-Clause, with Apache-2.0, MIT and NVIDIA portions, as described in LICENSE.glslang |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | Apache-2.0 |
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers) | MIT-style Khronos license |

Vulkan support is built unconditionally; Vulkan-Loader is built on Linux and MoltenVK on macOS. OpenSSL is discovered from the system and is not vendored.

## Folly system dependencies

Folly is built from source but resolves Boost (BSL-1.0), glog and gflags (BSD-3-Clause), fmt (MIT), double-conversion (BSD-3-Clause), libevent (BSD-3-Clause), zstd (BSD-3-Clause), lz4 (BSD-2-Clause), snappy (BSD-3-Clause), bzip2, and xz from the system. `run_build.sh` checks for them before configuring Folly and offers to install them; they are not vendored and their notices are not copied.

## Built for downstream consumers only

`run_build.sh` also fetches and builds [Abseil](https://github.com/abseil/abseil-cpp) 20260107.1 (Apache-2.0), [simdjson](https://github.com/simdjson/simdjson) v4.6.3 (Apache-2.0) and [curl](https://github.com/curl/curl) curl-8_20_0 (curl license). BuffetAlligator neither links nor installs them; they are staged under `deps/` for projects that build on top of it, and those projects carry their notices.
