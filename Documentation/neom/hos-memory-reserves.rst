.. SPDX-License-Identifier: GPL-2.0

HyperOS port memory reserves
===========================

Scope and status
----------------

``CONFIG_NEOM_HOS_MEMORY_RESERVES`` adds functioning, bounded allocation
reserves behind two interfaces requested by the captured vendor init files:

* ``/sys/kernel/mi_mempool/pids`` selects a 32 MiB order-0 page reserve used
  before direct reclaim by explicitly registered native processes.
* ``/sys/kernel/reserve_pool/pid`` selects private cached and uncached ION
  pools, capped at 32 MiB in total with the normal {9, 4, 0} heap orders.
  As in the donor implementation, this ION extension requires more than
  6 GiB of usable RAM. The captured 8 GiB device satisfies that guard.

This is a 4.14 implementation of the relevant PID/reserve functionality,
not a full backport of the donor's GKI modules or all of their tuning knobs.
It does not implement Smart Cache, ``memory.mctrl.*``, ``iorap_dev``, or
the userspace Perf/Suspend protocols. It is not evidence that allocation
pressure caused the measured launcher GPU latency.

Registration and lifetime
-------------------------

Each registration file accepts one decimal or base-prefixed integer per
write. A positive PID or TID registers its thread group; a negative TGID
removes that registration. Zero and INT_MIN are rejected with EINVAL, nonexistent
processes with ESRCH, and a full 16-entry table with ENOSPC. Duplicate writes
are idempotent. Writers must use the initial PID namespace.

The tables retain ``struct pid`` references rather than trusting reused PID
numbers or storing unpinned task pointers. Dead registrations are pruned on
subsequent updates, reads and refill checks. Threads of a registered process
share its eligibility; an unrelated process reusing a number does not.

Both directories provide ``enable`` and ``stats`` attributes. Setting enable
to zero cancels refill and drains the reserve. It does not remove PID
registrations or change service, scheduler or display settings. Re-enabling
repopulates a reserve only while registered processes are alive.

Allocator behavior
------------------

The order-0 reserve is consulted only after the ordinary allocation attempts
fail and just before direct reclaim. It is bypassed for unsupported orders,
explicit nodemasks, atomic/non-reclaimable requests and DMA-constrained zones.
Cpuset restrictions remain checked, and the ordinary post-allocation memcg
charge still runs. An empty reserve falls through to the original allocator.

ION reserves use the existing page-pool accounting and zeroing flow. Cached
and uncached pools remain separate. Secure VMID buffers, force allocations
and shrinker-free buffers retain their existing paths. Fresh refill pages
are zeroed and synchronized for the heap's DMA device before becoming
available. Bounded insertion checks occupancy while holding the pool mutex.

The ION pools join the heap's existing shrink callback. The order-0 pool has
its own shrinker. Both expose their free pages as reclaimable, pause refill
after pressure scans and avoid direct reclaim in refill work. Refills use a
freezable workqueue, a 96 MiB available-memory headroom check and finite batches.
Unused reserves drain when no registered processes remain alive.

All state is boot-lifetime, matching this tree's built-in ION implementation
which has no heap removal callback. Failed setup unwinds created pool
objects, sysfs references or the registered shrinker. Ordinary ION creation
continues if optional reserve initialization fails.

Reference implementations
-------------------------

The design references are GPL sources published by MiCode, pinned to
``56295df675b92553f83bbf333f88b0c6ee8fa7bc`` in
``MiCode/Xiaomi_Kernel_OpenSource``:

* ``drivers/dma-buf/heaps/qcom_extend_system_heap.c``: PID-selected graphics
  reserve pools and the usable-RAM guard. Git blob:
  ``6ac66491eb25bea051d34c9dac6154e60ed46341``.
* ``drivers/staging/mi-perf/mi_mempool/mi_mempool.c`` and ``mi_mempool.h``:
  reserve-before-direct-reclaim design and bounded page reserves. These use
  GKI hooks and a different selection/tuning policy; their struct offsets,
  GKI hooks and allocator internals were not copied into the 4.14 ABI.

Validation boundaries
---------------------

The modified allocator, registry and ION objects were cross-compiled for
ARM64 with the feature enabled, and the complete Image.gz-dtb linked.
Disabled-feature object builds also cover the original allocator fallback.
Strict checkpatch covers the changes; its generic FILE_PATH_CHANGES notice
is accounted for by the added MAINTAINERS entry.

These checks establish compilation and linkage. A phone boot, sustained
allocation-pressure test, suspend/resume test, secure-buffer/camera test and
matched frame-latency comparison are still required before calling this a
validated device repair. No such hardware test is claimed here.
