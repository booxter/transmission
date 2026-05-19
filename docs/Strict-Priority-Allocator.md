# Strict Priority Allocator

Status: draft

Last updated: 2026-05-15

## Purpose

This document captures the goal and working design direction for a new bandwidth allocator implementation in Transmission.

The document is intended to be a living memory for the work. It should describe the intended behavior, constraints, and implementation direction clearly enough that future refinements do not lose the original motivation.

## Background

Transmission's current allocator gives priorities extra weight during an initial allocation pass, but it does not enforce strict priority once I/O continues through event-driven execution.

Today, priority mainly affects scheduling during the initial pass by putting peers into fallthrough buckets:

- `HIGH` peers participate in `HIGH`, `NORMAL`, and `LOW` passes.
- `NORMAL` peers participate in `NORMAL` and `LOW` passes.
- `LOW` peers participate only in `LOW` passes.

That gives higher-priority peers more chances during the initial pass, but it is still a weighted fairness model, not a strict priority model.

After that pass, peers that still have bandwidth left are re-enabled for event-driven I/O. At that point, priority is no longer strictly enforced. Any ready peer with remaining budget can make progress, regardless of whether higher-priority peers also have pending work.

## Problem Statement

The current design does not provide strict priority enforcement across the whole pulse.

In particular:

- `HIGH` priority work does not fully dominate `NORMAL` or `LOW` work.
- `NORMAL` priority work does not fully dominate `LOW` work.
- Once event-driven I/O begins, priority ordering effectively becomes opportunistic rather than enforced.
- New high-priority work that appears after lower-priority work has already started does not immediately reclaim precedence.

This means the current allocator can approximate priority, but it cannot guarantee it.

## Goal

Introduce a new allocator implementation that enforces strict priority ordering for peer I/O.

The core rule is:

- If any eligible `HIGH` priority work exists, it must be served before any `NORMAL` or `LOW` work.
- If no `HIGH` work exists but eligible `NORMAL` work exists, it must be served before any `LOW` work.
- `LOW` work should run only when neither `HIGH` nor `NORMAL` work is eligible.

Within each priority class, fairness may still be preserved if needed.

Strict priority must apply across the whole pulse, including both work discovered during the pulse and work that becomes runnable while the pulse is in progress.

If new `HIGH` priority work appears while `NORMAL` or `LOW` work is being processed, the system should switch back to serving `HIGH` work before continuing with lower priorities.

## Non-Goals

The following are out of scope for the initial implementation:

- changing the behavior of the existing allocator for users who do not opt into the new allocator
- removing or rewriting the current allocator implementation
- changing user-visible behavior by default
- deciding every future policy detail up front before implementation starts

This work should add a new allocator option, not replace the current one.

## Compatibility Requirements

- The current allocator must remain available.
- The current allocator must remain the default unless explicitly changed later.
- Users who do not select the new allocator must see no behavior change.
- The implementation choice must be made at runtime through a configuration knob.
- Any integration work required in shared code paths should be limited to what is necessary to support selecting between allocator implementations.

## Implementation Discipline

The implementation should land as a logically separated series of commits rather than one large change.

The intent is:

- each commit should have a focused purpose
- each commit message should explain what changed and why
- commit messages should be wrapped to a readable width rather than left as long unbroken lines
- tests should be included with the commit that introduces or changes the tested behavior when practical
- if a change is too intertwined to keep fully separate without breaking the tree, it is acceptable to lump the minimum necessary pieces together

Every commit in the series should build successfully. Where practical, each commit should also pass the relevant tests for the behavior it introduces or changes.

The bar for this work should be production quality:

- do not overcomplicate the design unnecessarily
- do not cut corners on validation or isolation
- keep the legacy path stable
- keep the strict path understandable enough to present upstream later

## Allocator Selection

Allocator selection should use a string configuration knob whose value is the allocator name.

The canonical configuration key should be `bandwidth_allocator`.

Required values:

- unset: use the existing allocator
- `default`: use the existing allocator
- `strict`: use the new strict-priority allocator

Invalid values should log a warning and fall back to `default`.

For the initial implementation, this setting should be configuration-file only. It should not be exposed through RPC.

Changing allocator mode should require a client restart. That is acceptable for this setting.

This is intended to be the narrowest shared runtime interface:

- one configuration value
- one selected allocator or scheduler service used from pulse logic and peer I/O readiness or timer paths
- two implementations behind that selection

The existing allocator remains the baseline implementation behind the unset and `default` modes.

## Behavioral Requirements

The new allocator should satisfy the following requirements:

1. Priority ordering must be strict, not weighted.
2. Work conservation must still hold.
   If no higher-priority work is available, lower-priority work should be allowed to run.
3. Within a priority class, fairness should be maintained when practical.
4. New higher-priority work must be visible to the scheduler quickly enough to preempt lower-priority work within the same pulse.
5. The allocator must continue to respect bandwidth limits and accounting rules.
6. The allocator must not require behavior changes from users who stay on the current mode.
7. The strict scheduler must be time-aware enough to yield in time for the next pulse.

## High-Level Design Direction

### 1. Two allocator implementations

Introduce a new allocator mode alongside the current one.

At a high level:

- current allocator: existing behavior, preserved as-is, selected by unset or `default`
- strict priority allocator: new behavior, selected by `strict`

The existing allocator should only be changed as needed to fit behind a shared selection mechanism.

### 2. Pulse-driven scheduling model

Transmission should keep its existing pulse-based framing.

The strict allocator does not remove the pulse. Instead, the pulse remains the mechanism that:

- refills bandwidth quota
- lets peers generate protocol work
- gives the scheduler a natural time budget

Within a pulse, the strict allocator should process work in strict priority order:

1. serve `HIGH`
2. then serve `NORMAL`
3. then serve `LOW`

The important distinction from the current design is that the strict allocator should treat this as one continuous scheduling policy for the whole pulse, rather than an initial fairness pass followed by uncontrolled event-driven execution.

The current code calls peer protocol maintenance before bandwidth allocation or draining during each pulse. The strict allocator should preserve that ordering initially unless there is a specific reason to change it.

### 3. Unified scheduler drain loop

The strict allocator should use one scheduler-owned drain loop for peer I/O work within each pulse.

That loop should cover both directions of peer I/O:

- readable peer I/O
- writable peer I/O

The loop should be fed by multiple sources of runnable work, including:

- work made runnable by the pulse
- readable socket readiness
- writable socket readiness
- immediate protocol-driven follow-up work generated while processing an item

When a transport callback or timer callback indicates that a peer is writable or readable, the peer should not immediately consume bandwidth by performing I/O directly from the callback path.

Instead:

- the readiness should be converted into scheduler-visible work
- that work should be inserted into a data structure managed by the strict priority allocator
- allocator-owned execution logic should choose what to run next

That execution logic should always prefer the highest priority class that currently has eligible work.

Within a priority class, the scheduler may still use round-robin service.

### 4. Time-aware yielding

The strict scheduler must not monopolize the session thread indefinitely.

It should remain aware of the pulse cadence and stop draining in time for the next pulse to run on schedule.

The goal is:

- keep the existing pulse-based framing intact
- allow strict priority within a pulse
- avoid letting one long drain loop delay the next pulse

This means the strict scheduler should be bounded by time, not only by queue emptiness.

## Event Handling Ownership

Peer I/O readiness and timer callbacks should call into a scheduler abstraction rather than performing I/O policy inline.

This is expected to be the main shared integration point between allocator implementations:

- callbacks report readiness or runnable work to the selected scheduler
- the selected scheduler decides what to do next

Behavior by mode:

- `default`: preserve current behavior, meaning the scheduler may execute I/O immediately in the callback path and otherwise behave as the current allocator does
- `strict`: callbacks must not perform I/O directly; they hand readiness to the strict scheduler, which owns ordering and execution

In `strict` mode, the scheduler should own the event enable/disable lifecycle for scheduled peer I/O work.

That means the scheduler is responsible for:

- accepting readiness notifications from event callbacks
- deciding whether a `peer + direction` item is already queued
- preventing duplicate runnable items from accumulating unnecessarily
- deciding when queued work should execute
- deciding when the underlying event should remain enabled, be temporarily suppressed, or be re-enabled after execution

The purpose of this ownership rule is to prevent peer I/O from bypassing strict priority ordering through direct callback-path execution.

In the current codebase, the relevant callback paths are broader than libevent socket callbacks alone. The strict scheduler needs to account for all of the following current direct-execution entry points:

- TCP read readiness callbacks
- TCP write readiness callbacks
- uTP writable-state callbacks
- uTP read callbacks
- the zero-delay outbound flush timer callback

### 5. Immediate visibility of new work

The strict priority model only works if newly arrived higher-priority work becomes visible to the scheduler loop immediately, rather than waiting for the next pulse.

That means:

- if processing a peer causes new peer work to become runnable
- or if a network event makes new work writable
- or if a protocol event generates more outbound work

then that work should be delivered to the strict priority scheduler quickly enough that the current pulse can react to it.

The intended behavior is preemptive at priority boundaries:

- lower-priority progress may pause
- newly available higher-priority work takes precedence
- lower-priority work resumes only when no higher-priority eligible work remains

Work should also be considered runnable when it is already buffered locally, not only when new external readiness arrives. In practice this means the scheduler may need to requeue a `peer + direction` item after partial progress if:

- the peer still has unread buffered input that can be processed without another read callback
- the peer still has buffered output that can be written without waiting for another readiness notification

### 6. Fairness within a priority class

Strict priority across classes does not forbid fairness within a class.

The strict priority allocator should use round-robin fairness within each priority class, matching the current model as closely as practical where it does not conflict with strict inter-class priority.

In particular, the strict priority allocator should:

- use round-robin within `HIGH`
- use round-robin within `NORMAL`
- use round-robin within `LOW`

The key rule is that fairness must not weaken inter-class priority ordering.

## Initial Implementation Shape

The implementation is expected to require:

- a runtime configuration knob that selects allocator mode
- a top-level integration point in the pulse path that selects allocator behavior
- a shared scheduler interface or dispatch point used by peer I/O callbacks
- a strict-priority scheduler data structure for peer I/O work
- execution logic that drains eligible work in strict priority order for both readable and writable work
- time-aware yielding so the scheduler does not miss the next pulse
- integration so that newly generated work can be surfaced to the scheduler in the same pulse

The design should prefer isolating the new behavior behind the new allocator mode instead of scattering conditionals throughout the current allocator logic.

## Work Item Representation

For the first version, the scheduler should treat `peer + direction` as the runnable unit of work.

That means:

- a readable peer contributes a read-direction work item
- a writable peer contributes a write-direction work item
- the same peer may have distinct runnable items for different directions

The scheduler item should still be represented as a struct rather than an ad hoc pair, so that additional metadata can be added later if needed without redesigning the scheduler interface.

The intent is to keep the first version narrow:

- start with `peer + direction`
- do not expand the item shape unless there is a concrete reason
- enrich the struct later only if implementation experience shows it is necessary

## Codebase Constraints

The current codebase introduces several concrete constraints that the implementation needs to respect.

### 1. Multiple runnable-work entry points already exist

Today, direct peer I/O execution is entered from several places, not one:

- the bandwidth pulse and allocator path
- TCP read and write readiness callbacks
- uTP callbacks
- the zero-delay outbound flush timer callback

This means the scheduler seam cannot be limited to one callback site. The selected allocator or scheduler service must be reachable from all peer-I/O execution entry points that can currently consume bandwidth or make forward progress.

### 2. Pulse ordering matters

The current pulse first runs peer protocol maintenance and only then runs the bandwidth allocator. That maintenance can generate outbound work immediately.

The strict allocator should preserve that order initially:

1. peer pulse work runs
2. bandwidth quota is refilled
3. the strict scheduler drains runnable work

### 3. Buffered local work is first-class runnable work

The scheduler cannot define runnable work only in terms of external readiness notifications.

The current peer I/O code can remain runnable after partial progress because:

- input may already be buffered in `inbuf_`
- output may already be buffered in `outbuf_`
- protocol processing can generate more output while handling input

The strict scheduler therefore needs a notion of requeueing runnable work without waiting for a new socket callback.

### 4. Protocol or control messages currently get explicit precedence

The current allocator flushes non-piece outbound protocol messages before its fairness pass.

The strict allocator should preserve that intent. Strict priority between peers should not regress the existing preference for getting protocol or control traffic out promptly within a peer.

For newly generated outbound work, the initial implementation should prefer reusing the existing zero-delay outbound flush timer path rather than seeding runnable write items directly from `peer->pulse()`. This is less invasive and also naturally covers outbound work produced outside the pulse path, such as request-triggered output generation while processing inbound peer traffic.

### 5. Settings validation is not generic today

The current generic session settings serializer does not provide a built-in hard-fail path for invalid setting values. Invalid deserialization generally leaves the existing field value unchanged.

Because the allocator setting should warn and fall back on invalid names, that validation will still need to be explicit in the implementation rather than assumed from the generic settings loader.

The preferred shape is:

- define one shared allocator-name parser or validator
- invoke it from the session settings load or initialization path, close to where merged config is deserialized and applied
- reuse that same helper from any future entry path that may set allocator mode

This keeps the validation logic near config deserialization, which matches how this setting is intended to be introduced, while still avoiding duplicated policy. The validator should surface whether the value was recognized so the caller can warn and select `default`.

### 6. Session settings and RPC exposure are separate concerns

Adding a field to session settings is straightforward, but that does not automatically expose it through RPC `session-get` or `session-set`.

For the initial implementation, allocator selection should remain config-file only and should not be exposed through RPC.

### 7. uTP read-side behavior differs fundamentally from TCP today

The current TCP path is pull-based at the `tr_peerIo` layer: Transmission decides when to call into `try_read()`.

The current uTP read path is different. libutp pushes data into the peer I/O buffer via a callback, and the callback currently proceeds directly into input processing.

This is important because strict read-side priority is much simpler to enforce when Transmission controls when bytes are pulled from the transport. In the current uTP path, transport delivery into userspace happens before the scheduler gets to choose whether that peer should be serviced next.

For the initial implementation, the goal is not to boil the ocean on uTP. The implementation should stay as close as practical to current uTP behavior unless there is a concrete reason it cannot.

For v1, this means:

- TCP read-side work should participate in strict scheduler ownership
- uTP should preserve its current delivery model as much as practical
- if possible, scheduler integration should happen at the post-buffer processing stage for uTP rather than by trying to redesign transport delivery itself

This is an explicit compromise for the initial implementation, not a claim that TCP and uTP read-side behavior will be equally strict internally.

## Invariants To Preserve

- Bandwidth accounting must remain correct.
- Existing speed limits must still be honored.
- Existing behavior must remain unchanged in the legacy allocator mode.
- Work should not be lost, duplicated, or starved accidentally within a priority class.
- The scheduler must avoid unbounded busy looping when work is repeatedly requeued.
- The strict scheduler must yield in time for the next pulse.
- Invalid allocator names must produce a warning and select the legacy allocator.
- Allocator selection remains config-file only and requires restart in the initial implementation.

## Limited-Mode Budget Retention

The current strict scheduler enforces priority among work that is runnable now, but it is still fully work-conserving within the pulse.

That means a limited-direction pulse can still spend too much of its budget on `NORMAL` or `LOW` work early in the pulse if no `HIGH` work is runnable yet. If `HIGH` work becomes runnable later in the same pulse, it may find that the limited budget has already been consumed.

This is not a problem in unlimited mode, but it is a problem when the goal is to preserve some budget opportunity for higher-priority work that may appear later in the pulse.

### Goal

For limited directions, the strict allocator should retain part of the pulse budget for potential later-arriving higher-priority work instead of allowing lower priorities to consume the whole budget immediately.

The intended effect is:

- `HIGH` remains eligible immediately
- `NORMAL` becomes eligible more gradually over the pulse
- `LOW` becomes eligible even more gradually over the pulse
- if higher-priority work never arrives, lower-priority work should still be able to consume the available limited budget by the end of the pulse

This refinement is intended specifically for limited-mode behavior. Unlimited directions should keep the simpler strict work-conserving behavior.

### Scope

The first version of this refinement should be scoped narrowly:

- apply it independently per direction
- only activate it when the relevant direction is limited
- protect session-level limited directions only
- only apply that protection to work that actually honors session limits in the current bandwidth tree
- leave unlimited directions unchanged
- treat uTP read-side behavior as the existing v1 compromise rather than trying to make it fit perfectly into the same model immediately

Because the strict scheduler is session-scoped, the most natural first target is session-level limited-mode behavior. The first version should stop there.

### Design Direction

The current idea is to model lower-priority eligibility as cumulative release curves over pulse time rather than hard release cutoffs.

Instead of saying "start `NORMAL` or `LOW` at one specific time", the scheduler would compute how much limited budget lower priorities are allowed to have consumed by the current point in the pulse.

Pulse-relative time:

- `t = elapsed_in_pulse / pulse_duration`
- `t` starts at `0`
- `t` ends at `1`

The design should use two lower-priority release envelopes:

- a `NORMAL+LOW` release curve that limits total non-`HIGH` consumption
- a `LOW` release curve that limits `LOW` consumption specifically

This gives a cleaner interpretation than independent per-class curves:

- `HIGH` is always eligible
- `NORMAL` can run only while total `NORMAL+LOW` consumption remains below its release envelope at time `t`
- `LOW` can run only while both total `NORMAL+LOW` consumption and `LOW`-only consumption remain below their release envelopes at time `t`

This structure preserves room for later-arriving higher priorities more directly:

- unreleased `NORMAL+LOW` budget is implicitly reserved for possible future `HIGH`
- unreleased `LOW` budget is implicitly reserved for possible future `NORMAL`

### Behavioral Shape

The release curves should satisfy these properties:

- monotonic nondecreasing over the pulse
- start near or at zero at the beginning of the pulse
- reach full release by the end of the pulse
- release `LOW` more slowly than `NORMAL+LOW`
- avoid stranding budget at the end of the pulse if no higher-priority work arrives

The first implementation should use power curves. This keeps the model cheap to evaluate, easy to explain, and easy to invert when computing the next eligibility wakeup.

The release curves should therefore be:

- `NORMAL+LOW`: `f_nl(t) = t^p_nl`
- `LOW`: `f_low(t) = t^p_low`

with:

- `p_nl > 1`
- `p_low > p_nl`

This guarantees the desired shape:

- slower than linear early
- catching up later
- `LOW` always released more slowly than `NORMAL+LOW` before the end of the pulse

The purpose of the curves is not to create perfect prediction. The purpose is to reduce the chance that lower-priority work burns the limited pulse budget too early.

### Strategy Selection

The curve family should stay within power curves, but the release strategy should no longer be limited to a few hardcoded fixed presets.

The canonical configuration key should remain `bandwidth_strict_limited_curve`.

This setting should remain:

- configuration-file only
- restart-required
- only meaningful when `bandwidth_allocator = strict`

Required strategy values:

- `relaxed`
- `balanced`
- `aggressive`
- `dynamic`

The fixed-strategy exponent pairs should remain:

- `relaxed`: `p_nl = 1.5`, `p_low = 3`
- `balanced`: `p_nl = 2`, `p_low = 4`
- `aggressive`: `p_nl = 3`, `p_low = 6`

The fixed presets remain useful as:

- simple operator choices
- regression anchors for testing
- bounds or landmarks for dynamic tuning

The default should remain `balanced`.

Invalid names should log a warning and fall back to `balanced`.

### Dynamic Strategy

The fixed presets are easy to reason about, but they are not adaptive:

- if the curve is too mild, lower priorities consume budget earlier than necessary
- if the curve is too aggressive, lower priorities may not have enough pulse time left to catch up and some limited budget is stranded

The dynamic strategy should therefore remain a power-curve strategy, but one whose exponents are adjusted online from observed pulse outcomes.

This is not a new curve family. It is a controller over the existing power-curve family.

The objective should be:

- tighten lower-priority release as far as possible
- but relax again as soon as the current tightness causes the pulse budget to stop filling completely

In other words, the dynamic strategy should try to operate just below the point where overtightening starts to strand budget.

The dynamic strategy should be allowed to tighten beyond the current fixed `aggressive` preset if real pulse behavior supports it. The fixed presets should not be treated as mathematical limits, only as named anchor points.

However, the dynamic strategy still needs bounded engineering limits so it does not become effectively undefined or degenerate. The correct boundary is not aesthetic smoothness. The correct boundary is operational:

- if a very steep curve still lets lower priorities consume their released remainder by pulse end, it is acceptable
- if the remaining lower budget can no longer be turned into bytes on the wire before the pulse closes, the curve is too steep

The dynamic strategy should therefore treat late lower-priority catch-up as the key safety signal.

### Dynamic Control Signals

The dynamic strategy should make decisions over a short multi-pulse window rather than from a single pulse. With the current `500ms` pulse, a natural first window is `4` pulses (`2s`).

Per pulse, the strategy should observe at least:

- protected session pulse budget
- `HIGH` piece bytes consumed
- `NORMAL` piece bytes consumed
- `LOW` piece bytes consumed
- whether `HIGH` demand was present in that pulse
- whether lower-priority demand was present in that pulse
- whether lower-priority work was gated by the release policy during that pulse

From those signals, the dynamic strategy should derive two high-level outcomes:

- tighten is safe
  - `HIGH` had real demand
  - total protected budget was still fully or nearly fully consumed
  - lower priorities still managed to catch up by pulse end
- relax is required
  - lower-priority demand existed
  - some limited budget was left unused by pulse end
  - and lower priorities had been gated earlier in the pulse, so the underfill is attributable to curve tightness rather than complete lack of lower demand

The hold case is everything in between:

- no clear sign that more aggression is needed
- no clear sign that the current aggression stranded budget

This controller should be deliberately asymmetric in confidence even if it is symmetric in step size:

- tightening should require repeated successful windows
- relaxing should require fewer confirming windows once real lower-budget slip is observed

The exact thresholds can be tuned later, but the design should be based on:

- windowed classification
- hysteresis
- adjustment by small steps

### Policy Abstraction

The current retained implementation does not yet have a pluggable admission seam.

Today, the strict scheduler itself owns all lower-priority release behavior:

- exponent lookup lives in `strict_curve_parameters()`
- power-curve math lives in `released_bytes()`
- admission decisions live in `retained_piece_limit()`
- next eligibility wakeups live in `next_retained_wakeup_msec()`
- lower-priority accounting lives in `charge_retained_piece_bytes()`

That means adding a new curve or admission strategy currently requires editing the allocator itself. This is exactly the coupling that the next iteration should remove.

The strict scheduler should instead delegate per-priority admission to a policy object through a narrow interface.

The scheduler should not decide whether a given piece transfer is "inside the curve". It should ask the active policy.

In the fixed power-curve implementation, `HIGH` will usually be admitted immediately. However, the API should still ask the policy about `HIGH` too rather than hardcoding that assumption in the allocator. This keeps the seam general enough for future strategies that may want to reason about every priority explicitly.

The interface should be shaped around what the scheduler already knows naturally:

- pulse start
- pulse duration
- protected pulse budget
- current time inside the pulse
- peer direction and priority
- pulse-local lower-priority consumption so far
- exact piece bytes consumed by the most recent flush

The first useful shape is:

- `on_pulse_start(...)`
- `admit(...) -> { piece_limit, next_wakeup_msec }`
- `charge(...)`
- `on_pulse_finish(...)`

Where:

- `admit(...)` answers whether piece work for the queried priority is eligible now, how many piece bytes may be consumed now, and when the next useful wakeup would be if it is gated
- `charge(...)` records exact piece-byte consumption after a successful read or write flush for the queried priority
- `on_pulse_finish(...)` receives a pulse summary for dynamic adaptation

With that seam in place:

- the existing fixed power-curve implementation becomes one policy module
- the new dynamic power-curve implementation becomes a second policy module
- future curve or admission modules can be added by extending the factory rather than changing allocator logic

This is the right decoupling line. The allocator remains responsible for queueing, draining, timers, and byte execution. The admission policy becomes responsible only for per-priority eligibility and pulse-to-pulse adaptation.

### Implementation Sketch

At a high level, the strict scheduler still needs pulse-local state for limited-mode retention:

- pulse start time
- pulse duration
- per-direction pulse budget for the session-level limited cap being protected
- per-direction cumulative lower-priority consumption for the current pulse
- the next time at which a currently gated lower-priority queue may become eligible
- per-pulse outcome data needed by a dynamic policy

Eligibility decisions would then change from "is anything queued in this class?" to:

- is the class queued?
- is the class allowed to spend more limited budget at the current pulse time?

The scheduler loop would need to handle three outcomes when lower-priority work is queued:

- eligible now: drain it as usual
- not eligible yet, but will become eligible later in this pulse: arm a timer for the next eligibility point
- no more pulse time left: stop and wait for the next pulse

The release model should not wake lower-priority work byte-by-byte. That would create too many scheduler wakeups and tiny I/O attempts. The first implementation should therefore release lower-priority work in bounded piece-data quanta while still charging exact piece bytes against the protected budget. The current target is a fixed `1024`-byte retention quantum, with the final partial quantum released near the end of the pulse.

This means lower-priority gating cannot rely only on the current zero-delay continuation timer. It needs a real wakeup time tied to the release model so that queued lower-priority work neither spins nor stalls indefinitely.

The scheduler should reuse its existing scheduler-owned one-shot timer for this purpose rather than introducing a second timer. The same timer should handle both:

- immediate continuation when eligible work exists now
- delayed wakeup at the earliest next lower-priority eligibility point when only gated work remains

In practice, the scheduler should compute the earliest useful wakeup and arm the same timer accordingly:

- eligible work exists now: arm `0ms`
- only gated work remains: arm the delay until the next envelope release point
- no queued work remains: do not arm the timer

If newly arrived higher-priority work makes immediate progress possible before a delayed wakeup fires, the scheduler should stop and re-arm the same timer for immediate draining.

For the next iteration, the scheduler should stop embedding the curve behavior directly and instead route these decisions through the admission-policy seam above. That refactor is a prerequisite for `dynamic`.

### Code Findings

On the retained branch, the code already has most of the raw signals needed by the new policy abstraction:

- the scheduler already knows exact `piece_bytes` for read and write flushes through `flush_with_result()`
- lower-priority accounting is already pulse-local
- the scheduler already computes the next lower-priority wakeup and already owns the only timer needed to honor it

What it does not yet have is the abstraction boundary:

- there is no existing callback or virtual hook for lower-priority curve admission
- the strict scheduler computes and enforces the curve directly

So the next implementation should be a refactor, not a feature bolt-on:

1. lift the current fixed power-curve behavior behind a policy interface with no behavioral change
2. add pulse summary plumbing and tests for policy inputs/outputs
3. add `dynamic` as another policy implementation behind the same config key
4. keep the allocator unchanged once the policy seam exists

### Implementation Plan

The next code series should be split into logical steps:

1. `docs: design dynamic strict curve strategy`
   - design-note update only

2. `bandwidth: factor lower-priority admission policy seam`
   - introduce the policy interface
   - move existing fixed power-curve logic behind it
   - no intended behavior change

3. `bandwidth: report pulse outcomes to admission policy`
   - add pulse-local summary accounting needed for dynamic adaptation
   - cover exact signals in tests

4. `settings: add dynamic strict limited curve option`
   - extend parsing/serialization
   - keep fixed presets as they are

5. `bandwidth: add dynamic power-curve admission policy`
   - implement windowed tighten/hold/relax behavior
   - preserve wakeup and charge semantics through the existing policy seam

6. `tests: cover dynamic curve adaptation`
   - tighten when `HIGH` pressure is real and lower catch-up is still complete
   - hold when the current curve is adequate
   - relax when overtightening strands lower budget

### Interaction With Current Accounting

This refinement is meant to target limited-mode behavior, so the byte quantity used for the curves matters.

The strict scheduler needs a pulse-local measure of how much protected budget lower priorities have already consumed. There are several plausible definitions:

- raw bytes transferred by the scheduler
- bytes returned by `flush()`
- the same limited-budget quantity that `tr_bandwidth` decrements from `bytes_left_`

The strict allocator should use the same limited-budget quantity that `tr_bandwidth` decrements from `bytes_left_`.

This is the strictest and cleanest semantic choice because it matches the budget actually being protected. If the scheduler does not currently receive that quantity directly, the implementation should add the plumbing needed to surface it rather than approximate it with a nearby byte count.

This also means the retention model should preserve current control-traffic semantics. Today, limited-budget accounting decrements `bytes_left_` only for piece data, while non-piece protocol or control traffic and packet overhead contribute to raw accounting but do not consume the limited piece budget. The limited-mode release envelopes should therefore track piece-budget consumption only, not raw traffic volume.

That does not mean control traffic is outside the limit machinery entirely. Read and write execution still pass through `clamp()`, so control traffic still depends on available bandwidth opportunity to make progress even though it does not itself decrement `bytes_left_`. The retention model should match the accounting rule, but any implementation discussion should keep this distinction explicit.

To preserve current behavior more closely, the scheduler should have explicit visibility into whether a queued write item has pending protocol or control output. Without that signal, lower-priority protocol traffic could be delayed indirectly just because lower-priority piece-budget release is being held back.

The first version should keep this visibility minimal:

- expose whether a peer has pending protocol or control writes
- use that signal to let lower-priority control traffic preserve current semantics as much as practical
- avoid full output-queue introspection unless later implementation experience shows it is necessary

### Known Constraints

- This design can only preserve budget that has not already been consumed. It does not retroactively reclaim budget from lower-priority work once spent.
- The design naturally fits session-level limited-mode protection better than arbitrary subtree protection.
- Session-level retention should not gate work that does not actually honor session limits. The current tree allows torrents or groups to stop honoring parent limits, and that needs to be reflected in the implementation.
- The current strict scheduler only sees a flat peer list produced by `allocatePulse()`, with each peer carrying its effective priority. It does not currently receive subtree identity or ancestor-chain metadata.
- A peer may sit under multiple relevant limited ancestors in the current tree, for example `session -> group -> torrent -> peer` or `session -> torrent -> peer`.
- Actual limit enforcement and accounting recurse through the whole honoring ancestor chain. Protecting limited subtrees correctly would therefore require per-limited-ancestor retention state rather than a single subtree tag.
- If subtree retention is ever pursued later, the likely shape is:
- add plumbing so the scheduler can identify the limited ancestor chain for each `peer + direction`
- keep pulse-local retention state keyed by `tr_bandwidth*` for each limited protected ancestor
- charge lower-priority piece consumption against every applicable protected ancestor in the chain
- require a lower-priority item to satisfy all relevant ancestor envelopes before it becomes eligible
- uTP read-side behavior will remain less strict than TCP read-side behavior in the first version.

## Open Questions

None at the moment.

## Testing Strategy

The new allocator should be validated primarily with behavioral tests that prove its ordering guarantees rather than only with narrow unit tests of internal helpers.

The test plan should include both:

- legacy-mode tests to confirm that `default` behavior remains unchanged
- strict-mode tests to confirm the new ordering guarantees

The most important tests for the new allocator are:

### 1. Pulse-ordering tests

Behavioral tests should verify that within a pulse:

- `HIGH` work is served before `NORMAL`
- `NORMAL` work is served before `LOW`
- lower-priority work does not make progress while higher-priority eligible work still exists
- round-robin fairness is preserved within a priority class

### 2. Event-driven priority tests

Behavioral tests should verify that for event-driven work within a pulse:

- readiness notifications are routed through the selected scheduler
- in `strict` mode, event callbacks do not perform I/O directly
- queued `HIGH` work is executed before queued `NORMAL` or `LOW` work
- queued `NORMAL` work is executed before queued `LOW` work
- newly arrived `HIGH` work preempts pending lower-priority work within the same pulse

### 3. Cross-source tests

Behavioral tests should verify that strict ordering continues to hold when work flows between sources, for example:

- pulse-generated work is handled by the same priority rules as readiness-driven work
- execution generates more runnable work and that work is still ordered correctly
- a pulse does not need to end before newly generated higher-priority work can take precedence

### 4. Pulse-timing tests

Behavioral tests should verify that:

- the strict scheduler yields in time for the next pulse
- one long drain loop does not indefinitely delay subsequent pulses

### 5. Limit and accounting tests

Behavioral tests should verify that:

- strict ordering does not break bandwidth limits
- byte accounting remains correct
- upload and download directions both obey strict priority
- legacy behavior remains unchanged in `default` mode

### 6. Configuration selection tests

Tests should verify that:

- unset allocator name selects legacy behavior
- `default` selects legacy behavior
- `strict` selects the new allocator
- invalid allocator names log a warning and select legacy behavior

The current tree does not appear to have dedicated bandwidth allocator tests yet, so this work will likely need new coverage.

The existing test harness already provides useful building blocks for event-driven behavioral tests, including:

- libevent pumping helpers
- socketpair-based peer I/O tests

The expectation is that the test harness should be able to inject readiness into the scheduler and observe execution order. If that turns out not to be sufficient, a focused scheduler test harness should be added rather than weakening the behavioral coverage goals.

## Build and Validation Workflow

This work is being developed in a Nix-centric environment and Linux validation is the primary target.

The preferred local validation path is based on the `transmission_4` package from a local `nixpkgs` checkout, with Linux builds executed through configured `x86_64-linux` builders.

Helper expression:

- [extras/nix/transmission-local-linux.nix](/Users/ihrachyshka/src/transmission/all-sync-bandwidth-allocator/extras/nix/transmission-local-linux.nix)

Checkout prerequisite:

```bash
git submodule update --init --recursive
```

The helper expression copies the current checkout into the Nix store. Unlike the released nixpkgs tarball, that means the local checkout must already have the required third-party submodules populated.

Expected command shape:

```bash
nix build \
  -I nixpkgs=$HOME/src/nixpkgs \
  -f extras/nix/transmission-local-linux.nix package \
  --system x86_64-linux \
  --no-link \
  -L
```

Checked build with tests:

```bash
nix build \
  -I nixpkgs=$HOME/src/nixpkgs \
  -f extras/nix/transmission-local-linux.nix checked \
  --system x86_64-linux \
  --no-link \
  -L
```

The helper expression exists because this branch's source layout is not identical to the released tarball currently packaged in nixpkgs, so the local validation path needs a small branch-specific override while still staying close to the nixpkgs package definition.

As implementation proceeds, this Nix path should be used regularly to confirm:

- the tree still builds on Linux
- the relevant tests still pass
- each logical commit is in a releasable state

## Success Criteria

This effort will be considered successful if all of the following are true:

- a user can opt into the new allocator through configuration
- the legacy allocator remains behaviorally unchanged when selected
- `HIGH` priority traffic strictly dominates `NORMAL` and `LOW`
- `NORMAL` priority traffic strictly dominates `LOW`
- new higher-priority work can preempt lower-priority work within the same pulse
- the strict scheduler yields in time for the next pulse
- bandwidth limits remain enforced correctly
- the implementation is isolated enough that future iteration on the strict-priority mode does not destabilize the legacy mode
