#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

usage() {
    echo "usage: $0 baseline|ans|gpu|ans-gpu|wireless [wireless-handoff-manifest.json]" >&2
    exit 2
}

test "$#" -ge 1 && test "$#" -le 2 || usage
profile=$1
case "$profile" in
    baseline)
        ;;
    ans)
        ;;
    gpu)
        ;;
    ans-gpu)
        ;;
    wireless)
        test "$#" -eq 2 || usage
        wireless_manifest=$(CDPATH= cd -- "$(dirname -- "$2")" && pwd)/$(basename -- "$2")
        test -f "$wireless_manifest" || { echo "error: missing wireless handoff manifest" >&2; exit 1; }
        ;;
    *) usage ;;
esac
if test "$profile" != wireless && test "$#" -ne 1; then
    usage
fi

source_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
source_parent=$(dirname -- "$source_root")
expected_branch=feature/j414s-windows-unified
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
if test "$profile" = wireless; then
    m1n1_root=${NTASI_M1N1_ROOT:-/Users/dj/Developer/m1n1}
    m1n1_verifier=$m1n1_root/tools/j414s-wireless-handoff-manifest.py
    test -f "$m1n1_verifier" || {
        echo "error: authoritative m1n1 wireless manifest verifier is missing" >&2
        exit 1
    }
    python3 "$m1n1_verifier" verify --manifest "$wireless_manifest"
    cp "$wireless_manifest" "$output_dir/wireless-handoff.json"
    wireless_manifest=$output_dir/wireless-handoff.json
else
    wireless_manifest=/dev/null
fi

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
    -e NTASI_WIRELESS_HANDOFF_MANIFEST=/wireless-handoff.json \
    -e NTASI_M2_PRO_MU_REFRESH=never \
    -e NTASI_M2_PRO_MU_RECIPE_IMAGE="$image" \
    -v "$source_root:/work:ro" \
    -v "$build_dir:/work/Build:rw" \
    -v "$conf_dir:/work/Conf:rw" \
    -v "$wireless_manifest:/wireless-handoff.json:ro" \
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
    --image-repo-digests-json "$image_repo_digests_json"

if command -v sha256sum >/dev/null 2>&1; then
    fd_sha=$(sha256sum "$artifact" | awk '{print $1}')
    manifest_sha=$(sha256sum "$manifest" | awk '{print $1}')
else
    fd_sha=$(shasum -a 256 "$artifact" | awk '{print $1}')
    manifest_sha=$(shasum -a 256 "$manifest" | awk '{print $1}')
fi

echo "READY_TO_TEST $profile"
echo "source=$commit"
echo "fd=$artifact"
echo "sha256=$fd_sha"
echo "manifest=$manifest"
echo "manifest_sha256=$manifest_sha"
