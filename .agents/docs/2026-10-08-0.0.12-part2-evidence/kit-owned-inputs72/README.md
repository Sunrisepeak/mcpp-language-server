# Final72 kit input refinement

This preparation has not generated a kit. The previous4025-file whole-include
capture remains unchanged. A new1422-file capture follows the official kit
recipe's `dpkg -L libc6-dev linux-libc-dev` ownership filter. All selected bytes
match the original Ubuntu20 input; package versions are unchanged. The two
existing license files cover the selected package input. Other build-dependency
headers are outside the new kit input.

The staged kit generator now runs inside the cached offline Ubuntu20 image,
using full72 source and its compiler. This fixes configure-time OS/toolchain
checks as well as C/kernel header data. CMake/ninja/compiler versions will be
logged when it actually runs. Container-local Git configuration leaves the host
unchanged. The Docker command has a distinct container ID and explicitly removes
its owned container on timeout. Source, engine and kit hashes remain prerequisites;
this preparation is not product/runtime-floor or immutable payload qualification.
