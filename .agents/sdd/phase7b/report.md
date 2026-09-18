# SpeechWorker Phase B Verification Report

## 1. Executive Summary
Phase B (Control-Event Policy Extraction) of the `SpeechWorker` incremental decomposition has been implemented and thoroughly verified under the Subagent-Driven Development (SDD) and Test-Driven Development (TDD) workflow. Control-event classification and final admission policy have been cleanly extracted into the pure Level-0 unit `ControlEventPolicy` (`CoreEngine/ControlEventPolicy.h` and `CoreEngine/ControlEventPolicy.cpp`), while `SpeechWorker` retains all threads, synchronization primitives, pipe IPC, COM callbacks, mutable context ownership, framing, diagnostics, and fault publication.

- **Branch:** `codex/phase7b-control-event-policy-plan`
- **Worktree:** `D:\Projects\ModernSapiAdapter\.worktrees\phase7b-control-event-policy-plan`
- **Base Commit:** `fa25d83c3830cb30af66904fa3bb8f5d7baf6221`
- **Head Commit:** Branch HEAD on `codex/phase7b-control-event-policy-plan`
- **Status:** All 5 tasks complete, verified, and independently reviewed. Ready for final whole-branch review by Codex.

---

## 2. Architectural Boundary & Invariants
- **`ControlEventPolicy`:**
  - Pure functional Level-0 leaf unit in `namespace ControlEventPolicy`.
  - Both decision functions (`EvaluateParsedEvent` and `EvaluateFinalAdmission`) are `[[nodiscard]]`, `noexcept`, allocation-free, lock-free, O(1), stateless, and free of I/O, logging, Win32 calls, or clock reads.
  - `EvaluateParsedEvent` maps `(const ParsedControlEvent&, const RequestContext&)` into a strongly typed `EventDecision` (`EventAction action`, `bool shouldRefreshProgress`, `bool shouldForwardToSapi`, `bool shouldEnterFaultedState`).
  - `EvaluateFinalAdmission` maps `(const ParsedControlEvent&, const RequestContext&)` into a strongly typed `FinalAdmission` (`bool shouldForwardToSapi`).
- **`SpeechWorker`:**
  - Retains full ownership of `m_audioThread`, `m_controlThread`, `m_requestMutex`, `m_eventForwardMutex`, `m_requestChanged`, and `m_context`.
  - Holds `m_requestMutex` continuously across fact capture, policy evaluation, progress tick update, and decision application.
  - Empty unnamed events and `speak_id == 0` checks are preserved in `ControlThreadProc` before locked policy evaluation.
  - Progress clock update (`m_lastProviderProgressTick`) is refreshed conditionally on `decision.shouldRefreshProgress`.
  - `ForwardEventToSapi` staging preserved: `m_faultVisible` and `zero-ID` checks occur under `m_eventForwardMutex`, followed by `EvaluateFinalAdmission` under `m_requestMutex`. Both locks are strictly dropped before dispatching `OnSpeechEvent`.
  - Stale events maintain healthy provisional admission (`p0 == true`) for debug-pause visibility, but are safely rejected at final admission.
  - Fatal log forwarding before fault publication is strictly preserved.
  - Obsolete `ShouldForwardEventLocked` helper excised from `SpeechWorker.h` and `SpeechWorker.cpp`.

---

## 3. Build & Test Toolchain
- **Compiler:** Microsoft Visual C++ 14.51.36231 (Visual Studio 2026 Professional)
- **MSBuild:** `C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe`
- **Windows SDK:** 10.0.26100.0
- **Shell:** PowerShell 7 (`pwsh`)
- **Target Architectures:** Strict x64 Debug & Release (`/WX` compliant, 0 warnings, 0 errors). x86 prohibited.

---

## 4. Verification Evidence

### 4.1 Test Count & Focused Filter Gate
- **Gate Filter:** `$focusedFilter = "ControlEventPolicyTests.*:" + $baselineFocusedFilter + ":SapiEngineTests.PunctuationBoundaryWithActiveSpeakIdIsIgnoredWithoutFault"`
- **Debug Count:** Exactly 81 tests selected via `--gtest_list_tests` (20 pure policy, 1 characterization, 60 focused integration). Full raw listing in `.agents/sdd/phase7b/focused-debug-10runs.txt`.
- **Release Count:** Exactly 65 tests selected via `--gtest_list_tests` (20 pure policy, 1 characterization, 44 focused integration; 16 debug-only tests excluded via `#if defined(_DEBUG)`). Full raw listing in `.agents/sdd/phase7b/focused-release-1run.txt`.

### 4.2 10-Run Focused Gate (Debug x64)
All 10 consecutive runs passed under the 30-second fail-closed watchdog with verified process quiescence (recorded in `.agents/sdd/phase7b/focused-debug-10runs.txt`):
- Run 1/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 81 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=3888
- Run 2/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 81 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=4164
- Run 3/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 81 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=3899
- Run 4/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 81 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=3890
- Run 5/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 81 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=4215
- Run 6/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 81 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=3907
- Run 7/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 81 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=4142
- Run 8/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 81 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=4162
- Run 9/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 81 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=3907
- Run 10/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 81 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=3891

### 4.3 Release Focused Gate (Release x64)
Single watchdog execution passed with verified quiescence (recorded in `.agents/sdd/phase7b/focused-release-1run.txt`):
- Run 1/1: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 65 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=3765

### 4.4 Full Test Suites
- **Full Debug x64:** 249/249 tests PASSED from 14 test cases in 13.30s (TimedOut=False, ExitCode=0, Quiesced=True). Recorded in `.agents/sdd/phase7b/full-debug-suite-watchdog.txt`.
- **Full Release x64:** 203/203 tests PASSED from 13 test cases in 11.84s (TimedOut=False, ExitCode=0, Quiesced=True). Recorded in `.agents/sdd/phase7b/full-release-suite-watchdog.txt`.
  *(Note: Release executes exactly 203 tests from 13 test cases because Debug-only test hook tests in DllEntryAdmissionTests are `#if defined(_DEBUG)`).*

### 4.5 Code Hygiene
- `git diff --check fa25d83..HEAD`: clean (0 whitespace errors, recorded in `.agents/sdd/phase7b/git-diff-check.txt`).
- All modified and new C++ blocks adhere to strict Allman bracing.

---

## 5. ARM64 Verification Exception Record
```text
Rule: BTW-01 and ModernSapiAdapter profile 10.1.1 (verify every supported architecture before integration).
Scope: SpeechWorker Phase B control-event policy extraction.
Risk tier: Tier 3.
Reason for exception: The available environment does not contain the ARM64 dependency libraries/toolchain required to link CoreEngine.Tests; the repository owner explicitly directed agents not to attempt ARM64 builds in this environment.
Risk introduced: A compile or linkage regression specific to ARM64 could remain undetected locally.
Compensating safeguards: The extracted policy uses fixed-width standard integer types, no architecture intrinsics, no packing change, no ABI export, and no new dependency. Debug and Release x64 builds and complete test suites are mandatory. No ARM64 artifact is shipped from this evidence.
Evidence reviewed: Existing project support matrix, unavailable ARM64 library state, x64 baseline, and the repository owner's prior explicit instruction.
Approver: Andres Godoy, repository owner.
Approval date: 2026-09-16.
Status: Active for this phase only.
Expiry or follow-up condition: Re-run the targeted CoreEngine and CoreEngine.Tests ARM64 build before producing or claiming an ARM64 release, or when the missing ARM64 dependencies become available.
```

---

## 6. Review Ledger
The subagent reviewer IDs below represent autonomous Gemini Pro reviewer agents whose execution sessions and full conversational transcripts are externally retained in the Antigravity conversation store (`~/.gemini/antigravity/brain/<id>`):

| Task | Description | Reviewer Agent ID | Verdict | Findings & Resolutions |
|---|---|---|---|---|
| Task 1 | Characterize Ignored Punctuation Through Real Pipe | `e3ea1e02` | Spec PASS, Code Quality APPROVED | Characterized punctuation boundary parsing as Unknown, verified no fault and silent drop through real pipe. |
| Task 2 | Add Pure ControlEventPolicy with TDD | `a4959dbb` | Spec PASS, Code Quality APPROVED | 20 pure unit tests in ControlEventPolicyTests; confirmed red linker failure and clean green compilation; Allman bracing confirmed. |
| Task 3 | Rewire Locked Classification to Policy | `51c51d7e` | Spec PASS, Code Quality APPROVED | Verified locked classification delegates to EvaluateParsedEvent under continuous mutex hold; progress clock refreshed conditionally. |
| Task 4 | Rewire Final Callback Admission to Policy | `bf873c51` | Spec PASS, Code Quality APPROVED | Excised ShouldForwardEventLocked; EvaluateFinalAdmission invoked under lock staging; OnSpeechEvent called outside all locks. |
| Task 5 | Phase 7B Whole-Task Final Review | `00c2c83e` | Spec PASS, Architecture & Ordering APPROVED | Confirmed behavior preservation, lock staging, callback ordering outside locks, fatal-log priority before fault publication, test coverage, and code hygiene. 0 findings. |

---

## 7. Commit History
- `ac08700`: `test(coreengine): characterize ignored punctuation through real control pipe` (Task 1)
- `3198d38`: `feat(coreengine): add pure control-event policy and direct unit tests` (Task 2)
- `9163f1e`: `refactor(coreengine): rewire locked control-event classification to policy` (Task 3)
- `c128c4d`: `refactor(coreengine): rewire final callback admission to policy` (Task 4)
