---
name: 4d-cli-component
description: WHEN TO USE - create or adapt a real 4D component wrapping a Go, Rust or C++ CLI from this template; initialize an exact destination repository; change a feature class or namespace; replace Greeting with a feature API; diagnose SystemWorker quoting and errors; compile locally with free pinned tool4d; run source and compiled host tests; or package and assess release readiness.
---

# 4D CLI component adoption and acceptance

Read the repository root `AGENTS.md` first: it is the canonical detailed
THINK/ASK → RUN playbook, including stop rules and the dated UUID case study.
Then read `README.md`, `template.json`, class documentation and relevant
scripts/tests. Resolve all paths below from the repository root, not this folder.

1. **THINK / ASK:** establish exact case-sensitive GitHub identity, one
   user-selected language, independent unique namespace, CLI basename, feature
   API/protocol/error/limits, licenses and acceptance scope. Never initialize the
   original template repository or guess material missing decisions.
2. **Inspect:** `python3 scripts/doctor.py --root .` reports configuration,
   platform/tools; optional `--tool4d PATH` checks the supplied tool. Do not
   interpret this read-only report as feature/runtime acceptance.
3. **Initialize the destination only:** use the initializer flags documented
   in AGENTS.md. It records the feature description and selects/renames a starter;
   it does not implement the feature.
4. **Replace the whole feature contract:** API class, CLI, docs, language/Python
   tests, `tests/cli-contract.json` native smoke vectors, host fixture and `tests/host/contract.json`
   feature/report expectations, metadata/notices and compiler completion metadata.
   Preserve the registry's separate generic execution checks. Optional
   `python3 scripts/rename_feature.py --root . --class NAME` is a bounded Greeting
   identity rewrite, not automatic feature implementation.
5. **Preserve real 4D execution:** keep `_CLI`/`_Process` generic, quote into
   SystemWorker command-line Text, guard scalar values before object-instance
   checks, use strict `Position(Char(0); argument; *)`, close stdin and preserve
   separate stdout/stderr/exitCode/system errors. Test native argument round trips.
6. **Validate:** follow AGENTS.md's Python/icon/native CLI/source-host/real
   compiler/pack/unpack/compiled-host sequence. Optional
   `python3 scripts/accept_local.py --root . --repository OWNER/NAME --tool4d PATH`
   orchestrates the existing stages and records `build/acceptance.json`.
   `--cross` is optional **Go-only artifact production**, not native foreign
   execution; available Rosetta CLI tests are separate translated evidence.
   Rust/C++ cross mode is unsupported. Packed output is `dist/<repository>.zip`
   only when required artifact/package checks succeed.
   Without `--cross`, all three current artifacts/manifests are still needed.
   For Rust/C++, restore binaries plus `build-manifest.json` from same-commit
   native CI into `build/<target>/`; never mix stale or changed inputs/outputs.
7. **Report:** identify exact revision/platform/tools, commands, artifact/evidence
   paths, `passed` / `failed` / `not_run` per check and blockers. Partial local success
   is **NOT release ready**. Do not publish or add credentials without approval.
   Unsigned free tool4d local compilation needs no license/signing secrets.
   For this destination, use the signed release contract below, not the
   template's historical unsigned-release assumptions. Registering secrets is
   not execution evidence.

## spooler-monitor signed release contract

Release only through **Actions → Publish → Run workflow** (`publish.yml`,
single `mode: patch | minor | major` input). It bumps from the latest `X.Y.Z`
tag (legacy `v` read) via `scripts/version.py`, commits the version to `main`,
reuses `ci.yml` for native builds, compiles, signs/notarizes with
`scripts/sign_release.py`, then creates bare tag/release `X.Y.Z`. Do not run
Publish, create tags or merge without user permission.

Six registered Actions secrets supply the base64 Developer ID Application P12
including private key, its password, temporary keychain password and notary
Apple ID/app-specific password/team: `APPLE_DEVELOPER_ID_CERTIFICATE`,
`APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD`, `KEYCHAIN_PASSWORD`,
`NOTARYTOOL_APPLE_ID`, `NOTARYTOOL_PASSWORD`, `NOTARYTOOL_TEAM_ID`.
No `CODESIGN_APPLE_ID` is required: derive exactly one imported valid Developer
ID Application identity and verify its team. Delete the isolated temporary
keychain on every exit, including failure; do not require local credentials.

Require native same-ref matrix → validated assembly → real source host →
compile with strict digest marker → unsigned intermediate package → separate
staged compiled `.4dbase`. Sign only the universal macOS CLI and generated
`Libraries` Mach-O dylib with hardened runtime, secure timestamp and no extra
entitlements. Preserve `.4DZ`, Windows and all non-code resource bytes in the
deterministic final `dist/spooler-monitor.zip`; never sign arbitrary resources.
Leave source/build manifests and compile markers unchanged as pre-sign
evidence; record signed hashes separately, never rewrite markers.

Submit the final ZIP to `notarytool`, require **Accepted**, and strictly verify
signature/team/timestamp/runtime for every architecture, separately for CLI
and dylib. Online Gatekeeper CLI assessment requires actual tool support;
unsupported or unexecuted checks are `not_run`, not passes. ZIP and bare
CLI/dylib files cannot be stapled: use Apple online ticket lookup, with no
offline stapled guarantee and no arbitrary DMG workaround.

Test the exact final ZIP's unpacked component in a real compiled 4D host after
signing. Bind final signed-byte inventory/report
`dist/spooler-monitor.signing.json` to final ZIP SHA256, original unsigned ZIP
SHA256 and native pre-sign manifests. Current signing/notary/Gatekeeper/final
signed-host evidence is `not_run`, not a claimed successful submission.
Generic unsigned local acceptance remains available without secrets and is
not signed release acceptance. Intel/Windows 4D, live queue mutations and
published dependency installation remain `not_run` without actual evidence.
Follow [AGENTS.md](../../../AGENTS.md) for the full destination-specific rules.

## Discovery is supported, invocation is not guaranteed

Official Copilot documentation supports project skills at
`.github/skills/<lowercase-hyphen-name>/SKILL.md`, with YAML `name` and
`description`; Copilot chooses relevant skills using the prompt/description.
This file does not install a toolchain, run scripts automatically, or guarantee
that every client/version/session sees it. No shell tools are pre-approved here.

On a supported Copilot CLI, start a new session or use `/skills reload`, then
`/skills info 4d-cli-component` to inspect discovery. These are **CLI slash
commands**, not shell commands; do not run them through Bash. Other surfaces
use their documented controls. If unavailable, read this file and AGENTS.md
directly and report skill invocation as unverified.

Conventions verified November 1, 2026:
[Agent skills](https://docs.github.com/en/copilot/concepts/customizations/agent-skills),
[adding skills](https://docs.github.com/en/copilot/how-tos/copilot-on-github/customize-copilot/customize-cloud-agent/add-skills),
[CLI skill setup/discovery](https://docs.github.com/en/copilot/how-tos/copilot-cli/customize-copilot/add-skills).
