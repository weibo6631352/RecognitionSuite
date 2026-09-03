# Published SDK baselines

This directory contains the reviewed Windows x64 SDK baselines consumed by
`apps/RecognitionStudio` during normal development:

- `ScanEngine/windows-x64/`
- `VoiceEngine/windows-x64/`

The expanded SDKs, their manifests, and large LFS-managed payloads are
versioned with the monorepo. Engine-local `build/<preset>/sdk/` directories are
only staging areas used during an explicit SDK refresh.

Refresh both baselines with:

```powershell
git add engines/ScanEngine engines/VoiceEngine  # when engine sources changed
.\scripts\构建.ps1 -Target SDKs
```

The publisher refuses unstaged or untracked engine sources. Each manifest
records both the base commit and the exact staged engine subtree used to build
the baseline.

The VoiceEngine main GGUF is committed as `part1` and `part2` because the
complete 4 GB file exceeds the repository's LFS per-file limit. The SDK ships a
materializer under `tools/`; `RecognitionStudio` runs it while assembling its
ignored application output, so the final runtime contains one verified GGUF
and does not write into the committed SDK directory.

Do not edit files below an engine/platform directory by hand. Refresh from the
engine staging SDK, inspect the resulting Git diff and manifest, and commit the
baseline atomically with any corresponding application change.
