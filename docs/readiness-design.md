# Readiness: design note (rev 2, no implementation yet)

Scope: level-triggered readiness for sockets — `POLLIN`, `POLLOUT`, `POLLERR`,
`POLLHUP`, waiters attached to the endpoint object, multiple descriptors per
`poll()`. Out of scope: TCP reliability/retransmission, UDP, edge-triggered
semantics, send buffering.

## 0. Decisions carried in

- **Send ring:** deferred. `POLLOUT` gets a narrow, documented contract.
- **POLLOUT shape:** "a 1-byte send would make progress", not "the socket is open".
- **Network pumping:** independent progress mechanism required.
- **Waiter storage:** fixed slots in the socket object.
- **Waiter overflow:** explicit failure plus immediate recheck, never silent.
- **Descriptor vs waiter refs:** separate accounting; shutdown is not destruction.
- **TCP reliability:** separate milestone.

## 1. Correction to rev 1: partial writes already exist

Rev 1 claimed `tcp_conn_send` writes straight to the wire and cannot report a
short write. **That was wrong** (tcp.c:321):

```c
while (sent < len) {
  int chunk = len - sent; if (chunk > 1000) chunk = 1000;
  if (conn_seg(...) != 0) { "[tcp] short write, got N"; return sent; }  /* partial */
  c->seq += chunk; sent += chunk;
}
return sent;
```

The send side already returns *bytes accepted*, 0..len, chunked at 1000. What
does not exist is a **send buffer**: nothing is queued, so a short write is data
the caller must retry, and there is nowhere to put bytes the NIC cannot take yet.
Recording this properly changes what `POLLOUT` may promise.

## 2. The honest POLLOUT predicate

`e1000_send` (drivers/e1000.c) fails in two distinct ways:

- the target TX descriptor is not done after 100 000 spins → `-1`, nothing sent;
- after posting, completion is not signalled within 2 000 000 spins → `-1`, and
  the frame may in fact have gone out.

So `POLLOUT == c->open` is **not truthful**: an open connection can be unable to
accept even one byte because the TX ring is backed up. The honest predicate is
the condition `e1000_send` itself waits for:

```
POLLOUT  ⇔  c->open  &&  tx_ring[tx_cur].status & 1     /* next slot free */
```

That needs one accessor — `int e1000_tx_ready(void)` — and nothing else.
Documented provisional contract:

> `POLLOUT` means a subsequent `send()` of at least one byte will make progress,
> not that any amount will be accepted. There is no send buffer; a short write
> returns the accepted count and the caller must retry the remainder.

Consequences accepted by deferring the ring: queue-full behaviour, write-side
wakeups when the ring drains, ordering against retransmission, and teardown with
pending data are all absent. None may be papered over later by making `POLLOUT`
unconditional.

**Readiness is a hint, not a reservation.** Another sender can take the TX slot
between the readiness check and the send. `send()` must still handle a short
write or `-EAGAIN` and never assume the capacity survived the check — ordinary
readiness semantics, not a defect.

**Ambiguous completion must never become a retransmission.** `e1000_send` returns
the same `-1` for two different failures: "descriptor never became free, nothing
was transmitted" and "frame posted, completion not observed within the timeout".
`conn_seg` (tcp.c:162) collapses both into its return, and `tcp_conn_send` turns
that into a short write. A caller retrying the remainder would then re-send bytes
that may already be on the wire — a duplicate segment carrying the same sequence
number.

So `e1000_send` needs three outcomes: `0` sent, `-1` definitely not sent (nothing
posted), `-2` posted but unconfirmed. Only `-1` may be retried immediately; `-2`
must be reported as accepted. Resending after ambiguity belongs to the TCP
reliability milestone, where it is driven by an unacknowledged sequence number
rather than by a driver timeout.

## 3. Network progress: what actually exists

Evidence, because the architecture choice depends on it:

- **No NIC interrupt is used.** Only two IDT gates are installed — 0x20 (PIT) and
  0x81 (yield), kernel_task.c:432-433 — and e1000.c never writes an
  interrupt-mask register. RX is polled: `e1000_poll_recv`, called from
  `tcp_pump` (tcp.c:272) and legacy loops (tcp.c:514, net.c:167/243/352,
  e1000.c:413/462).
- **`tcp_pump` runs from two places only**: `tcp_conn_open`'s connect loop and
  `tcp_conn_recv`'s spin loop. Nothing pumps on a schedule.
- **Kernel tasks are a supported shape**: `task_spawn(name, fn)` primes a ring-0
  task (`cs = 0x08`), `k_task_entry` calls `fn` then loops on `task_yield`, and
  `task_sleep_ticks` + `wake_deadlines` give a real sleep/wake. `wake_deadlines`
  runs in `sched_tick` **before** the `cs & 3` gate, so a sleeping kernel task is
  marked READY by the timer whatever was interrupted.
- **A kernel task is never preempted** (`sched_tick` returns immediately for
  `cs & 3 == 0`), so it runs only when something switches. The kernel main task
  runs `gui_main_loop`, which calls `task_yield()` once per frame
  (compositor.c:5995) — in practice, the scheduling heartbeat.

**Recommendation.** A dedicated pump task, created with `task_spawn`, looping
`pump-all-sockets; task_sleep_ticks(1)`, and only while at least one socket
exists. All packet processing stays in process context — no work in IRQ, which is
the constraint that rules out pumping from the timer.

Honest caveat: with no NIC interrupt, progress still rides on *some* switch
happening, and today that is guaranteed only by the compositor yielding every
frame. That is an implicit dependency on the UI loop, not a network guarantee.
Two ways to remove it, in order of cost:

1. **NIC RX interrupt as a doorbell** (recommended follow-up): wire the e1000 IRQ
   so the ISR only sets a flag and wakes the pump task; all ring and TCP work
   stays in the pump. Small, and it removes the dependency entirely — a blocked
   reader's frames get processed even if every other task is asleep.
2. **Pump from the switch path** (fallback, free): call the pump from the yield
   path when any socket has waiters. Cheap, but puts packet work inside a gate
   with interrupts cleared — the thing worth avoiding.

Milestone 1 must contain the test that distinguishes these: **all ring-3 tasks
blocked except the writer, reader waiting, no application syscalls at all** — the
reader must still wake. If it only wakes because the compositor yielded, the test
says so and the doorbell becomes mandatory rather than optional.

Do not ship sleeping readers on the pump task until that test passes.

**Survey first: is there an existing mechanism?** Checked, because installing a
doorbell just to make a test pass would be solving the symptom. `pick_next` always
returns task 0 (the kernel main task) when nothing else is ready, so the system is
never "idle" in the hlt sense; `sched_tick` only switches when it interrupted ring
3; a kernel task is never preempted; and the only other voluntary switch points
are `task_yield` calls from the compositor loop and from yield/sleep syscalls.
That is the whole inventory — so as of today there is **no** mechanism providing
network progress independent of ring-3 activity, and the compositor is the sole
heartbeat. Note that the compositor is in ring 3 now (`userland/comp.c`): while it
owns the screen the kernel's own compositor loop returns early, so the heartbeat is
the display server's sleep between frames, which is still a voluntary switch. The experiment below confirms it empirically before any doorbell is
built; the doorbell is then justified by evidence rather than by a hunch.

Note that milestone 1 does not depend on this: with the fake backend the writer
task flips readiness directly, so no pump is needed to prove the wait protocol.

## 4. Waiting: the protocol

`task_block_current()` (kernel_task.c:301) is `state = BLOCKED; task_yield()`, and
`task_wake(id)` only wakes a task already BLOCKED — so a wake between the
readiness check and the block is dropped. Not atomic, and not fixable in the
scheduler alone.

The futex path (ring3.c:522) already holds the resolution: **publish, re-read,
block**, with the waker clearing the publication. It is safe on this kernel
because IRQ0 preempts only ring 3, so no task runs mid-syscall; the re-read makes
that a local argument rather than a scheduler one.

Futex re-reads a *user* word the waker also writes. Socket readiness is
kernel-owned, so the socket carries `uint32_t seq`, bumped on **every**
readiness-relevant transition **before** any wake, and the waiter snapshots and
re-reads it.

```c
for (;;) {
  uint32_t s = sock->seq;
  unsigned r = sock_poll_now(sock);            /* backend poll: never blocks */
  if (r & want) return r;
  if (r & (OMI_POLLERR | OMI_POLLHUP)) return r;

  int slot = waiter_register(sock, task_current(), sock->gen);   /* publish */
  if (slot < 0) { /* exhaustion: explicit failure, see §7 */ }
  if (sock->seq != s || (sock_poll_now(sock) & want)) {          /* re-read */
    waiter_clear(sock, slot); continue;
  }
  task_block_current();                        /* only now sleep */
  waiter_clear(sock, slot);
}
```

Wakers (tcp.c:203 RST, :217 data, :226 FIN, and socket shutdown) do
`seq++; wake_all(mask)`. Level-triggered: waking everyone is correct — one reader
takes the bytes, the rest re-check and re-block.

`fd_poll(t, fd)` resolves fd → ofile → socket per call, so dups are two
descriptors on one object and one wake; a future `poll(fds[], n)` registers one
waiter slot per distinct socket with the union of masks. Waiters key on the
object, never on an fd integer.

## 5. Lifetime: descriptors, pins, and what each one protects

Rev 1 got this wrong twice. First: a parked reader holding a reference prevents
`refs` from reaching zero, so the final unref that would wake it never runs — a
circular dependency and a permanent hang. Second: a pin cannot mean "sleeping
waiter", because a thread already *inside* `recv()`/`send()` in the backend is
also using the transport without holding a descriptor, and the last `fd_close`
can land while it is in there.

So a pin covers **any** task inside a backend operation — parked on readiness or
executing a call — acquired before the first backend access, released after the
last.

| State | Permitted |
|---|---|
| `refs > 0` | descriptor access; new waits allowed |
| `refs == 0` | shutdown: `closing = 1`, `seq++`, waiters woken `POLLERR`; no new waits, no new backend calls from descriptors |
| `pins > 0` | object and transport stay alive, even at `refs == 0` |
| `refs == 0 && pins == 0` | destroy exactly once — the only path that calls `sock_close` |

```
ofile_unref():  refs--;  if (refs == 0) ofile_shutdown(o);
ofile_unpin():  pins--;  if (pins == 0 && refs == 0) ofile_destroy(o);

ofile_shutdown(o):   /* idempotent, guarded by o->closing */
  o->closing = 1;
  sock->seq++; sock_wake_all(POLLERR);   /* parked readers leave with an error */
  /* transport stays alive: a waiter may still be inside a backend call */

ofile_destroy(o):    /* only when refs == 0 && pins == 0 */
  sock_close(o->sock);                   /* exactly one backend destruction */
```

Invariant that follows: **no backend operation can outlive its pin**, so no
backend call ever executes against a destroyed transport. `read()`/`write()`
refuse to *start* when `closing` is set, checked inside the same snapshot as
`seq`, so it cannot slip between snapshot and block.

This keeps the extraction's rule intact: lifetime accounting lives in the ofile
layer, `socket_ops` never counts references, and it still performs exactly one
close.

## 6. Waiter storage and task-exit safety

A waiter node on the blocked task's kernel stack is a use-after-free if that task
is killed — the address space is freed, the stack with it, and the socket's list
would point into freed memory. So: fixed slots in the object.

```c
typedef struct { int task; uint32_t gen; } sock_waiter_t;   /* 4 per socket */
```

`gen` guards **object** reuse: `conns[]` is a static pool, so a released socket's
memory becomes the next connection, and a waiter holding only a pointer would
wake on a stranger's socket.

`gen` does *not* guard **task id** reuse. Rev 1 said task ids are recycled by
`task_alloc`; that was wrong — `task_alloc` appends (`tasks[ntasks++]`), so ids
are never reused today and there is no currently exploitable bug. `tgen` is kept
as prophylaxis for the day task slots are freed and recycled, and it is described
as such rather than as a fix for a live failure.

`task_t` grew `uint32_t tgen`, bumped on allocation from a monotonic counter. Registration is then two-sided publication, the
same shape as futex:

- the socket slot records `{task, gen, tgen}`;
- the task records `wait_obj = sock` (new `task_t` field beside `wait_addr`);
- `wake_all` wakes a slot only if the socket's `gen` matches, the task's `tgen`
  matches, `tasks[id].wait_obj == sock`, and that task is `TASK_BLOCKED`;
- task exit clears its own side (`mm_on_exit` / `task_kill_current`), so a
  registration can never outlive its task record.

Four conditions rather than one is deliberate: each catches a different way
identity can be wrong — object reuse, task reuse, a task that already left the
wait path, and a task that is running rather than parked.

Belt and braces: a parked task also carries `wake_at`, so a missed wake costs
ticks instead of a hang — the same belt futex already relies on.

## 7. Waiter overflow

4 slots per socket, with defined behaviour when full: `waiter_register` returns
`-EAGAIN`, the caller **rechecks readiness immediately** rather than blocking, and
if still not ready it reports resource exhaustion (`-ENOBUFS`) to userspace
instead of sleeping. Never a silent drop, never a task parked with no slot.

Permanent test: fill every slot with parked readers, then attempt a fifth and
assert a defined error — not a hang, not a lost waiter.

## 8. EOF, close, error

- FIN/RST → `peer_closed`: endpoint alive, `read()` returns 0, `poll()` reports
  `POLLIN|POLLHUP`. Readable EOF, not an error.
- Last descriptor → shutdown: waiters get `POLLERR`, transport alive until the
  last pin drops, then destroyed exactly once.
- Stale waiter (gen mismatch or `wait_obj` mismatch): `POLLERR`, never re-block.

The current recv mapping is lossy and should disappear with poll-based waiting:
`r == 0` (timeout) becomes `-EAGAIN`, `r < 0` (peer closed) becomes `0` (EOF).
Once a caller can *wait* on readiness, `tcp_conn_recv` should not carry a timeout
at all.

## 9. Milestone 1

One socket, one blocked reader, one writer — fake backend first (it flips
readiness on command), then TCP.

1. reader blocks, writer sends, reader returns exactly those bytes; wake count 1.
2. readiness flipped *between* snapshot and block: the re-read catches it and the
   reader never sleeps. Fully deterministic.
3. re-read disabled on purpose: the deadline belt wakes it and the test prints
   `LOST-WAKE` rather than hanging. A test that can only pass by not hanging
   proves nothing.
4. all other ring-3 tasks blocked, no application syscalls: the reader still
   wakes — the independent-progress test from §3.
5. FIN → `read()` returns 0 with `POLLIN|POLLHUP`, socket still alive.
6. last-descriptor close with a parked reader → `POLLERR`, exactly one backend
   destruction, which happens only after the reader leaves the wait path.
7. dup: reader parked via one descriptor, the other closed, reader unaffected.
8. slot exhaustion → defined error.

`poll(fds[], n)` across descriptor kinds is written only after these pass.

## 9b. Milestone 1: implemented, fake backend only

`kernel/fs/readiness_selftest.c`, 32 checks, all green alongside the untouched
suites (`iso` 9/9, `vfs` 16/16, `futex` 3/3, `[sock]` 22/22, `FATAL 0`, no
double close). What it establishes:

| Check | Result |
|---|---|
| block, notify, wake, read the payload | one wake, payload consumed, no leftover registration |
| event lands after registration, before the block | reader never sleeps; counted spurious |
| same, with the re-check disabled | wake genuinely lost, deadline belt returns -EAGAIN |
| last descriptor closes while a backend call is pinned | POLLERR to the reader, transport alive until unpin, destroyed once |
| task exits while registered | registration unlinked by the exit path |
| socket slot reused | stale generation dropped, not woken |
| waiter table full | `-ENOBUFS`, filled slots untouched |

Things the implementation forced, which the design note had left implicit:

- **The read path must pin.** The first version of the close test parked a reader
  on an *unpinned* description; the close then destroyed the transport underneath
  it and the reader saw `POLLERR|POLLHUP` from a dead slot instead of a clean
  `POLLERR` wake. That is not a bug in the protocol — it is the protocol telling
  you a wait without a pin is illegitimate. `fd_read` must pin around
  `sock_wait`.
- **`task_sleep_ticks` refuses task 0** (`cur <= 0` returns immediately), so a
  test running in the boot task cannot sleep; it has to yield and watch
  `task_total_ticks()`. The first run silently ran zero worker iterations and
  looked like a protocol failure.
- **The fake backend must report `open`,** or every poll returns `POLLERR` and
  `sock_wait` returns immediately on the terminal bits — the test then proves
  nothing while appearing to run.

Still uncovered, as required: invariant 8 (network progress independent of ring-3
activity) — it needs the pump task and a real NIC, so it belongs to the next
milestone. Invariant 5 now has its test (task exit while registered).

## 10. Invariants the implementation must hold

Stated so they can be tested individually rather than assumed collectively:

1. No backend operation can outlive its pin.
2. Shutdown is idempotent — repeated `ofile_shutdown` changes nothing.
3. Destruction occurs exactly once, and only at `refs == 0 && pins == 0`.
4. No waiter can wake a task that is no longer the one that registered: socket
   `gen`, task `tgen`, `wait_obj` and `TASK_BLOCKED` must all agree.
5. No task retains a stale socket registration after exit.
6. Every readiness change increments `seq` **before** waking anyone.
7. `poll()` never sleeps after observing a changed `seq`.
8. Network progress cannot depend on a ring-3 application making syscalls.

Each maps to a test in §9 except 5 and 8, which needed their own: a task killed
while parked (assert the socket's slot is clear, and that a wake cannot land on an
exited task or on a subsequently reused task slot), and the all-blocked progress
test. Both now exist; invariant 8's test is in
`kernel/fs/preempt_selftest.c` and is described in §10b.

## 10b. Milestone 2: the syscall side, and two bugs it found

Implemented on top of `d014993`, fake-backend tests kept green.

**What was added.** `fd_read` now has a real wait: it pins the description for the
whole operation, attempts the backend read with no patience, and only enters
`sock_wait` on `-EAGAIN`. Positive is bytes, `0` is EOF, `-EAGAIN` means the
deadline expired (the caller's own timeout), and every other negative propagates
untouched - so `-EAGAIN` from the would-block attempt can never reach a caller as
a read failure. Partial data returns immediately; the loop never waits to fill the
requested buffer. `SYS_POLL` (33) takes `{int fd; uint16 events; uint16 revents}`,
computes every descriptor's readiness, and paces on a tick rather than registering
interest in several objects - `wait_obj` is one pointer, so a multi-object wait is
a scheduler change, not a socket one. `polltest.elf` is the ring-3 caller.

**Bug 1: the fake backend made the tests look finished.** With no way to say "no
data yet, ask again", `fk_recv` returned `-1` (EOF) for an empty stream, so a read
that should have parked returned 0 immediately. The fake now models the three
outcomes separately - bytes, would-block, EOF - because a fake that conflates the
last two cannot test a wait at all.

**Bug 2: nothing ever notified the socket.** `sock_notify` had exactly one caller:
the test injector. The 32 milestone-1 checks passed because they called it by
hand. Real traffic never did, so a parked reader could only be woken by its own
deadline - measured, not theorised: a read that should have returned after the
server's 2 s delay returned after 500 ticks (the whole belt). Fix: `socket_ops`
gained `bind(handle, cb, ctx)`, the socket layer binds `sock_ready_cb`, and the
TCP path reports readiness at four transitions (RST, handshake, data, FIN). The
backend reports a mask and never learns what the ctx is; the layer above never
learns what the handle is. The same read then returned at 204 ticks.

**Bug 3: `revents` was written at offset +4.** That is `events`, so poll came back
"ready, no bits" and the caller's `events` was silently destroyed. Fixed with
`_Static_assert(offsetof(kpollfd_t, revents) == 6)`, and `polltest` now asserts the
specific bits and that `events` survives the call.

**Independent progress.** A task sleeping in a wait cannot drain the NIC for
itself, and that stopped being theoretical the moment reads slept: the 8899
clients timed out while the server's log showed the responses sent. `netpump` is
now a kernel task that calls `tcp_service()` (one frame, then out) and sleeps a
tick. Delivery no longer depends on what ring 3 is doing.

**Invariant 8: covered, and the experiment changed the answer.** The design note
previously assumed the scheduler only switched at yield sites, so a syscall-free
ring-3 loop would starve the pump. That was wrong: `sched_tick()` - the IRQ0 stub
- preempts whenever the interrupted frame is CPL 3 (`(frame->cs & 3) == 3`). Ring 3
has been preemptible all along; the non-preemptible case is a kernel frame.

`kernel/fs/preempt_selftest.c` separates the two claims the invariant combines,
because a tick is not a switch:

| window | ticks | probe runs | pump frames |
|---|---|---|---|
| quiet, 30 ticks | 30 | 30 | 30 |
| ring-3 `spin.elf` running, no syscalls | 31 | 31 | 31 |

The probe is a kernel task that increments a counter and sleeps a tick, so it can
only advance if the scheduler runs it. `spin.elf` loops on its own arithmetic and
never traps, yields or sleeps - so nothing in that window can be credited to the
application. The tick count proves the timer fired; the probe and pump counts prove
the scheduler switched, which is the distinction that matters. The spinner is
bounded by a loop count rather than the clock, because a test that can hang the
machine would be worse than the bug it looks for. Six checks, all passing; the
window is asserted non-empty first, so "the probe advanced" cannot describe an
idle machine.

The remaining limitation is a different one, and is not invariant 8: a *kernel*
task that spins forever is still never preempted, so it would hang the system. No
kernel code does that today; the pump and the probe both sleep.

**Verified:** `[rdy] 44/44` (12 read-path checks, including "the backend's report
woke the reader"), `[pre] 6/6`, `[sock] 22/22`, ISO/VFS 16/16, futex 3/3,
`polltest` verdict 1 (read waited 204 ticks; drained-but-connected reports 0; HUP
appears and stays; two descriptors ready in one call), `FATAL 0`, no double close,
no panic.

One lesson from `polltest` is worth keeping: its original two-descriptor check
polled both while one of them was already permanently readable, so every retry
returned instantly and the test hammered the kernel for eight seconds while
claiming to wait. It now asks each descriptor separately first - which is also how
it distinguishes "poll is wrong" from "this connection never delivered" - and the
combined call asserts only that the ready count equals the number of descriptors
reporting bits.

## 10b-bis — capacity and the unconfirmed send

Two loose ends from the socket layer, closed.

**Waiter slots: four was too few.** The limit was sized for "one reader and a
writer", which is true of one thread and false of a program that polls from a
helper task or has two writers. Raised to eight (24 sockets x 8 x 32 B = 6 KB of
BSS). The refusal path is unchanged and still explicit: the ninth waiter gets
`-ENOBUFS`, `stats.overflows` counts it, and the eight existing registrations are
left intact — the exhaustion check asserts all three, so "we have more slots" did
not quietly become "we block forever instead of refusing".

**The `-2` send outcome had a policy and no test.** It was documented as "posted
but unconfirmed, treat as sent", and it is the one branch whose wrong answer is
silent duplicate bytes on the wire — so it is now driven directly, with `e1000`'s
return value injected through `e1000_test_send_outcome()` and the number of send
attempts counted rather than assumed. The injection is one-shot on purpose: a
sticky override makes the retry it is supposed to detect invisible.

`[tcp] 6 checks, 6 ok` asserts, for `-2`: reported as sent, sequence advanced by
exactly the bytes, and exactly one attempt. For `-1`: nothing counted, sequence
untouched, exactly one attempt — the retry is the caller's decision, not a loop
inside `tcp_conn_send`.

## 10d — pipes

The last descriptor type step 4 was missing. A pipe is a 4 KB ring with two
counts: open read ends and open write ends. The bytes are the easy part; every
decision that matters is about a condition that cannot clear itself.

**Four outcomes, and none of them are "spin".** Empty with a writer alive waits.
Empty with no writers is `0` — EOF, the thing that ends a pipeline, not an
error. A write with no reader is `-EPIPE` immediately: waiting there would be
waiting for a condition that can never change. A write into a full pipe waits,
and once any room appears it returns a *partial* write rather than filling the
caller's whole buffer — the same contract the socket path uses.

**Readiness is part of the contract, not a nicety.** A closed write end makes the
read end readable — `read` returns 0 at once — so `poll` reports `POLLIN |
POLLHUP`. Without that, a poller waits for data that is never coming. A pipe
whose reader has gone reports `POLLERR` on the write end for the same reason.

**Waiting is a one-tick sleep, not a waiter table.** This is the deliberate
compromise and it is worth naming. The socket layer has a real waiter table with
two-sided generation checks, because a socket wait spans a transport that can
complete out of band. Pipes do not need sub-tick latency, so they get the
version that cannot lose a wake, cannot hold a stale task reference, and cannot
wake into an empty buffer without noticing — it re-checks the condition after
every sleep. The price is one wake per idle tick, and up to a tick of latency.
If something later needs better, this becomes a waitqueue with the socket's
generation discipline; not before.

**Each open end is counted once per descriptor object, not per descriptor.** A
`dup` shares the description, so it shares a single count: closing the original
while the dup lives must not produce EOF, and closing the dup last must. That is
asserted from ring 3, because it is exactly the case where a naive refcount
passes the kernel test and fails a real program.

**A slot is released only when both sides are gone**, so a writer closing cannot
free the ring a reader still points into.

### Tests

`[pipe] 37/37` (kernel) covers round trip, partial reads, full-pipe refusal,
`-EAGAIN` on deadline, EOF, `-EPIPE`, slot release, an empty-pool check, and one
real blocked read woken by a second task. Wraparound gets its own pipe driven to
the one position that matters: both pointers at CAP-8, then 12 bytes so the last
four land at the start of the buffer.

`[pipe3] 30/30` (ring 3, through `SYS_PIPE`) proves the parts that only exist for
a program: two usable descriptors, `EBADF` for the wrong end, poll truthfulness
on an empty pipe, FIFO order across a short read, EOF across a `dup`-and-close in
both directions, `-EPIPE` with a closed reader, and `EBADF` after close.

### What the suite caught

Adding one boot-time program pushed the task table over `TASK_MAX`, and the
symptom was two *descriptor* checks failing ("a thread inherits the descriptor
table", "closing the duplicate leaves the original open") — because a slot that
cannot be allocated fails as "thread creation failed" and then reads as an fd
bug. Task slots are never reclaimed: an exited process keeps its entry until
reboot, so every program spawned at boot is permanent cost. Raised 32 → 40 with
the reason recorded where the constant lives, rather than leaving the next person
to rediscover it from a misleading failure.

## 10c. Milestone 3: writes, and the send outcome that must not be guessed

The read rule, mirrored. `fd_write_ticks` pins the description for the whole
operation; `sock_write_wait` sends once, and only a send that accepts **nothing**
enters `sock_wait(POLLOUT)`. Bytes accepted return immediately, even when fewer
than asked: `write()` reports what it took and the caller decides whether to
continue. A hangup or error ends the call rather than being waited out - no amount
of patience makes room in a connection that is gone. Default patience is 500 ticks,
same as reads.

**The part that had to change first: `e1000_send` was lying by omission.** It
returned `-1` for two unrelated situations - the ring was stuck before anything was
posted, and the frame was posted but its completion bit never came back. Only the
first is safe to retry; retrying the second duplicates bytes on the far side, and
TCP will happily deliver both copies to an application. It now has three outcomes:

- `0` - completion seen, the frame is on the wire
- `-1` - nothing was posted, retrying is safe
- `-2` - posted, unconfirmed: **treat as sent**

`tcp_conn_send` counts a `-2` chunk as accepted, advances `seq`, logs
`tx unconfirmed`, and never resends it. A hole it cannot detect is better than a
duplication it cannot undo.

**What is tested:** partial write returns at once without waiting; a full pipe
parks the writer exactly once and wakes on `POLLOUT` through the bound backend
callback; an expired deadline reports `-EAGAIN`; a hangup turns a blocked write into
`-EPIPE` instead of a wait. Six checks, `[rdy] 51/51` overall.

**What is not tested, and is marked so here:** the `-2` path. Producing it means
making a NIC fail to post a completion bit, which needs fault injection below
`e1000_send`; there is no seam for that today, and pretending a fake backend can
reach it would test the wrong layer. The policy is written down and the code path
is three lines, but it has not been executed, and no test claims otherwise.

Two fake-backend defects surfaced while writing these checks, both the same
mistake as the earlier `open = 0` one - a fake that does not model a state the
real backend can be in:

- `fk_poll` reported `POLLOUT` unconditionally, so a full pipe looked writable and
  the writer never parked. `POLLOUT` means "a 1-byte send would make progress".
- `fake_reset()` cleared the bound readiness callback while the socket was still
  bound to it, so the test's next report jumped to address 0 and took the kernel
  down. The registration belongs to the socket; a fake forgetting it models a
  transition that cannot happen.

## 10d. Milestone 4: one reader at a time

Two tasks could previously park a blocking read on the same socket. Level-triggered
wake-all makes that *permitted*, which is the problem: both wake for the same bytes
and split a byte stream in whatever order the scheduler picked. Nothing corrupts,
but no caller can reason about which read gets which bytes, and a protocol on top of
it would be guessing.

**Decision: one blocking read per socket, refused rather than queued.** A second
concurrent blocking read returns `-EBUSY` immediately. Queueing would need a
fairness policy and a second waiter table for readers; refusing is a rule a caller
can hold in its head, and a caller that genuinely wants to share a stream can `dup()`
and coordinate, which is a decision it should be making explicitly.

The mechanism is an owner field, not a flag: `read_owner` holds the task id. A bare
boolean would leak on the one path that matters - a task killed while parked inside
the read leaves the socket unreadable for ever, with nothing to point at. Ownership
is therefore released in three places: every normal exit from the read loop
(including `-EAGAIN`, EOF and errors), socket allocation, and `sock_drop_waiters()`
- the same sweep that already withdraws a dead task's waiter registrations.

Deliberately *not* excluded: a non-blocking or partial read racing a parked reader.
`read()` reports what it took, in whatever order calls arrive; that is ordinary byte
stream behaviour. What is excluded is two tasks sleeping on the same object and being
woken for the same transition.

Five checks: the first reader parks; a second read gets `-EBUSY` rather than parking;
the parked reader still receives its bytes; the read side is free again once that
read returns; and a task that acquires the read side and dies inside it releases it
by the same sweep that clears its waiters.

## 11. Still open

- **Waiter count 4** is a guess; a browser may want more (a poller plus readers).
  Cheap to raise, expensive to get wrong silently.
- **Doorbell vs switch-path pump** — decided by test 4's evidence, not before.
- **`e1000_send` tri-state** is implemented (§10c), but its `-2` outcome is
  unexercised: it needs fault injection below the NIC driver. Until then the policy
  is a decision recorded in code, not a behaviour observed under test.
## 12. The syscall filter, and what it is not

The sandbox starts with the one piece that is cheap and is the foundation of the
rest: the door. `SYS_SANDBOX 55` takes a bitmap of syscall numbers - 256 of them in
four words, one bit per number - and from then on `syscall_dispatch` checks it
before it looks at the number. A call outside the mask does not fail, it ends the
task, at code 159 (what an uncaught `SIGSYS` does on Linux), for the same reason
`kill_req` is checked in that spot: between calls is the one point where ending a
task leaves nothing half-torn.

Three decisions worth recording, because each could reasonably have gone another
way:

- **Per task, not per address space.** seccomp is per thread, and that is not an
  accident: the mask is a property of the code that is running. A process that
  confined itself and then forked or made a thread would otherwise hand the new
  task an unfiltered door, which is the entire escape the mask exists to close, so
  `task_spawn_clone` copies it. `SYS_EXIT` is exempt in the check itself rather
  than in every mask: a sandboxed process has to be able to die.
- **Narrow only.** A second install ANDs with the first. Without it, "install a
  filter" would be a formality a process could undo one call later - and the test
  that proves otherwise (mask, then `~0ULL`, then a forbidden call) is the one that
  would otherwise pass.
- **The Linux ABI is the violation.** The filter is written against this kernel's
  numbers, and a confined task calling through the `syscall` instruction would be
  making the same call under a number the mask never saw. Refusing the door is
  honest - one filtered door is the point - and it is why a musl-linked binary
  cannot run under a filter yet. When that matters, the fix is a second bitmap over
  Linux numbers, not a translation table pretending the two numberings are one.

What it is not: a capability model. A permitted `write` writes to any descriptor
the task already holds, `mmap` is unconstrained, and there are no namespaces, so the
filter is exactly as strong as the handles a process holds at the moment it
confines itself. Handing a renderer a socket and then filtering `socket()` is the
shape a real sandbox needs, and that is the next piece, not this one.

`TASK_MAX` went from 96 to 256 in the same change, and not speculatively: a
sixteen-core boot spends 31 slots on per-CPU tasks, and one new test with six
children in it was enough to starve `lives` at four cores - which showed up as a
"broken" shared page rather than as a failed fork. The budget shape is still wrong
(a cap for the life of the boot, not a live count); raising it removes the symptom
without fixing that.
