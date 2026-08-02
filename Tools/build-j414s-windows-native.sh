#!/bin/sh
# SPDX-License-Identifier: MIT
# Native Apple Silicon host build for the J414s Windows firmware profiles.
set -eu

usage() {
    echo "usage: $0 <profile>" >&2
    exit 2
}

test "$#" -eq 1 || usage
profile=$1
source_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
profile_tool=$source_root/Tools/j414s_mu_profile_manifest.py
mu_python=${AURORADBG_MU_PYTHON:-$(command -v python3.12 || command -v python3)}
test -x "$mu_python" || {
    echo "error: Python 3.11 or 3.12 is required for the pinned Mu tools" >&2
    exit 1
}
"$mu_python" -c 'import sys; raise SystemExit(0 if (3, 11) <= sys.version_info[:2] < (3, 13) else 1)' || {
    echo "error: pinned Mu Python tools require Python 3.11 or 3.12: $mu_python" >&2
    exit 1
}

# The manifest is the profile authority. This avoids a third hand-maintained
# profile list drifting from the policy and compiler tables.
"$mu_python" - "$profile_tool" "$profile" <<'PY'
import ast, pathlib, sys
tree = ast.parse(pathlib.Path(sys.argv[1]).read_text())
for node in tree.body:
    if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "PROFILES" for t in node.targets):
        profiles = ast.literal_eval(node.value)
        break
else:
    raise SystemExit("profile manifest has no PROFILES table")
if sys.argv[2] not in profiles:
    raise SystemExit(f"unknown Mu profile: {sys.argv[2]}")
PY

test "$(uname -s)" = Darwin || {
    echo "error: native J414s build currently supports macOS only" >&2
    exit 1
}
test "$(uname -m)" = arm64 || {
    echo "error: native J414s build requires an Apple Silicon host" >&2
    exit 1
}

for submodule in MU_BASECORE Common/MU Common/TIANO Common/MU_OEM_SAMPLE \
    Silicon/ARM/TIANO Common/MU_DFCI mu_feature_debugger; do
    test -e "$source_root/$submodule/.git" || {
        echo "error: submodule is not initialized: $submodule" >&2
        echo "run: git -C '$source_root' submodule update --init --recursive" >&2
        exit 1
    }
done

llvm_bin=${AURORADBG_LLVM_BIN:-/opt/homebrew/opt/llvm/bin}
for tool in clang llvm-lib llvm-rc; do
    test -x "$llvm_bin/$tool" || {
        echo "error: Homebrew LLVM tool missing: $llvm_bin/$tool" >&2
        exit 1
    }
done
lld_link=${AURORADBG_LLD_LINK:-$(command -v lld-link || true)}
test -n "$lld_link" && test -x "$lld_link" || {
    echo "error: lld-link is missing (set AURORADBG_LLD_LINK or install Homebrew lld)" >&2
    exit 1
}
for tool in make iasl nasm; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "error: required native build tool is missing: $tool" >&2
        exit 1
    }
done

venv=${NTASI_MU_NATIVE_VENV:-$source_root/.venv-native}
if test ! -x "$venv/bin/stuart_build"; then
    # --clear also repairs a partial environment created by an unsupported
    # Homebrew `python3` (currently 3.14); pinned pygount only supports <3.13.
    "$mu_python" -m venv --clear "$venv"
    "$venv/bin/python" -m pip install --disable-pip-version-check -r "$source_root/pip-requirements.txt"
    # edk2-pytool-extensions 0.27.6 still imports pkg_resources.
    "$venv/bin/python" -m pip install --disable-pip-version-check 'setuptools<81'
fi

commit=$(git -C "$source_root" rev-parse HEAD)
if test -n "$(git -C "$source_root" status --porcelain=v1 --untracked-files=all --ignore-submodules=none)"; then
    echo "note: unified Mu tree is dirty; manifest will record clean=false" >&2
fi
output_root=${NTASI_MU_OUTPUT_ROOT:-$(dirname "$source_root")/apple_silicon_nt_drivers/build/m2-pro}
output_dir=$output_root/$profile/$commit
build_dir=$output_dir/Build
conf_dir=$output_dir/Conf
artifact_dir=$output_dir/artifacts
toolchain_dir=$output_dir/native-toolchain
lock_dir=$output_dir/.build-lock
mkdir -p "$output_dir"
if ! mkdir "$lock_dir" 2>/dev/null; then
    echo "error: profile build is already running: $output_dir" >&2
    exit 1
fi

cleanup() {
    test -L "$source_root/Build" && rm "$source_root/Build"
    test -L "$source_root/Conf" && rm "$source_root/Conf"
    rmdir "$lock_dir" 2>/dev/null || true
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$build_dir" "$conf_dir" "$artifact_dir"
mkdir -p "$toolchain_dir"
native_log=$output_dir/native-build.log
: > "$native_log"
run_logged() {
    label=$1
    shift
    echo "$label"
    if ! "$@" >> "$native_log" 2>&1; then
        echo "error: $label failed; tail of $native_log:" >&2
        tail -80 "$native_log" >&2
        exit 1
    fi
}
# EDK2's CLANGPDB definition accepts one prefix for clang, llvm-lib, llvm-rc,
# and lld-link. Homebrew deliberately ships lld as a separate formula, so
# assemble a profile-local prefix instead of changing global Homebrew links.
ln -sfn "$llvm_bin/clang" "$toolchain_dir/clang"
ln -sfn "$llvm_bin/llvm-lib" "$toolchain_dir/llvm-lib"
ln -sfn "$llvm_bin/llvm-rc" "$toolchain_dir/llvm-rc"
ln -sfn "$lld_link" "$toolchain_dir/lld-link"
test ! -e "$source_root/Build" || {
    echo "error: $source_root/Build already exists; native output cannot be isolated" >&2
    exit 1
}
test ! -e "$source_root/Conf" || {
    echo "error: $source_root/Conf already exists; native config cannot be isolated" >&2
    exit 1
}
ln -s "$build_dir" "$source_root/Build"
ln -s "$conf_dir" "$source_root/Conf"

base_tools=$source_root/MU_BASECORE/BaseTools
base_tools_work=$output_dir/native-basetools
base_tools_bin=$base_tools_work/bin
base_tools_wrappers=$base_tools/BinWrappers/PosixLike
if test ! -x "$base_tools_bin/GenFfs" || test ! -x "$base_tools_bin/VfrCompile"; then
    # Build from an output-local copy. Building in MU_BASECORE/Source/C leaves
    # object files inside a locked nested gitlink and invalidates the source
    # provenance hash even though every tracked byte is unchanged.
    mkdir -p "$base_tools_work"
    ditto "$base_tools/Source/C" "$base_tools_work"
    run_logged "Building native EDK2 BaseTools" make -C "$base_tools_work" \
        EDK2_PATH="$source_root/MU_BASECORE" \
        -j "${AURORADBG_MU_JOBS:-$(sysctl -n hw.logicalcpu)}"
fi
# Mu's published BaseTools archive contains Linux and Windows binaries only.
# Stuart's host-specific dependency resolver still needs a Darwin directory,
# so point its ignored extdep cache at the C tools we just built from the
# pinned source. The POSIX wrappers supply the Python `build` frontend.
host_tools=$base_tools/Bin/Mu-Basetools_extdep/MacOs-ARM-64
if test -e "$host_tools" && test ! -L "$host_tools"; then
    echo "error: refusing to replace non-symlink native BaseTools directory: $host_tools" >&2
    exit 1
fi
mkdir -p "$(dirname "$host_tools")"
ln -sfn "$base_tools_bin" "$host_tools"

export PATH="$venv/bin:$toolchain_dir:$llvm_bin:$base_tools_wrappers:$PATH"
export CLANG_BIN="$toolchain_dir/"
export CLANG_HOST_BIN="/usr/bin/"
export UNIX_IASL_BIN="$(command -v iasl)"
export CONF_PATH="$conf_dir"
export NTASI_MU_PROFILE="$profile"
export NTASI_DEPLOY_EVIDENCE_ECHO="${NTASI_DEPLOY_EVIDENCE_ECHO:-0}"

platform_build=Platform/MacBookProEarly2023Pkg/PlatformBuild.py
stamp=$build_dir/.auroradbg-native-setup-v1
cd "$source_root"
if test ! -f "$stamp" || test "${NTASI_MU_NATIVE_REFRESH:-auto}" = always; then
    run_logged "Preparing native Mu workspace" stuart_setup -c "$platform_build" TOOL_CHAIN_TAG=CLANGPDB TARGET=DEBUG
    run_logged "Updating pinned Mu dependencies" stuart_update -c "$platform_build" TOOL_CHAIN_TAG=CLANGPDB TARGET=DEBUG
    {
        shasum -a 256 "$platform_build" pip-requirements.txt NTASI-AIC2-OVERLAY.json
        "$llvm_bin/clang" --version | head -1
    } > "$stamp"
fi
run_logged "Building $profile with native CLANGPDB" stuart_build -c "$platform_build" TOOL_CHAIN_TAG=CLANGPDB TARGET=DEBUG

fd=$build_dir/MacBookProEarly2023-AARCH64/DEBUG_CLANGPDB/FV/MACBOOKPROEARLY2023_EFI.fd
test -f "$fd"
cp "$fd" "$artifact_dir/MACBOOKPROEARLY2023_EFI.fd"
# The build-time links are implementation details, not source changes. Remove
# them before the manifest samples Git state so a clean native build is sealed
# as clean instead of recording the two temporary paths as provenance drift.
rm "$source_root/Build" "$source_root/Conf"
llvm_identity=$(
    {
        "$llvm_bin/clang" --version
        "$lld_link" --version
        "$mu_python" --version
        shasum -a 256 "$base_tools_bin/GenFfs" "$base_tools_bin/VfrCompile"
    } | shasum -a 256 | awk '{print $1}'
)
"$venv/bin/python" "$profile_tool" seal \
    --source-root "$source_root" \
    --output-root "$output_dir" \
    --profile "$profile" \
    --image-ref "native:darwin-arm64" \
    --image-id "sha256:$llvm_identity" \
    --image-repo-digests-json '[]' \
    --builder-platform "darwin/arm64" \
    --evidence-echo "$NTASI_DEPLOY_EVIDENCE_ECHO"

artifact=$artifact_dir/MACBOOKPROEARLY2023_EFI.fd
manifest=$artifact_dir/manifest.json
echo "READY_TO_TEST $profile"
echo "source=$commit"
echo "backend=native-darwin-arm64"
echo "log=$native_log"
echo "fd=$artifact"
echo "sha256=$(shasum -a 256 "$artifact" | awk '{print $1}')"
echo "manifest=$manifest"
echo "manifest_sha256=$(shasum -a 256 "$manifest" | awk '{print $1}')"
