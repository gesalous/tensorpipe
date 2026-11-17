# tensorpipe over Portals4 / BXIv2: design

Living document. Sections marked **TBD** or **OPEN** are decisions still to take.
Update it in the same commit as the code that changes the design.

## 1. Goal and scope

Run tensorpipe's `ibv` transport over Portals4 on BXIv2 by providing the verbs
calls it uses on top of the Portals4 API. First milestone: the `Ibv*` unit tests
(`tensorpipe/test/transport/ibv`) pass with the Portals-backed library.

Non-goals (for now): CUDA/GDR channels, the `ibv` code path for devices that are
not BXI, multi-rail, a native tensorpipe `portals` transport.

## 2. Constraints

- **No FIFO delivery in a PTE.** Messages from one sender can be delivered, and
  their events raised, in a different order from the one they were sent. See 6.
- **Must coexist with real libibverbs in the same process** (PyTorch: NCCL IB,
  Gloo, UCX). We must not interpose or replace `libibverbs.so.1` globally.
- Build/run hosts: `borg0`, `borg1` (via `ssh -J shark2`), BXIv2, EL8, gcc 8.5,
  libportals + `portals4_bxiext.h`. The workspace is an NFS share.
- Reference implementation: SPDK branch `bxiv2-nonmatching`
  (`lib/rdma_provider`), a Portals-backed verbs + rdma_cm for NVMe-oF.

## 3. Architecture

```
tensorpipe ibv transport (unchanged call sites)
   |  IbvLib: dlopen(TP_IBV_LIBRARY, RTLD_LOCAL), symbols resolved as ptl_<name>
   v
libtensorpipe_portals.so  (portals_shim/)     exports ptl_* with ibv_* signatures
   |  now:   every function is a FATAL "unimplemented" stub
   |  goal:  Portals4 (PtlNIInit, PtlPut, PtlGet, PtlLEAppend, PtlEQGet ...)
   v
libportals -> BXIv2
```

- Selection: env var `TP_IBV_LIBRARY=<path to libtensorpipe_portals.so>`; unset
  means stock libibverbs, unchanged behaviour.
- The data-path calls (`poll_cq`, `post_send`, `post_recv`, `post_srq_recv`) are
  inline in `verbs.h` and dispatch through `ibv_context.ops`; stock libibverbs
  doesn't export them. `IbvLib` therefore loads `ptl_poll_cq` etc. as function
  pointers in override mode.
- The shim never calls `ibv_*` and does not link libibverbs (it would resolve to
  the real library). It links only libportals and libc. Checked: the built
  library has no undefined `ibv_*` symbols.
- Symbol visibility: everything hidden except the `ptl_*` exports.

## 4. Verbs surface to implement

Calls used by tensorpipe (from `common/ibv_lib.h`):

| Call | Now | Portals-backed plan |
|---|---|---|
| get_device_list / free / get_device_name | **done** | one fake device `portals0`; the list is empty if the process-wide NI cannot be created, so tensorpipe reports the transport as not viable |
| open_device / close_device | **done** | process-wide NI (lazy, released at exit); per context: EQ, whole-address MD, one PTE, one persistent whole-address LE (5.1) |
| query_port | **done** | ACTIVE, MTU 4096, `lid = 0` so tensorpipe addresses peers by gid |
| query_gid | **done** | gid = `"PTL4"`, nid, pid, pte (`struct ptl_gid`) |
| alloc_pd / dealloc_pd | **done** | trivial object |
| reg_mr / dereg_mr | stub | **OPEN** see 5.3 |
| create_cq / destroy_cq | **done** | a *logical* CQ: every event of a context arrives on the context's one EQ (5.1), so several CQs can share it |
| create_srq / destroy_srq | **done** | accounting only (5.4): ring of posted wr_ids |
| create_qp / destroy_qp | stub | local state: qpn, peer identity, sequence numbers |
| modify_qp | stub | INIT/RTR/RTS: store peer `(nid,pid,pte,qpn)` from `ah_attr` + `dest_qp_num` |
| post_send | stub | `RDMA_WRITE_WITH_IMM` -> PtlPut to RMA window; `SEND_WITH_IMM` -> PtlPut to message LE |
| post_srq_recv | **done** | records wr_ids; only zero-sge receives (FATAL otherwise) |
| post_recv | stub | not called by any `Ibv*` test |
| poll_cq | partial | T1: drains nothing and returns 0; any event is FATAL until the data path (T3) |
| get_async_event / ack_async_event | stub | no events generated initially |
| event_type_str / wc_status_str | stub | local tables |

Tensorpipe does **not** use rdma_cm: QP parameters are exchanged over its own
TCP socket and applied with `modify_qp`. So the SPDK control-plane server
(PTE 0, open/close-connection messages) is not needed here.

## 4.1 What tensorpipe does on the wire

- One global SRQ (1024 zero-length receives), one CQ, one QP per connection.
- Data: `RDMA_WRITE_WITH_IMM` of `length` bytes into the peer's 2 MiB inbox ring,
  `imm = length`; a wrapping write is split into two writes.
- Acks: zero-length `SEND_WITH_IMM`, `imm = bytes consumed`.
- `poll_cq` results are demultiplexed by `wc.qp_num` = the **local** qpn, also
  for receive completions.
- `WC_RECV_RDMA_WITH_IMM` -> "remote produced `imm` bytes";
  `WC_RECV` (+imm) -> "remote consumed `imm` bytes";
  `WC_RDMA_WRITE` / `WC_SEND` -> local send completions (flow control).

**Messages versus operations.** The transport is a byte stream per connection,
not a message transport: `write` copies into a 2 MiB outbox ring, `read` copies
out of the 2 MiB inbox ring, and the layers above (channels) frame their own
messages. Only two verbs ever reach the wire, confirmed by grep (no
`RDMA_READ`, no atomics anywhere in `transport/ibv`):

| Operation | Verb | Role |
|---|---|---|
| data | `RDMA_WRITE_WITH_IMM`, `imm = length` | one-sided placement into the peer inbox at `peerInboxPtr + (head mod 2 MiB)`; wrap-around is two writes; the imm raises the receive completion |
| ack | `SEND_WITH_IMM`, zero length, `imm = bytes consumed` | frees ring space on the sender |

On Portals both are a `PtlPut` into the context's persistent LE: the data write
has `length` bytes and `remote_offset = remote address`, the ack has length 0;
`hdr_data` carries the kind (5.5). The ordering hazard of section 6 applies to
the data writes.

## 5. Mapping to Portals4 (proposed, **OPEN**)

### 5.1 Interface and PTEs (implemented in sprint 1)
Non-matching NI, physical addressing (as in the SPDK non-matching branch).
- **One NI per process**, created lazily, finalised at exit (`ptl_ni.c`). The
  NIC has only 64 command queues and every `PtlNIInit` takes one (section 11).
- **Per ibv_context** (`ptl_open_device`): one EQ (65536 events), one
  whole-address-space MD for sends, **one PTE** bound to that EQ, and **one
  persistent whole-address-space LE** on it (`start=0, length=PTL_SIZE_MAX`,
  `OP_PUT|OP_GET`, no `USE_ONCE`, comm events on). RMA writes and zero-length
  sends both land on this single LE; `hdr_data` says which kind. This replaces
  the earlier plan of separate message and RMA PTEs.
- CQs are logical: `poll_cq` drains the context EQ and routes completions.
- Teardown order: LE unlink, PTE free, MD release, EQ free (same as SPDK).

### 5.2 Peer identity
`gid` (16 B) carries `nid`, `pid`, PTE indices; `dest_qp_num` is the peer qpn.
No central registry: everything the peer needs travels in tensorpipe's existing
setup-info exchange. Local qpns are allocated by the shim and must never be 0.

### 5.3 Memory registration and rkey
SPDK exposes one LE covering the whole address space and uses the remote virtual
address as the offset; `rkey` is not enforced. That is the simplest route and
we start there. Cost: no remote protection. **OPEN**: per-MR LEs if protection
matters.

### 5.4 Receives
In IB an incoming write-with-imm or send consumes an SRQ receive WR. In Portals
the target needs no posted buffer for a PUT into a persistent LE; it only gets an
event. Proposal: do **not** post one LE per receive WR (SPDK does, and pays for
it). Use one persistent, zero-length, no-`USE_ONCE` LE on the message PTE and one
persistent LE on the RMA PTE, both with comm events enabled
(`PTL_*_EVENT_COMM_DISABLE` cleared; SPDK disabled them on the RMA LE). The
event's `hdr_data` carries the imm and the sequence number. `post_srq_recv` then
becomes accounting only. **OPEN**: EQ sizing and overflow (`PTL_EQ_DROPPED`) must
replace the receive-WR back-pressure IB gives for free.

### 5.5 `hdr_data` layout (64 bit) **TBD**
Needs: message kind (write-imm vs send-imm), imm (32 bit, a length), per-QP
sequence number, and the destination qpn (the receiver must know the local qp
for `wc.qp_num`). Does not fit comfortably in 64 bits: candidates are
a 16-bit qpn + 16-bit sequence + 32-bit imm with the kind folded into a
reserved sequence bit, or putting the qpn in the PTE/match bits. Decide when the
first `post_send` is written.

### 5.6 Completions
- Send side: `PTL_EVENT_ACK` (PUT issued with `PTL_ACK_REQ`) -> `WC_RDMA_WRITE`
  or `WC_SEND`. A split write is one WR per part, so one ACK per WR.
- Receive side: `PTL_EVENT_PUT` on the RMA LE -> `WC_RECV_RDMA_WITH_IMM`;
  on the message LE -> `WC_RECV`.
- Only `PTL_EVENT_PUT` (data already placed) may be turned into a completion.

## 6. Ordering

Tensorpipe's ring protocol needs RC ordering: each `imm=length` completion means
"head += length" and the receiver then reads that region. If write B's event
precedes write A's, the receiver reads A's region before A has landed. The
hazard exists for any two data writes, even inline-size ones; inline only
removes partial placement of a single message. Acks are additive counters and
are order-insensitive.

Design: a per-QP sequence number stamped by the sender; a small per-QP
reorder buffer on the receiver delivers completions strictly in sequence. It is
empty in the in-order case. **OPEN**: window size, behaviour when a sequence
number never arrives (a lost PUT), and whether `hdr_data` has room (5.5).
Unit tests must force reordering (inject delay / swap) rather than rely on the
hardware reordering by chance.

## 7. Threading
Tensorpipe polls the CQ from a single reactor thread and posts from the same
or other threads (`post_send` is called from the reactor in `Reactor::postWrite`
but connection threads trigger it). **TBD**: which shim objects need locks.

## 8. Build and test

- Configure (borg1): `cmake -G Ninja -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  -DTP_BUILD_TESTING=ON -DTP_ENABLE_IBV=ON -DTP_BUILD_PORTALS_SHIM=ON ..`
  in `build-borg1/` (excluded from git). The policy flag is needed for the
  bundled googletest with a recent cmake.
- Run: `TP_IBV_LIBRARY=$PWD/portals_shim/libtensorpipe_portals.so
  tensorpipe/test/tensorpipe_test --gtest_filter='*Ibv*'`
- Test ladder: `portals_shim/run_tiers.sh` (stops at the first failing tier;
  `KEEP_GOING=1` runs all; `STOCK=1` runs against stock verbs as a control).
- Baseline (stock verbs, borg1): 19/19 `Ibv*` tests pass.

## 9. Test ladder

The 19 `Ibv*` tests, ordered simplest first. "Needs" is measured: each test
was run alone against the (then forwarding) shim and every `ptl_*` call it makes
was recorded (`portals_shim/test-call-matrix.txt`, measured with the forwarding shim of sprint 0). Counts are from one run and vary
(`poll_cq` is a busy-poll loop).

| Tier | Tests | New functions needed | Data path |
|---|---|---|---|
| T0 | `IbvSockaddr.*` (4) | none | none |
| T1 (**passes**) | `Context_DomainDescriptor`, `Listener_CreateAfterClosingContextAndThenGetAddress` | get/free_device_list, open/close_device, query_port, query_gid, alloc/dealloc_pd, create/destroy_cq, create/destroy_srq | `post_srq_recv` (32), `poll_cq` |
| T2 | `Context_Basics`, `Listener_Basics`, `Listener_CreateThenClose...`, `Listener_AcceptCallbacksAreQueued`, `Listener_IncomingConnectionsAreQueued`, `Connection_InitializationError` | create/destroy_qp, **modify_qp** (INIT/RTR/RTS), reg_mr, dereg_mr | still no `post_send` |
| T3 | `Connection_Initialization`, `Connection_NopWrite` | none new | first `post_send` (2): one write-with-imm + ack, tiny |
| T4 | `Connection_QueueWritesBeforeReads` (20 sends), `IbvTransportTest.QueueWrites` (6), `NopWriteWrapAround` (7) | none new | many writes on one QP: **first real exercise of ordering** (section 6), ring wrap-around |
| T5 | `IbvTransportTest.Chunking`, `ChunkingImplicitRead` (12 sends each) | none new | large payloads split into ring-sized writes; heavy `poll_cq` |

Observations that shape the implementation:
- T1 needs no QP, MR, or Portals data movement: pure bring-up. It is the right
  place to add the NI, EQ and PTE setup and the fake device.
- `get_async_event`, `ack_async_event`, `event_type_str`, `wc_status_str`,
  `post_recv` were never called by any `Ibv*` test (`wc_status_str` appears only in
  verbose logging). They stay stubs until something calls them.
- Every test creates its own contexts, QPs and MRs within one process: both ends
  of each connection are in the same process, so the first Portals traffic is
  loopback to self (same nid/pid). Cross-host (borg0 <-> borg1) needs a separate
  harness; the unit tests alone will not cover it.
- T2 is the first place the peer identity, `modify_qp` parameters and qpn
  allocation (section 5.2) have to be real.
- Beyond the `Ibv*` tests, `core/pipe_test` and channel tests may select the ibv
  transport; not yet checked (**TBD**).

### 9.1 T1 as implemented (sprint 1)

What tensorpipe does in T1: `Reactor` builds ctx, PD, CQ and SRQ, reads its own
address (`query_port`, `query_gid`), posts 32 receives per refill, and polls.
Nothing is sent. What the shim does for each step:

| Step | tensorpipe call | Shim behaviour (file) |
|---|---|---|
| find a device | `get_device_list` | tries to create the process NI (`ptl_ni.c`); returns one device `portals0`, or an empty list if Portals is unusable, so the transport reports "not viable" instead of crashing (`ptl_device.c`) |
| open it | `open_device` | allocates `ptl_ctx` (ibv_context embedded first) and builds the context's Portals resources: EQ, whole-address MD, PTE, persistent LE (5.1). Failure unwinds in reverse order and sets `errno` |
| address | `query_port`, `query_gid` | port ACTIVE, MTU 4096, **lid 0**; gid = `struct ptl_gid {"PTL4", nid, pid, pte}` |
| PD | `alloc_pd` | trivial object; counts toward the context's open-object leak check |
| CQ | `create_cq` | logical object (no Portals resource); rejects completion channels |
| SRQ | `create_srq` | ring of `max_wr` wr_ids; no Portals resource |
| refill | `post_srq_recv` | appends wr_ids; `ENOMEM` and `bad_wr` when full; FATAL for a receive with sges (tensorpipe posts none) |
| poll | `poll_cq` | `PtlEQGet` under the context lock; returns 0 when empty; any event is FATAL for now |
| teardown | `destroy_*`, `close_device` | LE unlink (retry while `PTL_IN_USE`), PTE free, MD release, EQ free; warns if a PD/CQ/SRQ is still alive |

Process NI (`ptl_ni.c`): `PtlInit`, `PtlNIInit(NO_MATCHING | PHYSICAL, PID_ANY,
NULL limits)`, `PtlGetPhysId`; finalised by `atexit`. Created once and never
re-created inside a process.

Debug: `TP_PTL_LOG=2` prints NI and context creation. Verified: T0 and T1 pass
on borg1; T1 passes 200 of 200 repetitions on borg0 and borg1, with no
warnings and no leak messages.

Known limits at this point: no remote protection (5.3); no event handling;
`ptl_ni_get` is the only global state; `get_device_list` creates the NI as a
side effect.

### 9.2 T2 analysis: QP and MR (next)

Functions: `reg_mr`, `dereg_mr`, `create_qp`, `modify_qp`, `destroy_qp`. Still
no data on the wire. What tensorpipe does (facts from `ibv.cc`,
`connection_impl.cc`):

- Per connection it creates **one QP**: `RC`, `send_cq = recv_cq = the shared
  CQ`, `srq = the shared SRQ`, `cap.max_send_wr = 2048`, `max_send_sge = 1`,
  **`sq_sig_all = 1`** (every send, including the zero-length acks, must
  complete) and registers the QP with the reactor by `qp->qp_num` straight
  after INIT, so the qpn must be non-zero and stable from `create_qp` on.
- **Four `modify_qp` calls per QP**: INIT (`STATE|PKEY_INDEX|PORT|ACCESS_FLAGS`),
  RTR (`STATE|AV|PATH_MTU|DEST_QPN|RQ_PSN|MAX_DEST_RD_ATOMIC|MIN_RNR_TIMER`),
  RTS (`STATE|SQ_PSN|TIMEOUT|RETRY_CNT|RNR_RETRY|MAX_QP_RD_ATOMIC`) and ERR at
  close. The peer address arrives in RTR: `ah_attr.is_global = 1`,
  `grh.dgid` (our `ptl_gid`) and `dest_qp_num`; `dlid` is not used because we
  report `lid = 0`.
- **Two MRs per connection** (inbox 2 MiB with `LOCAL_WRITE|REMOTE_WRITE`,
  outbox 2 MiB `LOCAL_WRITE`). `rkey` of the inbox and the inbox address are
  sent to the peer in the setup information; `lkey` of the outbox goes into
  every send sge.
- Both ends of a connection in the unit tests share one ibv_context, so QP
  lookup must work for a QP talking to a QP of the same context.

Proposed shim behaviour (**OPEN** items in section 12):

| Function | Behaviour |
|---|---|
| `reg_mr` | no Portals call (the context's LE and MD cover the whole address space and the NIC walks page tables, see 9.3). Does three things: validate the range is mapped (`mincore`, else `EFAULT`); **prefault** it best-effort (`mlock` then `munlock`; `TP_PTL_PIN=1` keeps it locked until `dereg_mr`); keep a `struct ptl_mr` (embedding `ibv_mr`: `addr`, `length`, `lkey`, `rkey`, `pd`) so `post_send` can check local sges against it. Reject length 0 and a PD of another context. `lkey = rkey = non-zero handle` |
| `dereg_mr` | removes it from the table; `munlock` only in `TP_PTL_PIN` mode; warns if the PD is gone |
| `create_qp` | allocate qpn (non-zero, **16 bit**, per-context table so the receive path can map `dest_qpn` to the QP); check `RC`, `srq != NULL`, both CQs and the PD in the same context; fill `cap` with the granted values; state RESET; keep `sq_sig_all` |
| `modify_qp` | enforce the state machine RESET -> INIT -> RTR -> RTS, any -> ERR/RESET, and the required masks for each step (return `EINVAL` otherwise, as libibverbs does). RTR stores `remote = {nid, pid, pte, qpn}` from `dgid` and `dest_qp_num` and rejects a gid without the `PTL4` magic. ERR marks the QP so later posts fail; flushing outstanding WRs belongs to T3 |
| `destroy_qp` | remove from the context's qpn table; events that arrive later for an unknown qpn are dropped with a debug message; `EBUSY` if still referenced |

Why qpn is 16 bit: `hdr_data` is 64 bit and has to carry the imm (32), the
destination qpn, a sequence number and a message kind (5.5). 2 (kind) + 16
(qpn) + 32 (imm) leaves 14 bits of sequence number, which is enough for any
realistic reorder window (needs well under 8192). A 24 bit qpn would leave 6.

Risks to retire before or in T3 (cheap experiments, not part of the unit tests). **Risks 1 and 2 are retired, see 9.3**; the probe is `probes/loopback_probe.c`:
1. **Loopback**: a PUT from the process NI to its own nid/pid/pte (all unit
   tests talk to themselves). Not exercised anywhere in the earlier work that
   I found. A standalone probe calling the shim functions directly can answer
   it in minutes.
2. **Unregistered memory** as MD/LE target: the inbox is anonymous `mmap`
   memory, untouched until written. The earlier `bigle` run used a 128 MiB
   `malloc`ed LE, which is encouraging, but a page the NIC must fault in is
   different from a touched one.
3. **ERR semantics**: tensorpipe moves the QP to ERR at close while sends may
   be in flight; IB flushes them with `WC_WR_FLUSH_ERR`. We need an answer
   before T4.

### 9.3 Memory and the NIC's page-table walk (measured, `probes/loopback_probe.c`)

BXIv2 resolves every source and destination buffer itself: it walks the
process page tables, and on a miss the **driver faults the pages in on the NIC's
behalf**. Kernel log (`bxi 0000:01:00.0`): `Fault for pid N rdonly 0
V2P_IT_CONTEXT ...`, `Failed to fault in 1 pages`. There is nothing to register,
and the shim's whole-address MD and LE (5.1) are the whole Portals-side story.
One process, loopback PUTs through the shim's own resources, borg1, kernel
4.18.0-553 (EL8), memlock unlimited:

| Case | Result |
|---|---|
| zero-length PUT with `hdr_data`, no buffers | OK; PUT, SEND and ACK events all carry what we expect, `hdr_data` intact |
| 384 B, 4 KiB, 2 MiB between touched buffers | OK, data correct |
| destination a fresh, **never touched** `mmap` (4 KiB, 2 MiB ring, middle of an untouched 1 GiB `MAP_NORESERVE`) | OK, data correct |
| destination or source pages dropped by `MADV_DONTNEED` after use | OK, data correct (the NIC does not use stale translations) |
| source never touched (zero pages) | OK, zeros delivered |
| after `fork()` (copy on write) | **OK**: translations are invalidated (v2p misses reappear after the fork); the child still reads the pre-fork byte, the parent sees the PUT. No `ibv_fork_init`-style trap |
| 2000 back-to-back 64 B PUTs to self | 2000 delivered, 0 order inversions (loopback only: says nothing about two hosts) |
| destination **unmapped** | the driver kills the process with **SIGSEGV** (`si_code 1 SEGV_MAPERR`, `V2P SIGSEGV ... si_addr`); no error event, no completion |

So **pinning is not needed for correctness**. What it changes is first-use cost.
Time to PUT 2 MiB (the size of a tensorpipe ring), loopback, 3 runs each, and
the CPU-side cost of preparing the memory:

| Preparation | PUT time | Prefault cost |
|---|---|---|
| none (untouched) | about 2100 us | none |
| `MAP_POPULATE` | about 225 us | in `mmap` |
| touch each page (`atomic or 0`) | about 224 us | about 220 us |
| `mlock` | about 225 us | about 175 us |

Faulting through the NIC costs about 5 times what faulting from the CPU costs
(about 4 us per 4 KiB page versus under 0.5 us), and all three prefault
strategies end up the same for the PUT. The NIC's own translation cache is cold
the first time (about 215 us for a touched 2 MiB buffer): not avoidable from
here.

Decision (user): **do not exercise the NIC fault path; pin.** `reg_mr` mlocks
the page-aligned range and `dereg_mr` munlocks it (implemented in `ptl_mr.c`):
- `mlock` faults the pages in on the CPU, so the NIC never takes a fault; a
  failure with ENOMEM (RLIMIT_MEMLOCK) makes `reg_mr` fail with a warning that
  names `ulimit -l` and `TP_PTL_PIN=0`; a hole in the range gives EFAULT, and a
  partial lock is undone.
- `munlock` is not reference counted, so overlapping MRs are handled by
  `dereg_mr` computing the part no other live MR covers (tested in
  `probes/mr_pin_test.c`: overlap, unaligned, hole, errors; VmLck returns to 0).
- `TP_PTL_PIN=0` degrades to prefault only (mlock then munlock).
- Pinning does not stop migration; the driver copes (fork result above).
- The MR table also lets `post_send` reject a bad local sge instead of letting
  the driver SIGSEGV this process.

New hazard: **a peer's late write into memory we already freed kills us.**
Because any in-range address is accepted by the whole-address LE and an
unresolvable fault is a process-wide SIGSEGV, a write still in flight when we
`munmap` a ring buffer (tensorpipe frees them when the connection goes away)
crashes the process. With real verbs this is a remote-access error on the
peer. Registering does not help (we cannot pin freed memory), and a per-MR LE
would not select by address in a non-matching NI (the first fitting LE on the
PTE wins). The fix has to be protocol level: do not let the buffer go while
the peer may still write, which ties into ERR and flush semantics (risk 3
above). To be examined with T4, and measured across two hosts.

### 9.4 NI limits (measured, `probes/ni_limits.c`, non-matching physical NI)

`max_pt_index 255` and `PtlPTAlloc(PTL_PT_ANY)` succeeds 256 times then returns
`PTL_PT_FULL`; `max_entries 4080`, `max_mds 1024`, `max_eqs 960`, `max_msg_size`
64 MiB, `max_volatile_size` 64. So a PTE per connection would cap a process at
about 250 connections, and an LE per connection costs entries. The design uses
one PTE and one LE per context and demultiplexes by qpn in `hdr_data`.

## 10. Sprint log

| Sprint | Content | Status |
|---|---|---|
| 0 | `TP_IBV_LIBRARY` loader, `ptl_*` forwarding shim, baseline | done, 19/19 on borg1 (forwarding bodies since replaced by stubs) |
| 0b | All `ptl_*` bodies replaced by FATAL stubs; test ladder + `run_tiers.sh` | done: T0 passes, T1+ abort at `ptl_get_device_list` |
| 1 | T1: NI/EQ/PTE bring-up, fake device, PD, CQ, SRQ | **done**: T1 passes, 200/200 repetitions on borg0 and borg1 |
| 2 | T2: QP state, `modify_qp`, MR (analysis in 9.2) | next |
| 3 | T3+T4: `post_send` (write-imm, send-imm), receive path, `poll_cq`, sequence numbers + reorder buffer, fault-injection test | planned |
| 4 | T5, cross-host borg0 <-> borg1, then pipe/channel tests | planned |

### 10.x Sprint 3 (T3-T5): the data path

`ptl_data.c`: `post_send` (RDMA_WRITE_WITH_IMM -> `PtlPut` of the sge to
`remote_offset = remote_addr`; zero-length SEND_WITH_IMM -> zero-length `PtlPut`;
both `PTL_ACK_REQ`, `user_ptr` = a malloc'd tracker), event handling and `poll_cq`.
`hdr_data = kind(2) | dest qpn(16) | imm(32) | seq(14)`. Receive side: a data
`PTL_EVENT_PUT` is delivered in sequence order through a per-QP reorder buffer (16384
slots, allocated on first out-of-order arrival); acks are delivered at once; a
delivered message waits in the SRQ backlog until a receive wr_id is posted; send
completions are `PTL_EVENT_ACK`s (possibly out of posting order, which tensorpipe
tolerates: it only counts them). One lock per context guards PtlPut, the EQ, the CQ
rings, backlogs and reorder state. `*Ibv*` passes 20/20 on borg0 and borg1, in
process (so loopback PUTs).

**Two hosts** (`tests/xhost_echo.cc`, run by `tests/run_xhost.sh`; server on one
host, client on the other, address handed over through an NFS file): echoes of 1 B to
3 MiB (the 3 MiB + 17 B message wraps the 2 MiB ring, and 2 MiB exactly lands on the
wrap point) followed by a burst of 20000 pipelined 64 B messages. 20/20 passes
(10 each way, borg0 <-> borg1), about 200k msg/s for the burst, 2 MiB echo about 2.2
ms. **No reordering was ever observed** on this path: zero early arrivals in all
runs, so the real network (one link, one hop) delivers a single flow in order. That
is an observation, not a guarantee (multiple paths, load).

**The reorder buffer is exercised by a test aid**, `TP_PTL_TEST_SWAP=1`, which
delivers data events pairwise swapped (holding one across polls). With it the
burst saw 10002 early arrivals on the server and still passed in both directions.
The in-process suite does not trigger it (one write in flight at a time).

Harness note: the server must not close before the client has read its last
message: a TCP EOF can be seen before the final data event is polled, and tensorpipe
then fails the read with `eof`. The harness has the client send a "bye".

## 11. Hardware facts from the BXI reverse-engineering work

Measured or found earlier (`/home1/private/gesalous/claude/gpu_triggered_io`,
`/home/gesalous/claude/bxi_inc_from_gpu`; binaries `bxi.ko`, `bxi-portals.ko`,
`libportals.so` there):
- **64 command queues per NIC.** Each `PtlNIInit` (a "context") takes one, with
  its own PID, TX ring, EQ and MD; the 65th fails with `PTL_NO_SPACE` (rc 6)
  (`04_multi_command_queues/README.md`). Hence one NI per process here. A node
  with many PyTorch worker processes, or other Portals jobs, can exhaust them.
- **Memory needs no registration.** The NIC walks page tables and the driver
  faults pages in; an unresolvable fault is a SIGSEGV to the process (9.3).
- **A single persistent LE works at rate.** `08_portals_rate/bigle.c`: one 128 MiB
  persistent LE, 512 B puts: about 4.0 M puts/s with no events, 1.8 M/s when the
  initiator takes send events, 1.4 M/s with send + ack events, one initiator.
  Supports the persistent-LE receive design (5.4).
- **Per-message entries lose messages.** With a pool of re-appended entries
  (`reappend_qd*.txt`) acks failed (`last_fail_type=4`) when entries were not
  re-posted in time. A persistent LE has no such window.
- Node ids: borg0 = nid 100, borg1 = nid 101 (physical NI).
- First PTE handed out by `PTL_PT_ANY` is 0 (SPDK reserves PTE 0 for its
  control plane; we have no control plane).

## 12. Open questions (summary)
0. Late peer writes into freed memory crash the process (9.3): protocol-level
   quiesce before freeing a ring; to be designed in T4.
0. T2: qpn width (16 bit proposed, 9.2) and per-context qpn table; how strictly
   `modify_qp` validates masks; whether `rkey` should encode anything.
1. rkey enforcement vs whole-address-space LE (5.3).
2. `hdr_data` layout (5.5).
3. EQ sizing / overflow policy (5.4).
4. Lost-PUT handling in the reorder buffer (6).
5. Fake-device gating: when is the Portals device "present" (4).
6. Interaction with other Portals users in the same process (NI refcount).
