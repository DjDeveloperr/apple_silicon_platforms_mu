# Building Mu

AuroraSilicon uses native incremental builds. Docker is not part of the build
workflow.

Preferred entry point:

```sh
abg boot prepare [--no-ans] [--no-wireless] [--no-xhc2] [--usb3-left]
```

ABG selects a safe profile, verifies prerequisites, rebuilds only when the
source fingerprint changed, and verifies the sealed artifact. Add
`--force-build` only to discard that cache decision.

Direct firmware build:

```sh
Tools/build-j414s-windows-native.sh <profile>
```

The authoritative profile list is `PROFILES` in
`Tools/j414s_mu_profile_manifest.py`. Native output defaults to the main
AuroraSilicon repository:

```text
../AuroraSilicon/build/m2-pro/<profile>/<source-commit>/
```

Override it with `AURORADBG_MU_OUTPUT_ROOT`. Required host tools are Python
3.11 or 3.12, Homebrew LLVM/lld, `iasl`, `nasm`, and `make`. Submodules must be
initialized recursively.

Verify an artifact without rebuilding:

```sh
.venv-native/bin/python Tools/j414s_mu_profile_manifest.py verify \
  --manifest <artifact-directory>/manifest.json \
  --source-root .
```

Only DEBUG/CLANGPDB firmware is currently supported by the J414s native
builder.
