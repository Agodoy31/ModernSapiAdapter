# Task 2 Review Package: Pure Policy with TDD

## Diffstat
```text
 CoreEngine.Tests/CoreEngine.Tests.vcxproj   |   5 +
 CoreEngine.Tests/SpeechStatePolicyTests.cpp | 370 ++++++++++++++++++++++++++++
 CoreEngine/CoreEngine.vcxproj               |   2 +
 CoreEngine/CoreEngine.vcxproj.filters       |  10 +-
 CoreEngine/SpeechStatePolicy.cpp            | 138 +++++++++++
 CoreEngine/SpeechStatePolicy.h              | 150 +++++++++++
 6 files changed, 675 insertions(+), 3 deletions(-)
```

## Summary of Changes
1. **`CoreEngine/SpeechStatePolicy.h`**:
   - Created pure Level-0 policy header in `namespace SpeechStatePolicy`.
   - Defined typed action enums and decision structs: `StartDecision`, `UpstreamTerminalDecision`, `RequestFailureDecision`, `TerminalBoundaryFacts`, `TerminalBoundaryDecision`, `BeginCancellationDecision`, `StopDecision`, and `TimeoutDecision`.
   - Declared 8 pure, `[[nodiscard]]`, `noexcept` evaluation functions consuming `const RequestContext&` and immutable facts.
2. **`CoreEngine.Tests/SpeechStatePolicyTests.cpp`**:
   - Implemented 34 distinct unit tests asserting against all lifecycle transitions, boundary evaluations, timeouts, and immutability invariants.
3. **`CoreEngine/SpeechStatePolicy.cpp`**:
   - Implemented pure functional logic following Section 5 of `2026-09-16-speechworker-phase-a-request-policy-design.md`.
   - Allocation-free, lock-free, O(1), no I/O, no logging, no Win32 clock queries.
4. **Project Registration**:
   - Added `SpeechStatePolicy.h` and `SpeechStatePolicy.cpp` to `CoreEngine.vcxproj` and `CoreEngine.vcxproj.filters`.
   - Added `SpeechStatePolicyTests.cpp` and `..\CoreEngine\SpeechStatePolicy.cpp` to `CoreEngine.Tests.vcxproj`.

## Verification Evidence
- **Red State (Linker LNK2019):** Saved in `.agents/sdd/phase7a/task2-red.txt`. Confirms tests compiled against header declarations and failed at link time as expected.
- **Green State:** Saved in `.agents/sdd/phase7a/task2-green.txt`. 34/34 tests passed in 26 ms.
- **Focused Gate Count:** Exactly 53 tests selected by the focused test filter (`SpeechStatePolicyTests.*` [34] + `SpeechProtocolUtilsTests.RequestContext_TransitionToCancellingPreservesCompletedUpstreamEnum` [1] + 18 `SapiEngineTests` [18] = 53).
- **Code Hygiene:** `git diff --check` emits zero whitespace errors.
- **Compiler Warnings:** 0 warnings, 0 errors in Debug|x64 build (`/WX` compliant).
