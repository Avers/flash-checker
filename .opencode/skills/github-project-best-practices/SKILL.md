---
name: github-project-best-practices
description: Ship a GitHub project like a release engineer. Use when cutting a release, bumping a version, writing a CHANGELOG, opening a pull request, tagging vX.Y.Z, setting up tag-driven CI releases, or defining branch and approval workflows for a repo.
---

# GitHub Project Best Practices

Distilled from shipping `flashcheck` 0.3.0 end-to-end (feature branch → tests → PR → tag → multi-platform GitHub release). Adapt paths and tool names; keep the gates.

## 1. Guardrails before code

Write repo rules down (`AGENTS.md` / `CONTRIBUTING.md`) and enforce them every turn:

- **No commit, push, merge, or tag without explicit developer approval.** Wait for "yes/approved/proceed".
- **Never push to `main` directly.** All work on `ai/<feature>` branches.
- **Protect sensitive files** (version manifests, changelogs, build system, safety-critical code) — changes need a separate explicit OK.
- **Destructive actions need explicit approval twice:** once to plan, once with the exact command/target confirmed (device paths, `--destructive` flags, tag pushes).

## 2. One branch, one purpose

- Cut feature branches from `main`: `git checkout -b ai/<name> main`.
- If a branch accumulates unrelated commits (docs + feature), **don't ship it as one PR**. Cut a clean branch from `main` and `git cherry-pick` only the release commits onto it.
- Keep the diff reviewable: `git log --oneline main..HEAD` and `git diff --stat main...HEAD` must tell a single story.

## 3. Test battery — run all of it, every time

Never declare "done" on a subset. For a Makefile C project the full battery is:

```sh
make clean && make        # release build, -Werror clean
make test                 # unit tests + CLI smoke (exit-code assertions)
make asan                 # full suite under AddressSanitizer + UBSanitizer
make debug                # sanitizer build compiles clean
make lint                 # clang-tidy if present, else -Werror build
```

Plus, where the tool has behaviors worth a matrix, script it:

- **Policy × mode matrix**: every fake/error policy × every flag combination that changes output; assert exit codes (`0` pass, `1` fail, `2` inconclusive, `3` usage, `4` I/O error).
- **Output-mode separation**: if the tool has quiet/verbose modes, byte-verify both directions — default output must contain zero detail-only lines, verbose must contain zero quiet-only artifacts. Check TTY vs piped fallbacks separately (`\r` redraw on TTY, `\n` lines when piped, no ANSI escapes in files).

Re-run the battery after every fix, however small. A one-line string change broke the build once (unterminated C literal) — the battery catches what eyes skip.

## 4. Versioning that survives contact with reality

- `VERSION` file is the **single source of truth** (`0.2.0` format). Compile it in (`-DVERSION_STR`), never hardcode versions in headers.
- **Semver**: PATCH = fixes, MINOR = backwards-compatible features, MAJOR = breaking changes. Bump via `make bump part=patch|minor|major`, which also creates the annotated tag.
- `CHANGELOG.md` (Keep a Changelog) gets an entry with **every** release: `Added` / `Changed` / `Fixed` sections, dated, matching the version.
- Verify the triple before release: `cat VERSION`, `head CHANGELOG.md`, `binary --version`, `--help` text.

## 5. Tag-driven releases

Wire releases to annotated tags (`release.yml` on `push: tags: ["v*"]`):

- Matrix-build every supported platform (here: Linux x86_64/aarch64, macOS Intel/Apple Silicon), run the test binary on each runner, package `binary + LICENSE + README + install.sh` into tarballs with `.sha256` sidecars.
- Generate release notes from the CHANGELOG section matching the tag (`awk` on `## [vX.Y.Z]`), so notes can never drift from the changelog.
- Release with: `git tag -a "vX.Y.Z" -m "Release vX.Y.Z" && git push origin vX.Y.Z`.
- After pushing, watch the runs: a tag push can trigger **duplicate workflow runs** (queued + in-progress). If the release publishes from the first finisher, `gh run cancel <duplicate-id>` the spare so it doesn't fail recreating an existing release.
- Confirm with `gh release view vX.Y.Z`: status published (not draft), all assets present.

## 6. PR hygiene with `gh`

- Before creating: `git fetch origin main`, review `git log main..HEAD` (commits) and the full diff — every commit in the PR gets reviewed, not just the tip.
- Create with a verification section in the body: exact commands run and their results (check counts, smoke status, sanitizer status).
- `gh pr list`, `gh pr view <n> --json state,mergeable,reviewDecision`, and after merge: `git checkout main && git pull`, then clean up merged branches locally and remotely.

## 7. Iterate output in the open, with the user

- When default vs verbose semantics are disputed, **default to the general user** (one live line + short result) and gate engineer detail behind the flag — then byte-verify the split.
- For bikeshed-prone choices (glyphs, colors), render **live samples** in the terminal (a `python3 -c` one-liner printing each candidate in context) and let the user pick from a `question` menu. Decide fallbacks explicitly (TTY color vs piped plain vs non-UTF-8 ASCII).
- Record each decision (flag semantics, glyph set, counter scheme) in the CHANGELOG entry so the release notes explain the UX, not just the code.
