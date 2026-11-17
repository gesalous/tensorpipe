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
   |  sprint 0: forwards to real libibverbs
   |  later:    Portals4 (PtlNIInit, PtlPut, PtlGet, PtlLEAppend, PtlEQGet ...)
   v
libportals -> BXIv2
```

- Selection: env var `TP_IBV_LIBRARY=<path to libtensorpipe_portals.so>`; unset
  means stock libibverbs, unchanged behaviour.
- The data-path calls (`poll_cq`, `post_send`, `post_recv`, `post_srq_recv`) are
  inline in `verbs.h` and dispatch through `ibv_context.ops`; stock libibverbs
  doesn't export them. `IbvLib` therefore loads `ptl_poll_cq` etc. as function
  pointers in override mode.
- The shim never calls `ibv_*` internally once Portals-backed (it would resolve
  to the real library). It links only libportals and libc.
- Symbol visibility: everything hidden except the `ptl_*` exports.

## 4. Verbs surface to implement

Calls used by tensorpipe (from `common/ibv_lib.h`):

| Call | Sprint-0 | Portals-backed plan |
|---|---|---|
| get_device_list / free / get_device_name | forward | one fake device ("portals0"); **TBD** gating on a BXI NI being available |
| open_device / close_device | forward | process-wide NI (refcounted) + ops table |
| query_port | forward | report ACTIVE, MTU, a fixed lid |
| query_gid | forward | gid encodes `(nid, pid, pte)`: see 5 |
| alloc_pd / dealloc_pd | forward | trivial object |
| reg_mr / dereg_mr | forward | **OPEN** see 5.3 |
| create_cq / destroy_cq | forward | a Portals EQ |
| create_srq / destroy_srq | forward | **OPEN** see 5.4 |
| create_qp / destroy_qp | forward | local state: qpn, peer identity, sequence numbers |
| modify_qp | forward | INIT/RTR/RTS: store peer `(nid,pid,pte,qpn)` from `ah_attr` + `dest_qp_num` |
| post_send | forward | `RDMA_WRITE_WITH_IMM` -> PtlPut to RMA window; `SEND_WITH_IMM` -> PtlPut to message LE |
| post_srq_recv / post_recv | forward | see 5.4 |
| poll_cq | forward | PtlEQGet -> synthesise `ibv_wc`, in per-QP order (6) |
| get_async_event / ack_async_event | forward | no events generated initially |
| event_type_str / wc_status_str | forward | local tables |

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

## 5. Mapping to Portals4 (proposed, **OPEN**)

### 5.1 Interface and PTEs
Non-matching NI, physical addressing (as in the SPDK non-matching branch).
Per process: one NI, one EQ per CQ, and two PTEs: *message* (zero-length
acks / sends) and *RMA* (the write window).

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
- `TP_PTL_TRACE=1` logs shim control-path calls and prints data-path call counts.
- Baseline (stock verbs, borg1): 19/19 `Ibv*` tests pass.

## 9. Sprint log

| Sprint | Content | Status |
|---|---|---|
| 0 | `TP_IBV_LIBRARY` loader, `ptl_*` forwarding shim, baseline | done, 19/19 on borg1 |
| 1 | Portals NI/EQ/PTE bring-up, fake device, CQ, loopback PUT/ACK in one process | planned |
| 2 | QP state, `modify_qp`, `post_send` (write-imm, send-imm) + sequence numbers | planned |
| 3 | Receive path, `poll_cq`, reorder buffer, reordering fault-injection test | planned |
| 4 | Cross-host borg0 <-> borg1, full `Ibv*` suite, then pipe/channel tests | planned |

## 10. Open questions (summary)
1. rkey enforcement vs whole-address-space LE (5.3).
2. `hdr_data` layout (5.5).
3. EQ sizing / overflow policy (5.4).
4. Lost-PUT handling in the reorder buffer (6).
5. Fake-device gating: when is the Portals device "present" (4).
6. Interaction with other Portals users in the same process (NI refcount).
