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
- **MFGAmpereUnlock-RenoDx** — ImDreamt, mavismmg and nefh, MIT; SM86 PTX
  preparation, provider architecture policy and scoped NGX/NVAPI compatibility
  techniques, adapted from revision dd349cdbbae6525188e71fbf2e6d3c648be40db9.
  https://github.com/nefh/MFGAmpereUnlock-RenoDx
- **NVAPI** — NVIDIA, MIT; minimal public architecture-query ABI declarations,
  revision 87dca625e83fd89a983e19b904e5f3a580da90d2.
  https://github.com/NVIDIA/nvapi

## Libraries and SDKs

CommonLibSSE-NG 8.1.0 (`3c0f5a87c3b166c9a6712d5c3bd180e9ac5ad0fd`) uses
GPL-3.0-or-later with its Modding Exception and Linking Exception with
Corresponding Source. It is statically linked from the
[alandtse fork](https://github.com/alandtse/CommonLibSSE-NG/tree/3c0f5a87c3b166c9a6712d5c3bd180e9ac5ad0fd).
Its original MIT attribution is retained for the historical portions; the
library as a whole is no longer MIT-licensed. Its HDE64 patch diagnostics use
the BSD-2-Clause decoder from MinHook v1.3.4. Exact exception/decoder terms
appear below; the full GPL v3 text is included in [LICENSE](LICENSE).

Dear ImGui, SimpleIni, spdlog, fmt, DirectXMath, DirectXTK and
the Detours wrapper use MIT licenses. rapidcsv and xbyak use BSD-3-Clause.
Streamline public headers use NVIDIA's MIT license. See [Notices](#notices)
for their copyright holders and full permissions.

Detours is built from [Nukem9/detours revision
cc5a2e4a58ef462821877b35ad30215f0e16bba1](https://github.com/Nukem9/detours/tree/cc5a2e4a58ef462821877b35ad30215f0e16bba1),
including its bundled Zydis decoder and Zycore headers. All three use MIT;
their copyright notices and permission text are included below. The build uses
this source dependency, with no precompiled Detours archives in the tree.

NVIDIA NGX SDK headers, inline helpers and linked SDK code are used by the
renderer. Their original license is reproduced below. This software contains
source code provided by NVIDIA Corporation.

DLSS, DLSS-G, Streamline/Reflex and Neural Rendering runtime DLLs are supplied
separately by the user. NVIDIA SDK and runtime components retain their own
terms and are not relicensed under the project's GPL license.

## Notices

These notices apply to the third-party source and SDK code incorporated into
Theo's Render Pipeline binaries and shaders. Separately downloaded vendor DLLs
retain their accompanying terms. This file does not grant
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
- Original MIT portions of CommonLibSSE-NG: Copyright (c) 2018 Ryan-rsm-McKenzie
- Dear ImGui: Copyright (c) 2014-2024 Omar Cornut
- SimpleIni: Copyright (c) 2006-2022 Brodie Thiesfield
- spdlog: Copyright (c) 2016 Gabi Melman
- Detours: Copyright (c) 2019 Nukem <Nukem@outlook.com>
- Zydis (bundled in Detours): Copyright (c) 2014-2019 Florian Bernd; Copyright (c) 2014-2019 Joel Höner
- Zycore (bundled in Detours): Copyright (c) 2018-2019 Florian Bernd; Copyright (c) 2018-2019 Joel Höner
- DirectXMath: Copyright (c) Microsoft Corporation
- DirectXTK: Copyright (c) Microsoft Corporation
- RenoDX: Copyright (c) 2025 Carlos Lopez Jr.
- AMD FidelityFX RCAS source used through RCAS for ReShade: Copyright (C) 2023 Advanced Micro Devices, Inc.
- Streamline 2.11.1 public headers: Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved.
- Streamline 2.11.1 public headers: Copyright (c) 2022-2024 NVIDIA CORPORATION. All rights reserved.
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

### NVIDIA NGX SDK code

The renderer uses the NGX SDK headers, inline helpers and `nvsdk_ngx_d.lib`
from Streamline 2.11.1. Their license is reproduced verbatim below (line endings
normalized). This notice covers the SDK code included in the renderer;
separately downloaded runtime DLLs retain the terms supplied with them.

Copyright (c) 2018-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
Copyright (c) 2019-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

Source: https://github.com/NVIDIA-RTX/Streamline/blob/v2.11.1/external/ngx-sdk/license.txt

```text
NVIDIA RTX SDKs LICENSE

This license is a legal agreement between you and NVIDIA Corporation ("NVIDIA") and governs the use of the NVIDIA RTX software development kits, including the DLSS SDK, NGX SDK, RTXGI SDK, RTXDI SDK, RTX Video SDK, RTX Dynamic Vibrance SDK and/or NRD SDK, if and when made available to you under this license (in each case, the “SDK”).
This license can be accepted only by an adult of legal age of majority in the country in which the SDK is used. If you are under the legal age of majority, you must ask your parent or legal guardian to consent to this license. If you are entering this license on behalf of a company or other legal entity, you represent that you have legal authority and “you” will mean the entity you represent.
By using the SDK, you affirm that you have reached the legal age of majority, you accept the terms of this license, and you take legal and financial responsibility for the actions of your permitted users.

You agree to use the SDK only for purposes that are permitted by (a) this license, and (b) any applicable law, regulation or generally accepted practices or guidelines in the relevant jurisdictions.

1. LICENSE. Subject to the terms of this license and the terms in the supplement attached, NVIDIA hereby grants you a non-exclusive, non-transferable license, without the right to sublicense (except as expressly provided in this license) to:
a. Install and use the SDK,
b. Modify and create derivative works of sample source code delivered in the SDK, and
c. Distribute any software and materials within the SDK, other than developer tools provided for your internal use, as incorporated in object code format into a software application subject to the distribution requirements indicated in this license.

2. DISTRIBUTION REQUIREMENTS. These are the distribution requirements for you to exercise the grants above:
a.	An application must have material additional functionality, beyond the included portions of the SDK.
b.	The following notice shall be included in modifications and derivative works of source code distributed: “This software contains source code provided by NVIDIA Corporation.”
c.	You agree to distribute the SDK subject to the terms at least as protective as the terms of this license, including (without limitation) terms relating to the license grant, license restrictions and protection of NVIDIA’s intellectual property rights. Additionally, you agree that you will protect the privacy, security and legal rights of your application users.
d.	You agree to notify NVIDIA in writing of any known or suspected distribution or use of the SDK not in compliance with the requirements of this license, and to enforce the terms of your agreements with respect to the distributed portions of the SDK.

3. AUTHORIZED USERS. You may allow employees and contractors of your entity or of your subsidiary(ies) to access and use the SDK from your secure network to perform work on your behalf. If you are an academic institution you may allow users enrolled or employed by the academic institution to access and use the SDK from your secure network. You are responsible for the compliance with the terms of this license by your authorized users.

4. LIMITATIONS. Your license to use the SDK is restricted as follows:
a.	You may not reverse engineer, decompile or disassemble, or remove copyright or other proprietary notices from any portion of the SDK or copies of the SDK.
b.	Except as expressly provided in this license, you may not copy, sell, rent, sublicense, transfer, distribute, modify, or create derivative works of any portion of the SDK. For clarity, you may not distribute or sublicense the SDK as a stand-alone product.
c.	 Unless you have an agreement with NVIDIA for this purpose, you may not indicate that an application created with the SDK is sponsored or endorsed by NVIDIA.
d.	 You may not bypass, disable, or circumvent any technical limitation, encryption, security, digital rights management or authentication mechanism in the SDK.
e.	You may not use the SDK in any manner that would cause it to become subject to an open source software license. As examples, licenses that require as a condition of use, modification, and/or distribution that the SDK be: (i) disclosed or distributed in source code form; (ii) licensed for the purpose of making derivative works; or (iii) redistributable at no charge.
f.	 Unless you have an agreement with NVIDIA for this purpose, you may not use the SDK with any system or application where the use or failure of the system or application can reasonably be expected to threaten or result in personal injury, death, or catastrophic loss. Examples include use in avionics, navigation, military, medical, life support or other life critical applications. NVIDIA does not design, test or manufacture the SDK for these critical uses and NVIDIA shall not be liable to you or any third party, in whole or in part, for any claims or damages arising from such uses.
g.	You agree to defend, indemnify and hold harmless NVIDIA and its affiliates, and their respective employees, contractors, agents, officers and directors, from and against any and all claims, damages, obligations, losses, liabilities, costs or debt, fines, restitutions and expenses (including but not limited to attorney’s fees and costs incident to establishing the right of indemnification) arising out of or related to your use of the SDK outside of the scope of this license, or not in compliance with its terms.

5. UPDATES. NVIDIA may, at its option, make available patches, workarounds or other updates to this SDK. Unless the updates are provided with their separate governing terms, they are deemed part of the SDK licensed to you as provided in this license. Further, NVIDIA may, at its option, automatically update the SDK or other software in the system, except for those updates that you may opt-out via the SDK API. You agree that the form and content of the SDK that NVIDIA provides may change without prior notice to you. While NVIDIA generally maintains compatibility between versions, NVIDIA may in some cases make changes that introduce incompatibilities in future versions of the SDK.

6. PRE-RELEASE VERSIONS. SDK versions identified as alpha, beta, preview, early access or otherwise as pre-release may not be fully functional, may contain errors or design flaws, and may have reduced or different security, privacy, availability, and reliability standards relative to commercial versions of NVIDIA software and materials. You may use a pre-release SDK version at your own risk, understanding that these versions are not intended for use in production or business-critical systems. NVIDIA may choose not to make available a commercial version of any pre-release SDK. NVIDIA may also choose to abandon development and terminate the availability of a pre-release SDK at any time without liability.

7. THIRD-PARTY COMPONENTS. The SDK may include third-party components with separate legal notices or terms as may be described in proprietary notices accompanying the SDK. If and to the extent there is a conflict between the terms in this license and the third-party license terms, the third-party terms control only to the extent necessary to resolve the conflict.

8. OWNERSHIP.

8.1 NVIDIA reserves all rights, title and interest in and to the SDK not expressly granted to you under this license. NVIDIA and its suppliers hold all rights, title and interest in and to the SDK, including their respective intellectual property rights. The SDK is copyrighted and protected by the laws of the United States and other countries, and international treaty provisions.

8.2 Subject to the rights of NVIDIA and its suppliers in the SDK, you hold all rights, title and interest in and to your applications and your derivative works of the sample source code delivered in the SDK including their respective intellectual property rights.

9. FEEDBACK. You may, but are not obligated to, provide Feedback to NVIDIA. “Feedback” means all suggestions, fixes, modifications, feature requests or other feedback regarding the SDK. Feedback, even if designated as confidential by you, shall not create any confidentiality obligation for NVIDIA. If you provide Feedback, you hereby grant NVIDIA, its affiliates and its designees a non-exclusive, perpetual, irrevocable, sublicensable, worldwide, royalty-free, fully paid-up and transferable license, under your intellectual property rights, to publicly perform, publicly display, reproduce, use, make, have made, sell, offer for sale, distribute (through multiple tiers of distribution), import, create derivative works of and otherwise commercialize and exploit the Feedback at NVIDIA’s discretion. You will not give Feedback (i) that you have reason to believe is subject to any restriction that impairs the exercise of the grant stated in this section, such as third-party intellectual property rights or (ii) subject to license terms which seek to require any product incorporating or developed using such Feedback, or other intellectual property of NVIDIA or its affiliates, to be licensed to or otherwise shared with any third party.

10. NO WARRANTIES. THE SDK IS PROVIDED AS-IS. TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW NVIDIA AND ITS AFFILIATES EXPRESSLY DISCLAIM ALL WARRANTIES OF ANY KIND OR NATURE, WHETHER EXPRESS, IMPLIED OR STATUTORY, INCLUDING, BUT NOT LIMITED TO, WARRANTIES OF MERCHANTABILITY, NON-INFRINGEMENT, FITNESS FOR A PARTICULAR PURPOSE, USAGE OF TRADE AND COURSE OF DEALING. NVIDIA DOES NOT WARRANT THAT THE SDK WILL MEET YOUR REQUIREMENTS OR THAT THE OPERATION THEREOF WILL BE UNINTERRUPTED OR ERROR-FREE, OR THAT ALL ERRORS WILL BE CORRECTED.

11. LIMITATIONS OF LIABILITY. TO THE MAXIMUM EXTENT PERMITTED BY APPLICABLE LAW NVIDIA AND ITS AFFILIATES SHALL NOT BE LIABLE FOR ANY (I) SPECIAL, INCIDENTAL, PUNITIVE OR CONSEQUENTIAL DAMAGES, OR FOR DAMAGES FOR  (A) ANY LOST PROFITS, PROJECT DELAYS, LOSS OF USE, LOSS OF DATA OR LOSS OF GOODWILL, OR (B) THE COSTS OF PROCURING SUBSTITUTE PRODUCTS, ARISING OUT OF OR IN CONNECTION WITH THIS LICENSE OR THE USE OR PERFORMANCE OF THE SDK, WHETHER SUCH LIABILITY ARISES FROM ANY CLAIM BASED UPON BREACH OF CONTRACT, BREACH OF WARRANTY, TORT (INCLUDING NEGLIGENCE), PRODUCT LIABILITY OR ANY OTHER CAUSE OF ACTION OR THEORY OF LIABILITY, EVEN IF NVIDIA HAS PREVIOUSLY BEEN ADVISED OF, OR COULD REASONABLY HAVE FORESEEN, THE POSSIBILITY OF SUCH DAMAGES. IN NO EVENT WILL NVIDIA’S AND ITS AFFILIATES TOTAL CUMULATIVE LIABILITY UNDER OR ARISING OUT OF THIS LICENSE EXCEED US$10.00. THE NATURE OF THE LIABILITY OR THE NUMBER OF CLAIMS OR SUITS SHALL NOT ENLARGE OR EXTEND THIS LIMIT.

12. TERMINATION. Your rights under this license will terminate automatically without notice from NVIDIA if you fail to comply with any term and condition of this license or if you commence or participate in any legal proceeding against NVIDIA with respect to the SDK. NVIDIA may terminate this license with advance written notice to you, if NVIDIA decides to no longer provide the SDK in a country or, in NVIDIA’s sole discretion, the continued use of it is no longer commercially viable. Upon any termination of this license, you agree to promptly discontinue use of the SDK and destroy all copies in your possession or control. Your prior distributions in accordance with this license are not affected by the termination of this license. All provisions of this license will survive termination, except for the license granted to you.

13. APPLICABLE LAW. This license will be governed in all respects by the laws of the United States and of the State of Delaware, without regard to the conflicts of laws principles. The United Nations Convention on Contracts for the International Sale of Goods is specifically disclaimed. You agree to all terms of this license in the English language. The state or federal courts residing in Santa Clara County, California shall have exclusive jurisdiction over any dispute or claim arising out of this license. Notwithstanding this, you agree that NVIDIA shall still be allowed to apply for injunctive remedies or urgent legal relief in any jurisdiction.

14. NO ASSIGNMENT. This license and your rights and obligations thereunder may not be assigned by you by any means or operation of law without NVIDIA’s permission. Any attempted assignment not approved by NVIDIA in writing shall be void and of no effect. NVIDIA may assign, delegate or transfer this license and its rights and obligations, and if to a non-affiliate you will be notified.

15. EXPORT. The SDK is subject to United States export laws and regulations. You agree to comply with all applicable U.S. and international export laws, including the Export Administration Regulations (EAR) administered by the U.S. Department of Commerce and economic sanctions administered by the U.S. Department of Treasury’s Office of Foreign Assets Control (OFAC). These laws include restrictions on destinations, end-users and end-use. By accepting this license, you confirm that you are not currently residing in a country or region currently embargoed by the U.S. and that you are not otherwise prohibited from receiving the SDK.

16. GOVERNMENT USE. The SDK, documentation and technology (“Protected Items”) are “Commercial products” as this term is defined at 48 C.F.R. 2.101, consisting of “commercial computer software” and “commercial computer software documentation” as such terms are used in, respectively, 48 C.F.R. 12.212 and 48 C.F.R. 227.7202 & 252.227-7014(a)(1). Before any Protected Items are supplied to the U.S. Government, you will (i) inform the U.S. Government in writing that the Protected Items are and must be treated as commercial computer software and commercial computer software documentation developed at private expense; (ii) inform the U.S. Government that the Protected Items are provided subject to the terms of the Agreement; and (iii) mark the Protected Items as commercial computer software and commercial computer software documentation developed at private expense. In no event will you permit the U.S. Government to acquire rights in Protected Items beyond those specified in 48 C.F.R. 52.227-19(b)(1)-(2) or 252.227-7013(c) except as expressly approved by NVIDIA in writing.

17. NOTICES. You agree that any notices that NVIDIA sends you electronically, such as via email, will satisfy any legal communication requirements. Please direct your legal notices or other correspondence to NVIDIA Corporation, 2788 San Tomas Expressway, Santa Clara, California 95051, United States of America, Attention: Legal Department.

18. ENTIRE AGREEMENT. This license is the final, complete and exclusive agreement between the parties relating to the subject matter of this license and supersedes all prior or contemporaneous understandings and agreements relating to this subject matter, whether oral or written. If any court of competent jurisdiction determines that any provision of this license is illegal, invalid or unenforceable, the remaining provisions will remain in full force and effect. Any amendment or waiver under this license shall be in writing and signed by representatives of both parties.

19. LICENSING. If the distribution terms in this license are not suitable for your organization, or for any questions regarding this license, please contact NVIDIA at nvidia-rtx-license-questions@nvidia.com.
(v. March 14, 2024)


NVIDIA RTX SUPPLEMENT TO SOFTWARE LICENSE AGREEMENT FOR NVIDIA SOFTWARE DEVELOPMENT KITS
The terms in this supplement govern your use of the NVIDIA RTX SDKs, including the DLSS SDK, NGX SDK, RTXGI SDK, RTXDI SDK, RTX Video SDK, RTX Dynamic Vibrance SDK and/or NRD SDK, if and when made available to you (in each case, the “SDK”) under the terms of your license agreement (“Agreement”) as modified by this supplement. Capitalized terms used but not defined below have the meaning assigned to them in the Agreement.
This supplement is an exhibit to the Agreement and is incorporated as an integral part of the Agreement. In the event of conflict between the terms in this supplement and the terms in the Agreement, the terms in this supplement govern.

1. Interoperability. Your applications that incorporate, or are based on, the SDK must be fully interoperable with compatible GPU hardware products designed by NVIDIA or its affiliates. Further, the DLSS SDK, NGX SDK and RTX Dynamic Vibrance SDK are licensed for you to develop applications only for their use in systems with NVIDIA GPUs.

2. Game License. You may, but are not obligated to, provide your game or related content (“Game Content”) to NVIDIA. If you provide Game Content, you hereby grant NVIDIA, its affiliates and its designees a non-exclusive, perpetual, irrevocable, worldwide, royalty-free, fully paid-up license, to use the Game Content to improve NVIDIA DLSS SDK and DLSS Model Training.

3. Limitations for the DLSS SDK, NGX SDK,RTX Video SDK and RTX Dynamic Vibrance SDK. Your applications that incorporate, or are based on, the DLSS SDK, NGX SDK, RTX Video SDK or RTX Dynamic Vibrance SDK may be deployed in a cloud service that runs on systems that consume NVIDIA vGPU software, and any other cloud service use of such SDKs or their functionality is outside of the scope of the Agreement. For the purpose of this section, cloud services include application service providers or service bureaus, operators of hosted/virtual system environments, or hosting, time sharing or providing any other type of service to others.

4. Notification for the DLSS SDK, NGX SDK and RTX Dynamic Vibrance SDK. You are required to notify NVIDIA prior to commercial release of an application (including a plug-in to a commercial application) that incorporates, or is based on, the DLSS SDK, NGX SDK or RTX Dynamic Vibrance SDK. Please send notifications to: https://developer.nvidia.com/sw-notification and provide the following information in the email: company name, publisher and developer name, NVIDIA SDK used, application name, platform (i.e. PC, Linux), scheduled ship date, and weblink to product/video.

5. Audio and Video Encoders and Decoders. You acknowledge and agree that it is your sole responsibility to obtain any additional third-party licenses required to make, have made, use, have used, sell, import, and offer for sale your products or services that include or incorporate any third-party software and content relating to audio and/or video encoders and decoders from, including but not limited to, Microsoft, Thomson, Fraunhofer IIS, Sisvel S.p.A., MPEG-LA, and Coding Technologies. NVIDIA does not grant to you under this Agreement any necessary patent or other rights with respect to any audio and/or video encoders and decoders.

6. SDK Terms.
6.1 Over the Air Updates. By installing or using the SDK you agree that NVIDIA can make over-the-air updates of the SDK in systems that have the SDK installed, including (without limitation) for quality, stability or performance improvements or to support new hardware.

6.2 SDK Integration. If you publicly release a DLSS integration in an end user game or application that presents material stability, performance, image quality, or other technical issues impacting the user experience, you will work to quickly address the integration issues. In the case issues are not addressed, NVIDIA reserves the right, as a last resort, to temporarily disable the DLSS integration until the issues can be fixed.

7. Marketing.
7.1 Marketing Activities. Your license to the SDK(s) under the Agreement is subject to your compliance with the following marketing terms:
(a)  Identification by You in the DLSS SDK, NGX SDK, RTX Video SDK or Dynamic Vibrance SDK.  During the term of the Agreement, NVIDIA agrees that you may identify NVIDIA on your websites, printed collateral, trade-show displays and other retail packaging materials, as the supplier of the DLSS SDK,  NGX SDK, RTX Video SDK or Dynamic Vibrance SDK for the applications that were developed with use of such SDKs, provided that all such references to NVIDIA will be subject to NVIDIA's prior review and written approval, which will not be unreasonably withheld or delayed.
(b)  NVIDIA Trademark Placement in Applications with the DLSS SDK, NGX SDK, or RTX Video SDK.  For applications that incorporate the DLSS SDK or NGX SDK or portions thereof, you must attribute the use of the applicable SDK and include the NVIDIA Marks on splash screens, in the about box of the application (if present), and in credits for game applications.
(c) NVIDIA Trademark Placement in Applications with a licensed SDK, other than the DLSS SDK, RTX Video SDK or NGX SDK. For applications that incorporates and/or makes use of a licensed SDK, other than the DLSS SDK, RTX Video SDK or NGX SDK, you must attribute the use of the applicable SDK and include the NVIDIA Marks on the credit screen for applications that have such credit screen, or where a credit screen is not present prominently in end user documentation for the application.
(d) Identification by NVIDIA in the DLSS SDK, NGX SDK, RTX Video SDK or Dynamic Vibrance SDK.  You agree that NVIDIA may identify you on NVIDIA's websites, printed collateral, trade-show displays, and other retail packaging materials as an individual or entity that produces products and services which incorporate the DLSS SDK, NGX SDK, RTX Video SDK or Dynamic Vibrance SDK as applicable. To the extent that you provide NVIDIA with input or usage requests with regard to the use of your logo or materials, NVIDIA will use commercially reasonable efforts to comply with such requests.  For the avoidance of doubt, NVIDIA’s rights pursuant to this section shall survive any expiration or termination of the Agreement with respect to existing applications which incorporate the DLSS SDK, RTX Video SDK or NGX SDK.
(e) Applications Marketing Material in the DLSS SDK, NGX SDK, RTX Video SDK or Dynamic Vibrance SDK. You may provide NVIDIA with screenshots, imagery, and video footage of applications representative of your use of the NVIDIA DLSS SDK or NGX SDKs in your application (collectively, “Assets”). You hereby grant to NVIDIA the right to create and display self-promotional demo materials using the Assets, and after release of the application to the public to distribute, sub-license, and use the Assets to promote and market the NVIDIA RTX SDKs.  To the extent you provide NVIDIA with input or usage requests with regard to the use of your logo or materials, NVIDIA will use commercially reasonable efforts to comply with such requests.  For the avoidance of doubt, NVIDIA’s rights pursuant to this section shall survive any termination of the Agreement with respect to applications which incorporate the NVIDIA RTX SDK.

7.2 Trademark Ownership and Licenses. Trademarks are owned and licenses as follows:
(a)	Ownership of Trademarks.  Each party owns the trademarks, logos, and trade names (collectively "Marks") for their respective products or services, including without limitation in applications, and the NVIDIA RTX SDKs. Each party agrees to use the Marks of the other only as permitted in this exhibit.

(b)	Trademark License to NVIDIA.  You grant to NVIDIA a non-exclusive, non-sub licensable, non-transferable (except as set forth in the assignment provision of the Agreement), worldwide license to refer to you and your applications, and to use your Marks on NVIDIA's marketing materials and on NVIDIA's website (subject to any reasonable conditions of you) solely for NVIDIA’s marketing activities set forth in this exhibit Sections 7 (d)-(e) above.  NVIDIA will follow your specifications for your Marks as to style, color, and typeface as reasonably provided to NVIDIA.

(c)	Trademark License to You.  NVIDIA grants to you a non-exclusive, non-sub licensable, non-transferable (except as set forth in the assignment provision of the Agreement), worldwide license, subject to the terms of this exhibit and the Agreement, to use NVIDIA RTX™, NVIDIA GeForce RTX™ in combination with GeForce products, and/or NVIDIA Quadro RTX™  in combination with Quadro products (collectively, the “NVIDIA Marks”) on your marketing materials and on your website (subject to any reasonable conditions of NVIDIA) solely for your marketing activities set forth in this exhibit Sections 7.1 (a)-(c) above.  For the avoidance of doubt, you will not and will not permit others to use any NVIDIA Mark for any other goods or services, or in a way that tarnishes, degrades, disparages or reflects adversely any of the NVIDIA Marks or NVIDIA’s business or reputation, or that dilutes or otherwise harms the value, reputation or distinctiveness of or NVIDIA’s goodwill in any NVIDIA Mark. In addition to the termination rights set forth in the Agreement, NVIDIA may terminate this trademark license at any time upon written notice to you. You will follow NVIDIA's use guidelines and specifications for NVIDIA's Marks as to style, color and typeface as provided in NVIDIA Marks and submit a sample of each proposed use of NVIDIA's Marks at least two (2) weeks prior to the desired implementation of such use to obtain NVIDIA's prior written approval (which approval will not be unreasonably withheld or delayed).  If NVIDIA does not respond within ten (10) business days of your submission of such sample, the sample will be deemed unapproved. All goodwill associated with use of NVIDIA Marks will inure to the sole benefit of NVIDIA. For the RTX Video SDK, contact NVIDIA at nvidia-rtx-video-sdk-license-questions@nvidia.com.

7.3 Use Guidelines. Use of the NVIDIA Marks is subject to the following guidelines:
(a)	Business Practices.  You covenant that you will: (a)  conduct business with respect to NVIDIA’s products in a manner that reflects favorably at all times on the good name, goodwill and reputation of such products; (b) avoid deceptive, misleading or unethical practices that are detrimental to NVIDIA, its customers, or end users; (c) make no false or misleading representations with regard to NVIDIA or its products; and (d) not publish or employ or cooperate in the publication or employment of any misleading or deceptive advertising or promotional materials.

(b)	No Combination Marks or Similar Marks.  You agree not to (a) combine NVIDIA Marks with any other content without NVIDIA’s prior written approval, or (b) use any other trademark, trade name, or other designation of source which creates a likelihood of confusion with NVIDIA Marks.

(v. March 14, 2024)
```

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


### MFGAmpereUnlock-RenoDx

MIT License

Copyright (c) 2026 ImDreamt
Copyright (c) 2026 mavismmg
Copyright (c) 2026 nefh

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


### NVAPI public architecture-query ABI

MIT License

Copyright (c) 2019-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.

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
### CommonLibSSE-NG 8.1.0 exceptions

The upstream README identifies Skyrim and proprietary hardware drivers/SDKs
as Modded Code, and SKSE and Windows as Modding Libraries. The following is
the pinned dependency's unmodified EXCEPTIONS.md text.

This Program is intended to be used with and modify existing code (the "Modded Code") and to build a robust modding community with open source principles. The purpose of this exception is to address issues when an open source modding community interacts with potentially proprietary code. In addition, the modding community often uses libraries (the "Modding Libraries") under licenses that may be incompatible with the GPL ("Modding Library Licenses").

===

Modding Exception

In addition, as a special exception, the authors give You the additional right to link the code of this Program with the existing code that this Program is intended to be used with or modify and to distribute linked combinations including the two, subject to the limitations in this paragraph. Modded Code permitted under this exception may link to the code of this Program without causing the Modded Code and portion of the combined work corresponding to the Modded Code to be covered by the GNU General Public License. You must obey the GNU General Public License in all respects for all of the Program code and other code used in conjunction with the Program except the Modded Code covered by this exception. If you modify this file, you may extend this exception to your version of the file, but you are not obligated to do so. If you do not wish to provide this exception without modification, you must delete this exception statement from your version and license this file solely under the GPL without exception.

===

GPL-3.0 Linking Exception (with Corresponding Source)

Additional permission under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or combining it with Modding Libraries (or a modified version thereof), containing parts covered by the terms of Modding Library Licenses, the licensors of this Program grant you additional permission to convey the resulting work. Corresponding Source for a non-source form of such a combination shall include the source code for the parts of Modding Libraries used as well as that of the covered work.

### HDE64 (from MinHook v1.3.4)

Hacker Disassembler Engine 64 C
Copyright (c) 2008-2009, Vyacheslav Patkov.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
