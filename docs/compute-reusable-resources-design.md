# Reusable native compute resources and job retirement

Updated 2026-10-11. The internal native lifecycle described here is implemented,
independently reviewed and frozen in `build/macos-reuse-final`: 92 translation
units, 406 resolved imports and 234 verified source files. Executable SHA-256
is `fe01c5be0f8838dd238fd079cf7709324a81a69d13254e9590f5c50ec854f66b`.
Focused native validation passed eight kernel objects with a 2 KiB frame limit,
241,988 VM-context checks and 5,202 queue checks. The 3,820 KiB focused stage
and 9,792 KiB full-build stage were removed. See
[exact build evidence](validation-product-wire-power.md).

The internal two-job test is connected to engine startup after ShaderVerify,
but has not run on the target GPU. The later v1.4 Session/client profile and
bounded result-history API are implemented and independently reviewed outside
this immutable snapshot. Strict and UBSan protocol tests each passed 158,417
checks, and three real kernel-header objects compiled. Native transport
validation linked five artifacts, passed 158,417 ASan/UBSan checks and removed
the 1,040 KiB stage. The combined v1.4 kext is now frozen separately at
`build/macos-reusable-v14-final`, executable
`67fc0db187d72a5fd459194924d0f17b91dd6f20c3a08c69eee6d19863ea8cb6`:
92 units, 406 imports, 234 verified sources and removed 9,808 KiB stage.
Its three-object native focused check passed with a removed 616 KiB stage.
Existing v1.1/v1.2/v1.3 clients retain their single-job behavior. The first
reusable interface remains serialized, bounded Gfx1100LeafBufferV1 compute,
with one in-flight job per session and the existing exclusive queue and
FunctionGroup operation. General Metal, concurrent queues, imported buffers
and rendering require more work.

## Starting point preserved from the pre-reuse design review

The following table records the c1cb8-era design inputs; line references name
that earlier implementation. Later native reuse changes are covered by the
contract and milestone above, and do not retroactively change the old build.

| Existing implementation | Consequence for reuse |
| --- | --- |
| `HackyNaviClientSession.cpp:186-303` reserves CPU result storage, permanently sets `sealed_`, captures one job, submits it, reads actual GPU output and calls `retireContext`. | Clearing `sealed_` alone cannot make a second job possible. The underlying VM has already been destroyed. |
| `HackyNaviGraphicsQueue.cpp:304-340` validates its own completion, rechecks fence/RPTR and CP idle, detaches both hubs, then calls `releaseStorageLocked`. | The existing owner has a useful real completion boundary, but its cleanup is terminal. |
| `HackyNaviVMContext.cpp:593-616` releases the whole tree and every DMA BO, clears all mappings/capture state and removes the VMID registry entry. | Data and control ownership must be separated before this operation can support persistent buffers. |
| `HackyNaviVMContext.cpp:74-88,150-188` permits edits only in unexposed `Prepared`, excludes internal BOs from public handle lookup, increments generations and refuses generation wrap. | Reuse must return to a proven nonpublished state and preserve all live data generations. |
| `HackyNaviVMProduct.cpp:136-204,206-335` independently admits copied ISA, owns private code/IB/parameter BOs, and requires every GPU mapping to match exactly one admitted binding. | Keeping unrelated mappings between jobs is currently rejected. Reuse must not silently relax this admission rule. |
| `src/vm_context.cpp:213-271` enables only `Fresh`; detach leaves `Detached` with retention latched. | Repeated activation needs an explicit checked protocol transition, not assignment of a new `Context` or clearing its retention bit. |
| `src/vm_page_tree.cpp:147-173` can remove PTEs while retaining the tree; empty table pages persist until full release. | Private mappings can be removed after independent retirement, while retaining the same root and data mappings. Page-capacity exhaustion remains explicit. |
| `native_abi.hpp:24-43` advertises `SealedSingleJob` in the current profiles; the client also seals locally. | Persistent behavior needs a distinct negotiated contract. Existing v1.1/v1.2/v1.3 clients keep their current semantics. |

`HackyNaviVMCompletion` is issued only by the real queue after synchronized
GPU fence and consumed-RPTR observation. It retains the native queue and VM.
`completionMatchesLocked` compares issuer, context, epoch, VMID, exact queue
producer/sequence and result handle/generation/range; it rejects later producer
advances. `retireCompletion` additionally performs `checkIdleCompletion` and
revokes readback before the first detach write. This is the starting proof,
not a caller-supplied completed flag. The pinned Linux VMID manager likewise
consults active fences before selecting an idle VMID and separately tracks GPU
reset counts. [Primary source](https://github.com/torvalds/linux/blob/3857c2fe5449541e24afc5efdb0f81a8a8f9a3a0/drivers/gpu/drm/amd/amdgpu/amdgpu_ids.c#L215-L289).

## Native lifecycle contract implemented in the fe01c5be milestone

`retireContext` remains terminal teardown. The separate `consumeJob` operation
on the queue-issued completion consumes one completed job and preserves the
resource arena. It executes inside the existing native owners; no wire
structure, public numeric token or protocol-test report may authorize it.

The complete operation holds the existing FunctionGroup/GART authority and
Queue -> Firmware -> GART lock order. Session serialization remains outside
those descendant locks. Do not release the FunctionGroup operation between
the final result copy and completed-job cleanup. The required sequence is:

1. Require normal admission for reuse. Recheck the exact live completion,
   current resource epoch, retained sole producer, immutable capture, running
   queue with no pending work, synchronized fence/RPTR and bounded CP idle.
   Keep `rawIndirectUsed_` exclusion and `isolatedClaimed_` latched. Recovery
   scopes may destroy resources but must not reopen reusable admission.
2. Revoke the completion's live readback/retirement permission before the
   first detach write. Keep the observed job identity separately for the
   eventual historical result. A consumed completion cannot act on job N+1.
3. Run the existing `detachLocked` sequence: GFXHUB then MMHUB invalidation,
   required publication/ACK and actual context-control disable/readback.
   Both protocols must finish `Detached`, with the actual same tree/VMID.
   A partial hub detach or any write/read ambiguity remains a retained fault.
4. While producers are still excluded and both contexts disabled, remove
   PTEs only for the job's private internal BO mappings. Use the real mapping
   ledger's exact handle/generation/VA/extent and `Tree::unmap`. For a product,
   this includes every variant code BO plus its IB and parameter page, not
   merely `capturedShader_` (which names only variant zero). Verify each
   control is internal, accounted for, uniquely mapped and not a data binding.
5. Release only those private DMA owners after their mappings are removed;
   update each mapping count, resident bytes and slot fields. Preserve the
   generation counters of released slots so later allocation increments them.
   Never zero the whole `Buffer` ledger entry and reuse generation one.
   Keep every noninternal allocation, including data and unmapped code staging,
   its actual DMA mapping/descriptor, handle generation and intended VA records.
6. Release the private `ProductCapture` and its scratch only after extracting
   and validating the full control ledger. Clear all per-job captured class,
   request, result, code/IB/parameter handles and submitted queue/sequence/
   producer bindings. Preserve `epoch_`, VMID, root, data tree pages, data
   mappings and the strong registry hold.
7. Only after every preceding action succeeds, make the native arena mutable
   again, with no publication hold. Drop exactly the hardware self-reference
   outside all owner locks; keep the VMID registry and the caller's reference.
   The next job uses the same actual data allocations. No CPU readback copy is
   uploaded to masquerade as persistence.

Do not route this path through `releaseStorageLocked`: it is deliberately
terminal. Factor precise private-control cleanup from the ledgers, while
keeping full teardown available for close/reset. A cleanup error must never
return the arena to `Prepared`. Preserve existing GART teardown-error latching,
close admission, retain surviving data/registry ownership, and report through
the real owner-failure path. An emptied control slot must not turn a repeated
cleanup call into a false success after a DMA teardown error.

## Reactivation and stale identities

Keep `vmcontext::enable` Fresh-only. Add the separate
`vmcontext::reactivate(Context&, const Io&, const Poll&, Result&)` operation,
requiring an existing `Detached` context with its historical retention latch.
It takes no replacement layout or configuration: its private stored hub, VMID,
root address/domain/size, VA range and resource epoch remain unchanged.
Observe the disabled context, remembered root/range and existing global guards.
Republish the real modified tree and BO contents, then perform the existing
control enable, invalidation and readback protocol without rewriting the root.
Do not overwrite or reconstruct `mm_` or `gfx_` to manufacture `Fresh`; keep
the protocol's historical retention bit latched. Native queue completion plus
successful detach establishes the separate permission to edit before
reactivation. The pure protocol by itself does not grant that permission.

`activateLocked` must distinguish first publication from a fully consumed
prior job. Reacquire its hardware self-hold before any potentially visible
publication. If the second hub fails after the first enabled, retain all
controls/data and enter the existing fault path. Submission still validates
the newly captured private ISA/images before activation; no admission result
is cached across mutable intervals.

Add a private nonwrapping capture generation to the VM and completion binding.
Advance it when starting a new native capture and bind it into the actual
queue claim before exposure. Compare it in live completion checks and retain
it in historical per-session job records. It supplements, rather than
replaces, existing epoch, VMID, BO generations, issuer, sequence and producer
checks. The generation does not authorize cleanup without the live native
completion. Refuse exhaustion before starting another capture.

Keep queue sequence and producer counters monotonic. `gfx_queue.cpp:99-105`
already refuses their wrap. Add/read the actual queue's nonmutating capacity
check before VM exposure so exhaustion does not unnecessarily publish a new
VM, while preserving the existing authoritative check at submission. Do not
reset queue counters between jobs. A new queue/reset epoch invalidates all old
session and completion identities. Finite counter or VMID exhaustion is a
reported resource limit, not a reason to accept stale tokens.

An old completion must fail after consumption, a new capture, later queue
producer advance, same-slot BO generation change, context close, PM revocation,
reset or queue replacement. Its remaining OSObject reference may keep an owner
alive but grants no GPU access. Result records remain historical observations
and cannot authorize another detach, DMA release or fresh buffer read.

## Data mappings, hazards and Session integration

Initially allow edits only between completed jobs, while both hubs are
disabled. Reuse the actual `readBuffer` synchronization and write/publication
path. Retain the 64 BO slots, 128 mappings, 256 MiB resident limit and 1,024
page-tree-page limit. Controls count against the same limits. Repeated jobs
must release control BO slots; empty table pages may remain as the current
tree implementation specifies, and new VA patterns can explicitly exhaust
its capacity. No silent tree reconstruction is needed for the first step.

The existing product validator permits only the currently referenced exact
R/RW data mappings and private controls. For job A->B followed by B->C, retain
all three real BOs but initially leave C unmapped; after job one, unmap A,
change B's mapping from RW to R and map C as RW before capture two. The bytes
in B stay in its original GPU-written allocation. This is a concrete first
dependency test. Keeping extra data allocations is allowed; keeping extra
GPU mappings or broadening an input to RW is not an implicit exception to
admission. A later general mapping policy requires its own ISA/resource audit.

Preserve current single-job profiles. A new reusable profile must explicitly
drop `SealedSingleJob`, negotiate its own capability/version, and implement
the following behavior in Session, UserClient, protocol and client together:

- Represent preparing, in-flight, reusable-idle, lost and closed states
  explicitly. One submission owns the session lock and full operation through
  actual completion and completed-job cleanup. All buffer mutation/free waits
  behind that serialization or returns an explicit busy/error result.
- Publish a job identifier only after the actual GPU result and successful
  completed-job cleanup. Reserve bounded result memory before publication,
  as today. Keep an explicit bounded table of historical jobs keyed by the
  connection epoch plus actual sequence/capture generation. Never overwrite
  a still-readable result silently when submitting the next job.
- Add explicit consumption/release of a historical result record, with a
  no-wrap identity and a defined NotFound result after release. Reserve from
  the existing aggregate result budget. If the table/budget is full, reject
  before GPU publication. Completion/history release does not free a live
  resource BO or authorize hardware cleanup.
- Retain the whole context after ambiguous execution/detach failure. On close,
  use ordinary CPU cleanup only for never-published or queue-proven reusable
  idle state; use the actual terminal completion path for a live completed job,
  otherwise the existing registered retirement/recovery owner. Preserve the
  first teardown failure. Recovery never restores an old connection epoch.

Keep completion-object release outside the Session and descendant locks where
destruction can acquire GART. Review every replacement of `completion_` and
every post-job self/registry release for this lock order. Existing
`retireCompletedContexts` can release a proven idle arena whose publication
hold was already discharged, but its stopped-queue active-job branch must
also check the new capture identity. The GART registry must continue blocking
GART teardown while any persistent arena remains.

This first serialized design supplies hazards by actual exclusion and GPU
completion, not a general asynchronous dependency graph. It also retains
copied CPU I/O only. `MTLBuffer.contents`, shared/managed storage, dirty-range
tracking and CPU/GPU overlap need real mappings and synchronization later;
do not advertise those semantics based on a heap shadow.

## Completed internal primitive and next interface work

The internal consume-job path, checked detached reactivation and two-job
kernel test using retained BOs are implemented, independently reviewed and
native-linked in fe01c5be. Its probe requires two real completions, 8,320 words
including 128 guard words, nine stale-token rejections and actual control-slot
reuse with newer generations. These are required checks, not GPU results.
Only a real target run establishes persistence and repeated GPU execution.
The separately negotiated v1.4 profile with eight bounded history slots and
explicit result release is now implemented and independently reviewed;
native transport validation and the combined 67fc0db1 kext build passed.
Target execution is next. Older profiles remain unchanged.

| Workstream | Exact responsibility | Dependency |
| --- | --- | --- |
| VM protocol/arena owner | Separate checked `Detached` reactivation, private-control-only unmap/free, preserved data ledgers, generations and publication/registry holds; changes in `vm_context`, `VMContext`, `VMProduct` and focused tests. | Freeze the state/ownership contract with queue owner before editing shared headers. |
| Queue/completion owner | Consume the actual completion under existing locks, idle/producer checks, capture-generation binding, no-wrap admission and reference drops; changes in `GraphicsQueue`, `VMCompletion`, queue tests. | Uses the arena operation only after proof; no public authority flags. |
| Session/transport owner | Add the negotiated reusable profile, serialized job states, bounded historical result ownership and exact client cleanup semantics. | Start after the native lifecycle contract is frozen; retain all old-profile tests. |
| Independent reviewer/target validation | Review lock/reference/failure paths, compile real kernel objects/full kext, then execute actual multi-job/readback/recovery tests. | Runtime requires the engine-enabled target and actual ownership prerequisites. |

Required tests include both hubs' every read/write/publication/ACK failure,
partial detach/reactivation, every private-control cleanup failure, unexpected
internal/data mapping membership, BO/capture/sequence exhaustion, stale tokens,
close/PM revocation at every stage, unchanged data handle/owner across jobs,
and no data/control release before the actual queue proof. Host tests can
validate protocol transitions and rejection behavior; they cannot issue a
native completion or establish DMA retirement.

The target test should use actual independently compiled kernels for two jobs,
read the first result for validation without reuploading it, then let job two
consume that same GPU allocation. Verify both real fences/RPTR, unchanged
data owners, private control reclamation, the second output and guard bytes,
and final explicit close/retirement. A fault test must demonstrate admission
closure and retained-owner recovery, not merely return an error string.

The supplied-source `--reuse` smoke test now cross-compiles and passes an
independent review against the actual v1.4 client implementation. It compiles real
local64/local3 variants, dispatches A->B then retained B->C with no CPU writes
after the first submission, validates distinct guards and BO contents, checks
historical results and strict NotFound after release, then explicitly frees
all resources and closes. The native tool and v1.4 kext link; target execution
remains. See [its instructions](../userspace/transport/PRODUCT-SMOKE.md).

The later actual C++ Context backend owns persistent Buffer objects and
immutable compiler Pipeline copies. Its separate
[Context smoke test](../userspace/runtime/CONTEXT-SMOKE.md) exercises the same
A->B->retained B->C chain through those production APIs, with distinct guards,
stable addresses and explicit cleanup. Strict object compilation passes;
native runtime validation is in progress. No wrapper GPU run is established.

Separate compiler evidence in `build/metal-metallib/native-run-2` passed all
17 native commands, seven actual SDL vertex/fragment functions and 25 negative
C ABI cases, with 23 exports and a removed 158,219,947-byte stage. Native CLI
was explicitly skipped. That measured MTLB 2.5 support does not establish
modern compute containers, Metal objects or GPU execution.

Remaining blockers are target proof of engine
execution/recovery, followed by real Metal accelerator discovery and object
contracts. No register field, CPU result copy or disabled hub alone removes
those blockers.
