# Dear ImGui public headers

`imconfig.h`, `imgui.h` and `LICENSE.txt` (MIT) of Dear ImGui **1.92.8 WIP** (`IMGUI_VERSION_NUM` 19277), copied byte for byte from sc-offline's `src/third_party/imgui/` at commit 1ee0727cc63866a95e709ae61dd5dd355b511253. They are the headers of the ImGui the product links, so a plugin that draws in an `sco.ui` tab or overlay compiles against the same version. sc-offline's CI fails if its copies and these differ.

Use them unmodified, including the host's `imconfig.h`: `IMGUI_DEFINE_MATH_OPERATORS` is not defined (its line is commented out), so don't define it or other `imconfig.h` options in your plugin. `imgui_internal.h` is not shipped and not part of the contract.
