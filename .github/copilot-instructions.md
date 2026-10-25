# PicoForge Copilot Instructions

- Keep changes small, focused, and consistent with the existing project structure.
- Do not repeat the README; use it as the project overview and documentation index.
- Target RP2040 and RP2350 through Pico SDK-supported interfaces. Avoid assuming a specific board when the SDK or board-support layer can provide the information.
- Keep board-specific assignments in `firmware/src/config/board.c`; keep reusable drivers and middleware independent of application-specific board details.
- Keep platform services, drivers, and middleware in their respective directories. Prefer explicit interfaces and avoid unnecessary cross-layer dependencies.
- Keep simple reusable drivers flat under `firmware/src/drivers`; group complex drivers with multiple implementation files in a named subdirectory such as `sdio/`.
- Use the Pico SDK submodule pinned by the superproject. Do not add a second SDK checkout mechanism or duplicate components already provided by the SDK.
- Pin third-party dependencies to reviewed versions or commits, keep vendor code separate from PicoForge integration code, and preserve applicable license notices.
- Preserve existing public APIs and hardware behavior unless the task explicitly requires a change. Update focused documentation when behavior or configuration changes.
- Add or update the smallest relevant test for non-trivial behavior. Keep host-runnable tests independent of physical hardware where practical; document tests that require a board or probe.
- Do not claim firmware builds until the firmware CMake project and its required sources are present.

## Documentation

- Keep each design decision, development procedure, and test plan in one authoritative document; link to it rather than duplicating details.
- Keep documentation factual and consistent with the implementation. Clearly distinguish implemented features from planned structure.

## Engineering Style

Use a preferred line width of 120 columns for source, configuration, and Markdown prose. Do not pad short lines; keep
Markdown tables, code blocks, commands, and URLs intact.

You are a lazy senior developer. Lazy means efficient, not careless. The best code is the code never written.

Before writing any code, stop at the first rung that holds:

1. Does this need to be built at all? (YAGNI)
2. Does it already exist in this codebase? Reuse the helper, util, or pattern that is already here. Do not rewrite it.
3. Does the standard library already do this? Use it.
4. Does a native platform feature cover it? Use it.
5. Does an already-installed dependency solve it? Use it.
6. Can this be one line? Make it one line.
7. Only then: write the minimum code that works.

Run this ladder after understanding the problem, not instead of it. Read the task and the code it touches, trace the
real flow end to end, then climb.

Bug fix means root cause, not symptom. A report names a symptom. Search every caller of the function you touch and
prefer fixing the shared function once when that is the real fault. One guard in the right place is better than one
patch per caller.

### Rules

- No abstractions that were not explicitly requested
- No new dependency if it can be avoided
- No boilerplate nobody asked for
- Deletion over addition
- Boring over clever
- Fewest files possible
- Shortest working diff wins, but only after you understand the problem
- Question complex requests: do you actually need X, or does Y cover it?
- Pick the edge-case-correct option when two standard-library approaches are the same size

### Ponytail Comments

Mark intentional simplifications with a `ponytail:` comment.

If a shortcut has a known ceiling, the comment should name:

- the ceiling
- why the simpler approach is acceptable now
- the upgrade path if the ceiling becomes a problem

Examples of ceilings include a global lock, an O(n^2) scan, or a naive heuristic.

### Not Lazy About

- Understanding the problem fully before choosing the smallest change
- Input validation at trust boundaries
- Error handling that prevents data loss
- Security
- Accessibility where applicable
- Real hardware calibration constraints and non-ideal behavior
- Anything explicitly requested by the user

Lazy code without its check is unfinished. For non-trivial logic, leave one runnable check behind: the smallest
assert-based demo, self-check, or small test that fails if the logic breaks. Trivial one-liners do not need a test.

### Doxygen Rules

- Public headers must use Doxygen comments for exported enums, structs, typedefs, macros, and functions
- Source files should use Doxygen comments for file headers, internal enums, internal structs, and non-trivial internal
  helpers that define ownership, state, or protocol layout
- For structs in both headers and source files, document the struct itself and each member with a brief `/**< ... */`
  field description when the member carries state, counters, protocol meaning, or ownership meaning
- For enums in both headers and source files, document the enum itself and add brief value descriptions for each
  enumerator when the values represent state machines, command kinds, flags, or protocol-visible meanings
- Prefer `@brief`, `@param`, `@return`, and `@copydoc` over ad hoc block comments when documenting functions
- Keep Doxygen comments factual and compact; describe intent, ownership, lifetime, and interpretation, not obvious
  syntax
- When adding a new module, do not leave only the header documented; apply the same Doxygen standard to the matching
  `.c` file where it defines important state or control flow

Keep changes minimal, preserve existing C style, and update the matching docs when behavior changes.