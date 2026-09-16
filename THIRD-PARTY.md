# Third-party components, credits and notices

Theo's Render Pipeline includes original integration work and contributions from
the projects below. Project licensing, the full GPL text and modding exceptions
are recorded in [LICENSE](LICENSE). Full third-party copyright notices and
permission text follow in [Notices](#notices). Separately downloaded NVIDIA
runtimes retain the license files accompanying their downloads.

## Source contributions

- **Skyrim-Upscaler / skyrim-drs** — PureDark and Tim, MIT. Portions of the
  upscaler hooks, dynamic-resolution integration and engine declarations derive
  from this public Skyrim integration. This credit is scoped to those components.
  https://github.com/PureDark/Skyrim-Upscaler/tree/fa057bb088cf399e1112c1eaba714590c881e462
- **ENBFrameGeneration / Community Shaders** — doodlum, GPL-3.0-or-later with
  the Modding Exception and Linking Exception with Corresponding Source.
  The D3D11/D3D12 swapchain integration has this lineage.
  https://github.com/doodlum/ENBFrameGeneration
- **vrperfkit** — Holger Frydrych, MIT; mip-LOD-bias sampler technique.
  https://github.com/fholger/vrperfkit
- **RenoDX** — Carlos Lopez Jr., MIT. The NR Ratio path's luminance transfer,
  OkLab hue correction and AP1 gamut-clamp sequence correspond to the public
  implementations of [UpgradeToneMap](https://github.com/clshortfuse/renodx/blob/95f4cec5acc9c15ff47ac41e3641691a9f22a734/src/shaders/tonemap.hlsl),
  [HueOKLab](https://github.com/clshortfuse/renodx/blob/9a7e1080f974013b87434c66f3e3f7bf9a137ee5/src/shaders/colorcorrect.hlsl)
  and [AP1 clipping](https://github.com/clshortfuse/renodx/blob/95f4cec5acc9c15ff47ac41e3641691a9f22a734/src/shaders/color/clamp.hlsl).
  RenoDX is credited for these components. Numerical guards and coefficients
  differ; this is not attribution or license coverage of the entire NR pipeline.
- **OkLab** — Björn Ottosson; colour-space definition and conversion formulas.
  The author's [reference conversion code](https://bottosson.github.io/posts/oklab/)
  is offered in the public domain, with MIT as an alternative.
- **FidelityFX RCAS / RCAS for ReShade** — AMD and RdenBlaauwen, MIT; sharpening
  shader adapted through the ReShade implementation.
  https://github.com/RdenBlaauwen/RCAS-for-ReShade
- **RTX40MFG-Unlock** — Michael Robles, MIT; Ada temporal patch and provider
  helpers, revision 4ab7b5e16941e065f81c665b6d7fe2c2e2ec843f.
  https://github.com/dashdogy/RTX40MFG-Unlock

## Libraries and SDKs

CommonLibSSE-NG, Dear ImGui, SimpleIni, spdlog, fmt, DirectXMath and
the Detours wrapper use MIT licenses. rapidcsv and xbyak use BSD-3-Clause.
Streamline public headers use NVIDIA's MIT license. See [Notices](#notices)
for their copyright holders and full permissions.

Detours is built from [Nukem9/detours revision
cc5a2e4a58ef462821877b35ad30215f0e16bba1](https://github.com/Nukem9/detours/tree/cc5a2e4a58ef462821877b35ad30215f0e16bba1),
including its bundled Zydis decoder and Zycore headers. All three use MIT;
their copyright notices and permission text are included below. The build uses
this source dependency, with no precompiled Detours archives in the tree.

NVIDIA provides the NGX SDK and DLSS, DLSS-G, Streamline/Reflex and Neural
Rendering runtimes. These vendor components retain their own terms and are not
relicensed under the project's GPL license. Their use does not transfer
ownership of NVIDIA's kernels or trained models to this project.

## Notices

These notices apply to the third-party source incorporated into Theo's Render
Pipeline binaries and shaders. Vendor DLLs retain their own terms; their license documents are listed
below. This file does not grant
rights to NVIDIA, Intel, AMD, Skyrim, SKSE, ENB, or another separately
installed mod beyond the rights stated by their respective licenses.

### Skyrim Upscaler by PureDark

MIT License

Copyright (c) 2022 PureDark

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### Original skyrim-drs work credited to Tim

MIT License

Copyright (c) 2022 Tim

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### MIT-licensed components

The following copyright notices use the MIT permission and warranty text below:

- vrperfkit: Copyright (c) 2022 Holger Frydrych
- CommonLibSSE-NG: Copyright (c) 2018 Ryan-rsm-McKenzie
- Dear ImGui: Copyright (c) 2014-2024 Omar Cornut
- SimpleIni: Copyright (c) 2006-2022 Brodie Thiesfield
- spdlog: Copyright (c) 2016 Gabi Melman
- Detours: Copyright (c) 2019 Nukem <Nukem@outlook.com>
- Zydis (bundled in Detours): Copyright (c) 2014-2019 Florian Bernd; Copyright (c) 2014-2019 Joel Höner
- Zycore (bundled in Detours): Copyright (c) 2018-2019 Florian Bernd; Copyright (c) 2018-2019 Joel Höner
- DirectXMath: Copyright (c) Microsoft Corporation
- RenoDX: Copyright (c) 2025 Carlos Lopez Jr.
- AMD FidelityFX RCAS source used through RCAS for ReShade: Copyright (C) 2023 Advanced Micro Devices, Inc.
- Streamline 2.11.1 public headers: Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved.
- Streamline 2.11.1 public source license: Copyright (c) 2023 NVIDIA CORPORATION. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

RCAS for ReShade identifies the shader's implementation and additions as work
by RdenBlaauwen. Its source file is available at
https://github.com/RdenBlaauwen/RCAS-for-ReShade/blob/main/RCAS.fx. That file
contains the MIT permission above and identifies the additions without a
separate restrictive grant.

The Streamline notices above cover its public MIT-licensed material, including
the headers used here. Vendor binaries retain their separate terms.

### fmt

Copyright (c) 2012 - present, Victor Zverovich and {fmt} contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

Optional exception: if compiling source code embeds portions of fmt into a
machine-executable object, those embedded portions may be redistributed in
object form without including the preceding copyright and permission notices.

### rapidcsv

BSD 3-Clause License

Copyright (c) 2017, Kristofer Berggren
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.
* Neither the name of the copyright holder nor the names of its contributors
  may be used to endorse or promote products derived from this software
  without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

### xbyak

BSD 3-Clause License

Copyright (c) 2007 MITSUNARI Shigeo
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this
list of conditions and the following disclaimer.

Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

Neither the name of the copyright owner nor the names of its contributors may
be used to endorse or promote products derived from this software without
specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

### Vendor license documents

NVIDIA runtime downloads supply their own documents, including:

- `nvngx_dlss.license.txt`: NVIDIA RTX SDKs license and supplement.
- `reflex.license.txt`: NVIDIA SDK license and Reflex supplement.
- `nis.license.txt`: NVIDIA Image Scaling MIT notice.

These documents retain their original terms. The project's GPL license does
not relicense the supplied DLSS, DLSS-G, Neural Rendering or Streamline DLLs.

### RTX40MFG-Unlock experimental temporal patch

Upstream: https://github.com/dashdogy/RTX40MFG-Unlock
Base commit: 4ab7b5e16941e065f81c665b6d7fe2c2e2ec843f
Named temporal profile, wrapper matching and provider-branch adaptations:
e13a9841733b0ae43b7215e8c51fe0eb3897816f (v1.3.3).

MIT License

Copyright (c) 2026 Michael Robles

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
