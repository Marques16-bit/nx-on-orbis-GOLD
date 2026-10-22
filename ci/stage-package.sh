# ELF -> .pkg (and a copy of the ELF in elf/). See package-eden.sh for the layout and why.
set -euo pipefail
cd "$NXO_ROOT"
source tools/env-build.sh
bash package-eden.sh
ls -la out-eden elf
