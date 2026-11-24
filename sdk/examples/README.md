# Public SDK inspector

`inspect.cpp` uses only the installed public header and compiled SDK. It needs
no DriverKit headers or SDK, DEXT or QEMU implementation sources. A C++17 build
against an existing SDK package is:

```sh
clang++ -std=c++17 -Wall -Wextra -Werror -pedantic \
  -I "$SDK_PREFIX/include" sdk/examples/inspect.cpp \
  -L "$SDK_PREFIX/lib" -lextgpu -Wl,-rpath,"$SDK_PREFIX/lib" \
  -o "$PRIVATE_BUILD/inspect"
```

Root owns compilation and validation. The example is not built or run by this
documentation. A software-only functional test must bind a private executable
directly to the isolated model SDK DSO, verify no real-SDK fallback, and supply
the model constructor's mandatory fresh-log environment. Linking an ordinary
SDK package alone does not provide that isolation. Model results prove software
API behavior, not physical hardware operation.

The executable emits one JSON report. With no arguments it only enumerates
IORegistry; it opens no device and never chooses the first result. An explicit
`--inspect-id ID` opens only that exact driver registry ID and reads protocol
and identity, then closes it. IDs are nonzero uint64 values in unsigned decimal
or `0x` hexadecimal, with no signs, whitespace, trailing text or overflow.

Adding `--claim-pristine` explicitly acquires ownership. The current OpenPCI
driver leaves MSE/BME unchanged when claiming. This mode reads only sixteen aligned
32-bit words of the standard PCI header and memory BAR metadata. It accepts
only a type-0 header, skips address high halves of 64-bit BARs, and records absent
BARs as `NOT_FOUND` and unavailable metadata as `UNSUPPORTED`; both statuses
remain visible with null metadata. Other BAR errors fail the operation. It
performs no configuration writes, BAR mapping, MMIO,
DMA, IRQ setup or reset. It then requests acknowledged owner release and close.
The SDK performs its own protocol/generation checks.

The report retains operation and cleanup errors independently. A failed or
uncertain claim, release or close stops cleanup. The example emits
`recovery_needed_process_held`, reports its PID/live handles and remains alive.
SIGINT and SIGTERM defer exit; they do not drop the owner. There are no automatic
cleanup retries or forced closes. SIGPIPE is ignored so a closed report pipe
cannot terminate an uncertain owner. The operator must arrange physical/device
recovery before explicitly terminating it externally; SIGKILL and process or
machine failure cannot be prevented. Process exit never proves safe release.
Even a `NOT_CLAIMED` release reply is reported separately and held for review
after this example observed a successful claim. Session-destruction failure
also holds the process. The positive path exits only after acknowledgements.

Exit codes are `0` for success/help, `1` for an SDK/operation/cleanup failure and
`2` for invalid arguments. Ownership/cleanup failures hold instead of exiting.
Neither successful inspection nor a model pristine
claim proves GPU initialization, transfers, interrupts or rendering. Physical
hardware remains quarantined after the run06 incident; these instructions do
not authorize a hardware run.
