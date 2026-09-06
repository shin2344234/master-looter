# Third-party notices

## Trinity
`src/hooks/dx12_hook.cpp`, `dx12_hook.h`, `hdr_composite_shader.h`, `xinput_hook.cpp` and `xinput_hook.h` are adapted from Trinity, https://github.com/XeTrinityz/Trinity, by `scripts/adapt_trinity_dx12.py`.

MIT License

Copyright (c) 2026 XeTrinityz

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## Dear ImGui
Fetched at build time from https://github.com/ocornut/imgui (MIT License, Copyright (c) 2014-2025 Omar Cornut).

## MinHook
Fetched at build time from https://github.com/TsudaKageyu/minhook (BSD 2-Clause License, Copyright (C) 2009-2017 Tsuda Kageyu). Its bundled HDE disassembler is used by `src/loot/farhook.cpp` to measure function prologues.

## Research credits
The loot engine in `src/loot` follows the reverse engineering published with CDLoot for Crimson Desert: the loot event protocol and descriptors, the entity and component layout, the ownership (Take or Steal) oracle and the node-arming call. `src/loot/signatures.h` records which of its patterns and offsets came from there. No CDLoot code is included. The debt is knowing where to look.

Ultimate ASI Loader (winmm.dll) is required to load the plugin but is not part of this repository.
