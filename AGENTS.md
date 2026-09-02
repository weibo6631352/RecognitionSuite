# RecognitionSuite development contract

- This is one monorepo. `apps/RecognitionStudio`, `engines/ScanEngine`, and
  `engines/VoiceEngine` are versioned atomically in the root Git repository.
- Build the two engine SDKs before building `RecognitionStudio`. The desktop
  application may consume only the published SDKs; do not copy engine source
  code into the application repository.
- Keep acceptance inputs under `testdata/` and requirements/release documents
  under `docs/`. Generated results and assembled deliverables belong under the
  ignored `artifacts/` directory.
- Do not commit child build directories or generated SDK/package contents.
- Keep component-specific code in its owning app or engine directory. Shared
  requirements, validation data, and release automation belong at the root.
- Local commits use concise English imperative messages. Push only when the
  user explicitly requests it.
