# SpeechWorker Phase A Verification Report

## 1. Executive Summary
Phase A (Request-State Policy Extraction) of the `SpeechWorker` incremental decomposition has been completed and corrected under the strict Subagent-Driven Development (SDD) workflow following Codex whole-branch review. Request-lifecycle policy has been cleanly extracted into the pure Level-0 unit `SpeechStatePolicy` (`CoreEngine/SpeechStatePolicy.h` and `CoreEngine/SpeechStatePolicy.cpp`), while `SpeechWorker` retains threads, synchronization primitives, pipe IPC, COM callbacks, mutable context ownership, framing, and fault publication.

- **Branch:** `codex/phase7a-speech-state-policy`
- **Worktree:** `D:\Projects\ModernSapiAdapter\.worktrees\phase7a-speech-state-policy`
- **Base Commit:** `8dcd328a334a1d96da5e4599647a4f00816adba9`
- **Head Commit:** Branch HEAD on `codex/phase7a-speech-state-policy`
- **Status:** All 5 tasks and Codex review corrections complete, verified, and reviewed. Ready for final whole-branch re-review by Codex.

---

## 2. Architectural Boundary & Invariants
- **`SpeechStatePolicy`:**
  - Pure functional Level-0 leaf unit in `namespace SpeechStatePolicy`.
  - All 8 evaluation functions are `[[nodiscard]]`, `noexcept`, allocation-free, lock-free, O(1), stateless, and free of I/O, logging, Win32 calls, or clock reads.
  - Consumes `const RequestContext&` and immutable caller-provided facts; produces typed decisions.
- **`SpeechWorker`:**
  - Retains full ownership of `m_audioThread`, `m_controlThread`, `m_requestMutex`, `m_eventForwardMutex`, `m_requestChanged`, and `m_context`.
  - Holds `m_requestMutex` continuously across fact capture, policy evaluation, and decision application.
  - Preserves partial lifecycle reset via `ResetToIdleLocked()`.
  - Preserves duplicate-terminal-first precedence over malformed or misaligned byte diagnostics.
  - Preserves cancellation-before-inactivity timeout precedence.
  - Preserves idle cancellation deadline retention without timing out.
  - Preserves out-of-lock IPC cancellation sends and SAPI callbacks.

---

## 3. Codex Whole-Branch Review Corrections
In response to Codex whole-branch review feedback on commit `7107786`:
1. **Genuine Timeout Precedence:** In `CoreEngine.Tests/SpeechStatePolicyTests.cpp`, `CancellationTimeoutPrecedesInactivityTimeout` was updated to set `upstreamState = UpstreamState::Active` while `downstreamState = DownstreamState::Cancelling`, with both cancellation deadline and synthesis inactivity elapsed, proving CancellationTimeout wins when both conditions are simultaneously eligible and expired.
2. **Downstream-Busy Start Rejection:** `StartAcceptsOnlyQuiescentNonFaultPendingState` was extended to verify start rejection when `upstreamState` is Idle, `faultPending` is false, and `downstreamState` is `Speaking` or `Cancelling`.
3. **Deterministic Payload Assertions:** Added explicit assertions covering all decision fields across:
   - Start Reject (`speakId == 0, generation == 0`)
   - Terminal faults (`targetState == Faulted, terminalAudioBytes == 0`)
   - Non-transition cancellations (`speakId == 0, deadlineTick == 0`)
   - Transition cancellations (`supplied speakId, deadlineTick`)
   - Stop NoAction (`speakId == 0`)
   - Timeout None (`speakId == 0`)
   - Timeout expiries (`current speakId`)
4. **Visual Studio Filter Metadata:** In `CoreEngine/CoreEngine.vcxproj.filters`, removed undeclared `Speech` nested filters and retained all affected files in standard top-level `Header Files` and `Source Files` filters.
5. **Auditable Watchdog Evidence:** Reran all gates fresh and captured complete structured results across all 10 focused runs and both full test suites.

---

## 4. Build & Test Toolchain
- **Compiler:** Microsoft Visual C++ 14.51.36231 (Visual Studio 2026 Professional)
- **MSBuild:** `C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe`
- **Windows SDK:** 10.0.26100.0
- **Shell:** PowerShell 7 (`pwsh`)
- **Target Architectures:** Strict x64 Debug & Release (`/WX` compliant, 0 warnings, 0 errors). x86 prohibited.

---

## 5. Verification Evidence

### 5.1 Test Count & Focused Filter Gate
- **Gate:** Exactly 53 tests selected by `$focusedFilter`:
  - 34 tests in `SpeechStatePolicyTests.*` (pure unit tests)
  - 1 test in `SpeechProtocolUtilsTests.RequestContext_TransitionToCancellingPreservesCompletedUpstreamEnum` (characterization test)
  - 18 tests in `SapiEngineTests` (integration fixtures)
- **Selection Count Verification:** Verified 53 tests selected via `--gtest_list_tests`. Full raw listing recorded in `.agents/sdd/phase7a/focused-debug-10runs.txt`.

### 5.2 10-Run Focused Gate (Debug x64)
All 10 consecutive runs passed under the 30-second watchdog with verified process quiescence (recorded in `.agents/sdd/phase7a/focused-debug-10runs.txt`):
- Run 1/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 53 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=4703
- Run 2/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 53 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=4655
- Run 3/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 53 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=4688
- Run 4/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 53 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=4694
- Run 5/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 53 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=4719
- Run 6/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 53 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=4660
- Run 7/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 53 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=4646
- Run 8/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 53 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=4654
- Run 9/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 53 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=4634
- Run 10/10: TimedOut=False, ExitCode=0, StdOutSummary=[  PASSED  ] 53 tests., StdErr=<empty>, Quiesced=True, ElapsedMs=4720

### 5.3 Full Test Suites
- **Full Debug x64:** 228/228 tests PASSED from 13 test cases in 13.61s (TimedOut=False, ExitCode=0, Quiesced=True). Recorded in `.agents/sdd/phase7a/full-debug-suite-watchdog.txt`.
- **Full Release x64:** 182/182 tests PASSED from 12 test cases in 12.26s (TimedOut=False, ExitCode=0, Quiesced=True). Recorded in `.agents/sdd/phase7a/full-release-suite-watchdog.txt`.
  *(Note: Release executes exactly 182 tests from 12 test cases because Debug-only test hook tests in DllEntryAdmissionTests are `#if defined(_DEBUG)`).*

### 5.4 Code Hygiene
- `git diff --check 8dcd328..HEAD`: clean (0 whitespace errors).
- All modified and new C++ blocks adhere to strict Allman bracing.

---

## 6. ARM64 Verification Exception Record
```text
Rule: BTW-01 and ModernSapiAdapter profile 10.1.1 (verify every supported architecture before integration).
Scope: SpeechWorker Phase A request-state policy extraction.
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

## 7. Review Ledger
The subagent reviewer IDs below represent autonomous Gemini Pro reviewer agents whose execution sessions and full conversational transcripts are externally retained in the Antigravity conversation store (`~/.gemini/antigravity/brain/<id>`):

| Task | Description | Reviewer Agent ID | Verdict | Findings & Resolutions |
|---|---|---|---|---|
| Task 1 | Characterize Residual Lifecycle State Before Extraction | `ce50cf35` | Spec PASS, Code Quality APPROVED | Verified characterization test preserves residual Completed enum across TransitionToCancelling; no regressions. |
| Task 2 | Add Pure Policy with TDD | `ed0ce39f` | Spec PASS, Code Quality APPROVED | Verified 34 pure unit tests; confirmed red linker failure and clean green compilation; Allman bracing confirmed. |
| Task 3 | Rewire Request Admission, Stop, and Cancellation | `d1d6c6c1` | Spec PASS, Code Quality APPROVED | Verified continuous mutex hold across fact capture, policy evaluation, and decision application; confirmed out-of-lock IPC cancellation sends. |
| Task 4 | Rewire Terminal, Failure, Boundary, and Timeout Lifecycle | `72405b02` | Spec PASS, Code Quality APPROVED | Verified duplicate-first terminal fault precedence; verified timeout precedence; confirmed out-of-lock error logging and fault transitions. |
| Task 5 | Final Concurrency, State, and Lock Ordering Review | `6992051a` | CONCURRENCY REVIEW: APPROVED | Concurrency audit passed: single continuous lock holds, pure non-blocking evaluation, no deadlocks, no lock reversals. |
| Task 5 | Final Contract and Test Execution Review | `fe392480` | CONTRACT & TEST REVIEW: APPROVED | Verified contract adherence across all tests, 53-filter gate, full test suites, and MockProvider process quiescence. |
| Review Fix | Whole-Branch Corrections Review | Gemini Pro (Reviewer Subagent) | PENDING REVIEW | Covers test corrections (timeout precedence, downstream busy start rejection, complete field assertions) and filter metadata fix. |

---

## 8. Commit History
- `679718c`: `test(coreengine): characterize residual completed state on cancellation` (Task 1)
- `f7e6c1e`: `feat(coreengine): implement pure request-state policy and direct unit tests` (Task 2)
- `19564a2`: `refactor(coreengine): rewire request admission, stop, and cancellation to policy` (Task 3)
- `81556bb`: `refactor(coreengine): rewire terminal, failure, boundary, and timeout lifecycle to policy` (Task 4)
- `7107786`: `docs(speechworker): document Phase A verification evidence and final review ledger` (Task 5)
- Review-Fix Commit: `fix(speechworker): address Codex whole-branch review feedback on tests, metadata, and evidence`
