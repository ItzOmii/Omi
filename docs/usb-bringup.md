# USB storage: where it stands

Goal: a USB stick visible in Omi's Files window, read-only, attached to QEMU as an
image copy rather than a physical device.

## What is in place

- **The rig.** QEMU has no xHCI controller by default, so every boot reported
  `[xHCI] Found 0 controllers` and the driver was dead code. With:

  ```
  -device qemu-xhci \
  -drive if=none,id=stick,file=/tmp/stick.img,format=raw \
  -device usb-storage,drive=stick
  ```

  the controller is found and a device appears on port 0.

- **A synthetic stick.** `/tmp/stick.img` is a 64 MB FAT32 volume labelled
  `OMISTICK` with `README.TXT` and `a.txt`, built with `mkfs.vfat -F 32` + `mcopy`.
  Deliberately small: the layer under test is the driver, not the capacity, and a
  small image makes a wrong sector number obvious.

- **Identification before configuration.** `xhci_scan_storage()` walks the
  configuration descriptor for an interface with class 8 (mass storage, subclass 6,
  protocol 0x50 bulk-only) and records both bulk endpoints. The HID path must not
  run on a disk: it calls `Set_Protocol` on interface 0, a storage device STALLs
  it, and the probe then fails for a reason unrelated to the real one.
  `xhci_setup_keyboard()` returns `-2` for "enumerated, but not a keyboard", which
  the caller reports separately from a failure.

## Bugs fixed in this round (all confirmed by the trace, not by reasoning)

1. **Command TRBs carried no Slot ID.** dword3 bits 31:24 must name the target
   slot; Enable Slot is the one command that requires 0 there, which is why it
   worked while Address Device, Evaluate Context and Configure Endpoint all came
   back `CC_TRB_ERROR` (5). Adding the slot took Address Device from cc=5 to cc=1.
2. **Setup Stage used the wrong flag bit.** Bit 5 (IOC) has no meaning on a Setup
   TRB; bit 6 (IDT) is what says the eight setup bytes are in the TRB. With IOC
   set, the controller accepted the ring and then never advanced it.
3. **TRT was on the wrong TRB** (and set to 1, a reserved value). TRT lives in the
   Setup TRB's bits 17:16: 2 for IN, 3 for OUT. It is now put there.
4. **EP0's max packet size was 8 for a USB3 device.** A SuperSpeed device runs EP0
   at 512 from the first SETUP; at 8 the transfer never completes and there is no
   error to see. MPS now follows the port speed.
5. **ERDP was written with bit 0 set.** EHB is bit 3; bit 0 is reserved.

After 1-5 the controller does address the device (`usb_xhci_slot_address slotid 1,
port 1`), enables EP0, and runs control transfers: the trace shows `TR_SETUP`,
`TR_DATA`, `TR_STATUS` and `usb_xhci_xfer_success len 8`.

## What blocks it now

The driver never consumes a single transfer event, and the boot stops after
`address cc=1` - so it is stuck where the descriptors should arrive.

Measured, one boot: 6 control transfers issued, **432** `xfer_success` in the
trace, and the driver logging exactly 3 events (port status, Enable Slot
completion, Address Device completion) and **zero** "pop mismatch" lines. Those
two facts together mean `xhci_pop_event()` is not being reached at all, rather
than being reached and refusing the event - so the wait loop is not the thing to
stare at first.

Next, in order:

1. Put a counter inside `xhci_wait_for_transfer()` that prints every 1000
   iterations. That separates "never called" from "spinning with events unread",
   which the current evidence cannot distinguish.
2. Drain every pending event per wait instead of returning on the first one that
   matches. 432 completions for 6 transfers says the ring is producing far more
   events than the driver consumes, and a wait that handles one per call will fall
   behind until it wraps.
3. Re-check ERDP/cycle bookkeeping against that many events. The consumer index
   and the producer index must agree event-for-event, so if anything over- or
   under-counts, the very next pop sees the wrong cycle bit and silently refuses.

## What blocks it (previous round, kept for the record)

`Enable Slot` never completes. There is no Command Completion event, so no
descriptors are fetched, so the device is never addressed. The boot continues after
the timeout, which is why this looks like silence rather than a hang.

Fixed on the way through, and worth keeping: the command ring, event ring and EP0
ring were allocated 16-byte aligned. CRCR's low six bits are reserved and masked by
the hardware, so the controller was being handed a base address that was not the
ring; the driver also read phantom "events" (transfer-event-looking garbage) out of
an event ring nothing had written. 64-byte alignment removed the phantom events and
changed nothing else - the completion still does not arrive.

## What the trace showed (and where the driver's story disagrees)

Booting with `-trace enable="usb_xhci_*" -D /tmp/xhci-trace.txt` moves the question
from "why is it silent" to "which layer is lying":

- `ENABLE_SLOT` completes with `CC_SUCCESS` and enables slot 1. It always worked;
  the driver's silence hid that.
- `ADDRESS_DEVICE` traces as `usb_xhci_slot_address slotid 1, port 1` followed by
  `usb_xhci_ep_enable slotid 1, epid 1` - the controller *did* address the device
  and enable EP0. Meanwhile the driver logs `address cc=0x5` (TRB Error) for that
  same attempt. One of those two is wrong, and only the driver can be: it is
  reading or sequencing completions badly.
- `info usb` on the monitor says `Device 0.0, Port 1, Speed 5000 Mb/s` - the stick
  is a SuperSpeed device on path "1", so the RH-port number and the speed the
  driver sends (PORTSC speed 4 = SuperSpeed) are both right. Port mapping was not
  the problem.
- `CRCR`, `DCBAAP` and the doorbell were checked directly: DCBAAP is programmed
  with a 64-aligned DCBAA, and commands go out on `DB[0]` (the host controller
  doorbell) with value 0. Both are correct. The 64-byte alignment fix applied to
  CRCR/ERST/EP0 was still worth keeping: it removed phantom "events" being read
  out of a ring nothing had written.

The RH-port probe loop added while chasing this is **wrong and must not be kept**:
once a slot owns a port, addressing that same port from a *different* slot is
rejected by the controller's duplicate-port rule, so retrying 1..8 guarantees
`CC_TRB_ERROR` and tells you nothing. It should be replaced by logging the raw
completion TRB (all four dwords) for every command, which will show whether the
driver is popping the completion for the command it just rang the doorbell for.

## The next step (cheapest first)

1. **Let QEMU say why**: boot with `-trace usb_xhci_*` and read the trace. If the
   controller is rejecting the command TRB or the doorbell, it says so directly,
   and that beats another round of guessing.
2. Read back CRCR after programming it, and USBCMD/USBSTS after Run. A controller
   that is halted, or a CRCR that does not read back what was written, explains
   "no completions, ever" in one line.
3. Confirm the doorbell: `DB[0]` is the host controller doorbell and takes no
   target; command submissions ring it with any value. A doorbell aimed at a slot
   number (DB[1] ...) silently does nothing for commands.

## Then, in order

- **BOT/SCSI**: CBW (31 bytes) out, data in/out, CSW back; `INQUIRY`,
  `READ CAPACITY(10)`, `READ(10)`. `READ CAPACITY` is the first proof that block
  addressing works, and the LUN's block count and size are what a filesystem
  needs first.
- **FAT32, read-only**: boot sector and BPB, FAT chains, directory entries with
  long names. Read-only is the requirement; write support is a separate risk.
- **Mount and show**: a second root for Files - a provider function rather than
  `vfs_readdir`, so the RAM drive and the stick are two backends behind one list,
  and a `usb` entry in `places`.

## Not to do

- Do not pass the physical Windows disk to QEMU (`\\.\PhysicalDriveN`) while Windows
  still has the volume mounted: two writers on one FAT is how a stick gets
  corrupted. The image copy is the agreed route.