# paint.software — AI-Assisted Development Instructions

## Project vision
The purpose of this development effort is to make paint.software a faithful, native Linux equivalent of Paint.NET.

Paint.NET is the primary reference for application behaviour, interaction design, workflow, and feature expectations.

The objective is not merely to reproduce Paint.NET's feature list. The application should feel familiar to an experienced Paint.NET user, allowing them to perform equivalent tasks using substantially the same workflows.

## Design principles
- Prioritise behavioural and workflow fidelity to Paint.NET.
- Reproduce established Paint.NET interactions where practical, rather than inventing alternative UX solutions.
- Use the main editing canvas for previews when Paint.NET does so.
- Prefer simple, responsive, predictable user interactions.
- Preserve the existing application's architecture and conventions where appropriate.
- Prefer reusable solutions where multiple features have similar requirements.
- Maintain compatibility with the native Linux/Qt environment.
- Do not copy proprietary Paint.NET source code, artwork, or other protected assets.

## Decision making
- Before proposing an implementation, consider how the corresponding feature works in Paint.NET.
- If the reference behaviour is uncertain, state the uncertainty and request clarification or reference material.
- Explain any proposed deviation from Paint.NET behaviour and obtain approval.
- Treat suggestions as proposals, not authorisation to implement them.

## Development workflow
- First inspect the relevant existing code and architecture.
- For substantive changes, propose an implementation plan before modifying code.
- Wait for explicit approval before implementing.
- Once approved, implement the feature, build the application, and run applicable tests.
- Preserve existing functionality and avoid unrelated changes.
- Report modified files, test results, known limitations, and manual verification steps.
- Do not commit, push, or open pull requests without explicit approval.

## Acceptance criteria
- The feature behaves as closely as practical to its Paint.NET equivalent.
- Existing functionality is preserved.
- Cancel, undo/redo, selection, transparency, and layer behaviour remain correct where applicable.
- Changes are tested where practical.
- Visual and interactive behaviour requiring human verification is identified explicitly.

## Upstream contribution requirements
Source: [upstream CONTRIBUTING.md](https://github.com/Univers4craft/paint.software/blob/main/CONTRIBUTING.md), reviewed 20 October 2026. Recheck it when preparing an upstream contribution for any changed requirements.

- Work on a topic branch in the contributor's fork. Keep each pull request limited to one topic.
- Follow the surrounding C++17 style and Qt6 idioms.
- For bug fixes and added behaviour, add or update coverage in `tests/test_all.cpp`. Additional focused test executables may supplement this coverage.
- Layer storage uses `QImage::Format_ARGB32_Premultiplied`. Convert to straight alpha before raw colour-channel calculations, then restore premultiplied storage. Preserve alpha and the established premultiplied selection/compositing calculations.
- Before committing, build the application and run the upstream-required functional tests:

  ```bash
  cmake -B build
  cmake --build build -j
  cmake --build build --target paintdotnet_tests
  QT_QPA_PLATFORM=offscreen ./build/paintdotnet_tests
  ```

  Run additional tests relevant to the change as well; an equivalent separate build directory is acceptable.
- When authorised, use a clear commit message, push to the contributor's fork, and target `Univers4craft/paint.software:main` with the pull request. Explain what changed and why.
- Check GitHub Actions build/test results. Upstream merge requires maintainer review and approval; only the maintainer merges into `main`.
- An Issue is optional for discussing a bug or idea before implementation. Do not create Issues or other external communications without explicit user authorisation.

These requirements do not authorise commits, pushes, or pull requests; the explicit approval rule above still applies.

## Human role
The human directs product vision, priorities, and acceptance decisions. The AI agent is responsible for proposing technical solutions and performing the approved implementation work. The human is not expected to write application code.
