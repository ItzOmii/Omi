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
heartbeat. The experiment below confirms it empirically before any doorbell is
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
4. No waiter can wake a recycled task: socket `gen`, task `tgen`, `wait_obj` and
   `TASK_BLOCKED` must all agree.
5. No task retains a stale socket registration after exit.
6. Every readiness change increments `seq` **before** waking anyone.
7. `poll()` never sleeps after observing a changed `seq`.
8. Network progress cannot depend on a ring-3 application making syscalls.

Each maps to a test in §9 except 5 and 8, which need their own: a task killed
while parked (assert the socket's slot is clear and no wake lands on the recycled
id), and the all-blocked progress test.

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
tick. Delivery no longer depends on what ring 3 is doing. Invariant 8 is still
marked uncovered, for a narrower reason than before: the pump exists and is
independent of application syscalls, but it is scheduled by the same cooperative
scheduler, whose switch points are timer-driven deadline wakes and yield sites -
a ring-3 process spinning without syscalls still delays it. Proving that needs a
preemption experiment, not another socket test.

**Verified:** `[rdy] 44/44` (12 read-path checks, including "the backend's report
woke the reader"), `[sock] 22/22`, ISO/VFS 16/16, futex 3/3, `polltest` verdict 1
(read waited 204 ticks; drained-but-connected reports 0; HUP appears and stays;
two descriptors ready in one call), `FATAL 0`, no double close, no panic.

## 11. Still open

- **Two readers on one socket.** Level-triggered wake-all permits it; concurrent
  `read()` calls on one connection are still nonsense at API level. Serialize with
  a reader bit, or leave undefined?
- **Waiter count 4** is a guess; a browser may want more (a poller plus readers).
  Cheap to raise, expensive to get wrong silently.
- **Doorbell vs switch-path pump** — decided by test 4's evidence, not before.
- **`e1000_send` tri-state** (`-2` posted-but-unconfirmed) is a prerequisite for
  any caller that retries a short write, not just for readiness. Worth doing in
  whichever milestone first retries.