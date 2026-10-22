# Copies the presentation-related sources of Eden (after the patches) into eden-sources/, so the
# exact code the colour fix has to touch can be read. Run by the "Check patches" workflow, which
# uploads the folder as the "eden-sources" artifact.
set -euo pipefail
ROOT="${NXO_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$ROOT/deps/eden"
out="$ROOT/eden-sources"
rm -rf "$out"; mkdir -p "$out"
{
    find src/video_core -type f \( -iname '*swapchain*' -o -iname '*present*' -o -iname '*blit_screen*' \
         -o -iname '*window_adapt*' -o -iname '*surface*' \) 2>/dev/null
    find src/video_core/renderer_vulkan -maxdepth 1 -type f -name 'renderer_vulkan.*' 2>/dev/null
    find src/video_core/renderer_vulkan/present -type f 2>/dev/null
    find src/video_core/host_shaders -type f \( -iname '*present*' -o -iname '*blit*' \) 2>/dev/null
    [ -f src/common/settings.h ] && echo src/common/settings.h
} | sort -u | while IFS= read -r f; do cp --parents "$f" "$out/"; done
git log --oneline | head -n 40 > "$out/PATCH-STACK.txt"
echo "collected $(find "$out" -type f | wc -l) files, $(du -sk "$out" | cut -f1) KiB:"
(cd "$out" && find . -type f | sort)
