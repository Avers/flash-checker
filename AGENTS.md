# Agent Rules for flashcheck

## Primary Rule

**ALL changes to this repository require explicit developer approval before committing or pushing.**

No AI agent may commit, push, merge, or modify any file without:
1. Developer reviewing the proposed change
2. Developer explicitly approving the change
3. Agent waiting for approval before proceeding

## When Working on This Repository

### Before Any Change
- Read the full context: `README.md`, `CONTRIBUTING.md`, `Makefile`, relevant source files
- Explain the proposed change to the developer
- Wait for explicit approval ("yes", "approved", "proceed") before making any modification

### Making Changes
- Never push to `main` directly
- Create a branch: `ai/feature-name` or `ai/fix-description`
- Do not commit without approval
- Do not amend or force-push without approval
- Do not run `make install` or modify system files without approval

### Testing
- Run `make clean && make` to verify build
- Run `make test` to verify tests pass
- Report results to the developer
- Do not skip any test or ignore any failure

### What You Cannot Do Without Approval
- ❌ Cannot commit or push
- ❌ Cannot create or delete branches
- ❌ Cannot modify `VERSION`, `CHANGELOG.md`, or `Makefile`
- ❌ Cannot modify safety-critical code (`safety.c`, `device.c`, `io/sync.c`)
- ❌ Cannot add external dependencies
- ❌ Cannot change the build system
- ❌ Cannot modify the safety interlock
- ❌ Cannot push to `main`
- ❌ Cannot create PRs
- ❌ Cannot merge any branch

### What You Can Do Without Approval
- ✅ Read files and analyze code
- ✅ Explain code and architecture
- ✅ Run `make` and `make test` to verify
- ✅ Suggest changes
- ✅ Create branches (after approval)
- ✅ Edit files as long as you don't commit/push

## Communication Protocol

1. **Before every action**: Explain what you're about to do
2. **Before every commit**: Show the diff and wait for approval
3. **Before every push**: Confirm the branch and target
4. **Always report**: Test results, build status, any errors

## Safety

flashcheck writes to raw block devices (`/dev/disk*`). Never:
- Test on a real device without explicit developer approval
- Run `--destructive` mode without approval
- Modify or write to `/dev/` paths

## Summary

> **No commit, no push, no merge without developer approval.**
> **When in doubt, ask.**
