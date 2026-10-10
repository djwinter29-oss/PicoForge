# PicoForge Copilot Instructions

- Keep changes small, focused, and consistent with the existing project structure.
- Do not repeat the README; use it as the project overview and documentation index.
- Target RP2040 and RP2350 through Pico SDK-supported interfaces. Avoid assuming a specific board when the SDK or board-support layer can provide the information.
- Keep board-specific behavior in `firmware/src/platform/board`; keep reusable drivers and middleware independent of application-specific board details.
- Keep platform services, drivers, and middleware in their respective directories. Prefer explicit interfaces and avoid unnecessary cross-layer dependencies.
- Use the checked-in SDK setup script and its pinned Pico SDK revision. Do not add a second SDK checkout mechanism or duplicate components already provided by the SDK.
- Pin third-party dependencies to reviewed versions or commits, keep vendor code separate from PicoForge integration code, and preserve applicable license notices.
- Preserve existing public APIs and hardware behavior unless the task explicitly requires a change. Update focused documentation when behavior or configuration changes.
- Add or update the smallest relevant test for non-trivial behavior. Keep host-runnable tests independent of physical hardware where practical; document tests that require a board or probe.
- Do not claim firmware builds until the firmware CMake project and its required sources are present.

## Documentation

- Keep each design decision, development procedure, and test plan in one authoritative document; link to it rather than duplicating details.
- Keep documentation factual and consistent with the implementation. Clearly distinguish implemented features from planned structure.

## Engineering Style

- Prefer simple, explicit code and the smallest correct change. Fix root causes rather than adding caller-specific workarounds.
- Avoid new dependencies and abstractions unless they solve a demonstrated need.
- Validate inputs at trust boundaries and handle failures without hiding errors or risking data loss.
- Document public C interfaces with concise Doxygen comments, including ownership, lifetime, and non-obvious behavior where relevant.
- Run the narrowest useful validation for changed code. For shell scripts, at minimum run `sh -n`; do not imply hardware validation from host-only checks.
