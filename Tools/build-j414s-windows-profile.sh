#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

usage() {
    echo "usage: $0 baseline|ans|ans-noacpi|gpu|gpu-noacpi|ans-gpu|wireless|gpu-wireless|ans-gpu-wireless|media|media-gpu|ans-gpu-wireless-media|battery" >&2
    exit 2
}

test "$#" -eq 1 || usage
profile=$1
case "$profile" in
    baseline) ;;
    ans) ;;
    # Same FFS set as `ans`, NTAS2003 withheld. Single-variable control
    # for the BUGCODE_USB3_DRIVER 0x144 investigation.
    ans-noacpi) ;;
    gpu) ;;
    # Rails-down, no ANS, and NTAS0023 NOT published. This is the ONLY cell in
    # which an XHC2 A/B carries information: measured over all 49 boot logs that
    # reached Windows storage, rails-down non-ANS profiles show storage-death
    # 5/8 with XHC2 on vs 0/8 with it off (Fisher one-tailed p = 0.013), while
    # ans-gpu-wireless fails 9/11 with XHC2 ALREADY OFF -- so a sample there
    # says nothing. It also excludes GPU publication, which independently
    # stalls Windows before storage (measured 2026-07-31, 3292c5b).
    gpu-noacpi) ;;
    ans-gpu) ;;
    # 2026-08-02: ans-gpu-wireless with NTAS2003 withheld.  ANS is still
    # brought up and quiesced by the DXE -- only the ACPI publication is off --
    # so Windows never builds a devnode and AppleNvme never starts.  This is
    # what "ANS off" has to mean on this platform: `ans-live` (ans=FALSE) left
    # ps_ans2 unpowered and AppleNvme took a synchronous external abort at the
    # SART (0x34bc50010) that killed the boot.
    ans-noacpi-gpu-wireless) ;;
    # CORRECTED 2026-07-30: wireless used to require a second argument -- a
    # same-instance, hardware-captured handoff manifest sealing one specific
    # coordinator-chosen reservation address into this build. That is exactly
    # the hardcoding the end user objected to. MemoryInitPeiLib.c now derives
    # the reservation at PEI runtime from that boot's own boot_args, so
    # wireless takes no manifest and needs no more evidence at build time
    # than ans or gpu do.
    wireless) ;;
    gpu-wireless) ;;
    ans-gpu-wireless) ;;
    # 2026-08-01: the GPU-free counterpart of ans-gpu-wireless.  Turning only
    # NTAS0023 publication off (gpu_acpi=0) is NOT GPU-free: that build still
    # sets NTASI_J414S_GPU_RESOURCE_PROFILE=1 and carves the GPU reservations
    # out of the memory map with no device to own them.  gpu=0 turns both off.
    ans-wireless) ;;
    # ANS DXE never runs, NTAS2003 still published -- see PlatformBuild.py.
    ans-live) ;;

    # Publishes MCA0 (NTAS0080), AOPA (NTAS0081) and ISP0 (NTAS0090) on top of
    # baseline, and nothing else: no FFS module, no static ACPI table, no
    # interrupt resource, no CSRT change. Single variable on top of the
    # configuration that is known to boot.
    media) ;;
    # media + GPU carveouts, no ANS, no wireless. Carried in both profile dicts
    # since the GPU work landed; it was unbuildable only because this case
    # statement never learned about it.
    media-gpu) ;;
    # ANS + GPU + wireless + media together; CSRT variant m2-pro-media-gpu.
    ans-gpu-wireless-media) ;;
    # Publishes BAT0 (NTAS0053) on top of baseline and nothing else. The
    # device has an EMPTY _CRS -- no memory window, no interrupt -- so it
    # allocates no GSIV, changes no CSRT byte, and cannot take a resource
    # away from a devnode that already boots. Single variable on top of the
    # configuration that is known to boot.
    battery) ;;
    *) usage ;;
esac

source_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_parent=$(dirname -- "$source_root")
expected_branch=main
branch=$(git -C "$source_root" branch --show-current)
commit=$(git -C "$source_root" rev-parse HEAD)
git_dir=$(git -C "$source_root" rev-parse --absolute-git-dir)

if test "$git_dir" != "$source_root/.git"; then
    echo "error: $source_root is not the standalone unified Mu checkout" >&2
    exit 1
fi
if test "$branch" != "$expected_branch"; then
    echo "error: expected $expected_branch, found $branch" >&2
    exit 1
fi
if test -n "$(git -C "$source_root" status --porcelain=v1 --untracked-files=all --ignore-submodules=none)"; then
    echo "error: unified Mu source, build tooling, or a pinned top-level submodule is dirty" >&2
    exit 1
fi

# WinPE deploy-verdict echo. Orthogonal to the profile, OFF unless asked for, and
# recorded in the sealed manifest's build defines so a boot can always be told
# apart from one produced without it. See MacBookProEarly2023.dsc.
evidence_echo=${NTASI_DEPLOY_EVIDENCE_ECHO:-0}
case "$evidence_echo" in
    0|1) ;;
    *) echo "error: NTASI_DEPLOY_EVIDENCE_ECHO must be 0 or 1" >&2; exit 2 ;;
esac

image=${NTASI_MU_BUILD_IMAGE:-ntasi-m2-pro-mu-builder-fast:ubuntu-24.04-arm64-v1-d3e92152f8d175df}
output_root=${NTASI_MU_OUTPUT_ROOT:-$source_parent/apple_silicon_nt_drivers/build/m2-pro}
output_dir=$output_root/$profile/$commit
build_dir=$output_dir/Build
conf_dir=$output_dir/Conf
artifact_dir=$output_dir/artifacts
lock_dir=$output_dir/.build-lock

mkdir -p "$output_dir"
if ! mkdir "$lock_dir" 2>/dev/null; then
    echo "error: profile build is already running: $output_dir" >&2
    exit 1
fi
trap 'rmdir "$lock_dir" 2>/dev/null || true' EXIT HUP INT TERM
mkdir -p "$build_dir" "$conf_dir" "$artifact_dir"

docker image inspect "$image" >/dev/null
image_id=$(docker image inspect --format '{{.Id}}' "$image")
image_repo_digests_json=$(docker image inspect --format '{{json .RepoDigests}}' "$image")
docker run --rm --platform linux/arm64 \
    --read-only \
    --tmpfs /tmp:rw,exec,nosuid,size=4g \
    --tmpfs /root:rw,nosuid,size=1g \
    -e CONF_PATH=/work/Conf \
    -e GIT_CONFIG_COUNT=2 \
    -e GIT_CONFIG_KEY_0=safe.directory \
    -e GIT_CONFIG_VALUE_0=/work \
    -e GIT_CONFIG_KEY_1=diff.ignoreSubmodules \
    -e GIT_CONFIG_VALUE_1=all \
    -e NTASI_MU_PROFILE="$profile" \
    -e NTASI_DEPLOY_EVIDENCE_ECHO="$evidence_echo" \
    -e NTASI_M2_PRO_MU_REFRESH=never \
    -e NTASI_M2_PRO_MU_RECIPE_IMAGE="$image" \
    -v "$source_root:/work:ro" \
    -v "$build_dir:/work/Build:rw" \
    -v "$conf_dir:/work/Conf:rw" \
    "$image"

fd=$build_dir/MacBookProEarly2023-AARCH64/DEBUG_CLANGPDB/FV/MACBOOKPROEARLY2023_EFI.fd
test -f "$fd"
artifact=$artifact_dir/MACBOOKPROEARLY2023_EFI.fd
cp "$fd" "$artifact"

manifest=$artifact_dir/manifest.json
python3 "$source_root/Tools/j414s_mu_profile_manifest.py" seal \
    --source-root "$source_root" \
    --output-root "$output_dir" \
    --profile "$profile" \
    --image-ref "$image" \
    --image-id "$image_id" \
    --image-repo-digests-json "$image_repo_digests_json" \
    --evidence-echo "$evidence_echo"

if command -v sha256sum >/dev/null 2>&1; then
    fd_sha=$(sha256sum "$artifact" | awk '{print $1}')
    manifest_sha=$(sha256sum "$manifest" | awk '{print $1}')
else
    fd_sha=$(shasum -a 256 "$artifact" | awk '{print $1}')
    manifest_sha=$(shasum -a 256 "$manifest" | awk '{print $1}')
fi

echo "READY_TO_TEST $profile"
echo "source=$commit"
echo "evidence_echo=$evidence_echo"
echo "fd=$artifact"
echo "sha256=$fd_sha"
echo "manifest=$manifest"
echo "manifest_sha256=$manifest_sha"
