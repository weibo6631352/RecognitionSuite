# RecognitionSuite development contract

- This is one monorepo. `apps/RecognitionStudio`, `engines/ScanEngine`, and
  `engines/VoiceEngine` are versioned atomically in the root Git repository.
- Keep the validated, published engine SDK baselines under
  `sdk/<Engine>/windows-x64/` and commit them through Git LFS. Normal
  `RecognitionStudio` work consumes these baselines and must not rebuild or
  copy engine source code into the application.
- Rebuild the engines only when explicitly refreshing a published SDK. Stage
  any engine source changes first, generate SDKs under each engine's `build/`
  directory, validate them, and then update the root `sdk/` baseline through
  the release automation.
- Keep acceptance inputs under `testdata/` and requirements/release documents
  under `docs/`. Generated results and assembled deliverables belong under the
  ignored `artifacts/` directory.
- Do not commit child build directories or assembled application packages.
  The only committed binary deliverables are the reviewed SDK baselines under
  the root `sdk/` directory.
- Keep component-specific code in its owning app or engine directory. Shared
  requirements, validation data, and release automation belong at the root.
- Keep `scripts/` top-level files limited to human-facing one-click actions.
  Put implementation helpers under `scripts/internal/` and do not document
  them as direct entry points.
- Local commits use concise English imperative messages. Push only when the
  user explicitly requests it.
