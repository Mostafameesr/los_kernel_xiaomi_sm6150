# Codec2 ABI bridge: preparation only

This branch contains **unlinked translation primitives**, not a functional port
of the spring Codec2 service. There is no new video node, namespace-selection
ioctl, metadata queue, DMA importer, or Android service replacement.

The two new driver source files are deliberately absent from the VIDC Makefile
and are not called by the existing driver. Building or checking out this branch
does not activate a new Codec2 path. Do not package these files as a working
vendor update or flashable video fix.

Base: `stable` at `d73589d319c0e3f33048d99f69d1460b584feac7`.

## Implemented and checked

`msm_vidc_qc2_translate_ctrl()` takes an explicit namespace. Legacy requests
are copied without reinterpretation, including numbers that collide with the
new namespace. Modern requests use an allowlist; unknown controls fail with
`-EOPNOTSUPP`, and invalid values fail with `-EINVAL`. Failures leave the output
untouched. No process-name or 32/64-bit heuristic selects an ABI.

| Modern operation | Modern ID | Legacy ID | Accepted values |
| --- | --- | --- | --- |
| Output order | `0x00990b8e` | `0x00992003` | 0 display, 1 decode |
| Low latency | `0x00992003` | `0x00992038` | 0 off, 1 on |
| Thumbnail | `0x00992029` | `0x0099200f` | 0 off, 1 on |

The donor's enum-to-string ARM64 methods confirm output-order and low-latency
values. The legacy decoder's control descriptors establish the target values;
its sync-frame-decode implementation implements thumbnail mode.

The other known collisions remain deliberately unsupported:

- Secure mode: the legacy control is a BUTTON whose handler sets `VIDC_SECURE`
  regardless of the supplied value. A boolean-to-button number substitution
  cannot implement both enable and disable safely.
- Priority: the legacy public enum and the driver's `HAL_CONFIG_REALTIME`
  handler have conflicting zero/one descriptions. The modern interface also
  supports a wider range in the Qualcomm reference implementation. No speculative
  value conversion or clamping is used.
- Client ID: no verified legacy semantic equivalent has been established.

`msm_vidc_qc2_plane_to_legacy()` validates integer descriptor bounds, clears
reserved fields, and places fd/offset in the slots consumed by the old
`msm_vidc_qbuf()` path. It only converts descriptor fields. It does **not**
validate that an fd is open, pin/import memory, wait on a fence, negotiate plane
formats, or manage buffer lifetime. Its success must never be treated as a
successful `QBUF`. In particular, it does not justify adding `VB2_DMABUF` to a
queue's advertised capabilities.

## Verification

From the repository root:

```sh
make -C tools/testing/selftests/media/qc2_compat check
make -C tools/testing/selftests/media/qc2_compat clean
ASAN_OPTIONS=detect_leaks=0 make -C tools/testing/selftests/media/qc2_compat SANITIZE=1 check
```

The normal and AddressSanitizer/UndefinedBehaviorSanitizer runs each completed
20,106 assertions. They exercise colliding IDs, invalid values, null arguments,
output preservation on rejection, fd zero, integer boundaries, cleared reserved
fields, and 10,000 legacy namespace requests. LeakSanitizer was disabled because
the execution environment cannot inspect the required `/proc/.../task` state;
these helpers perform no allocations. This is not a hardware or kernel-runtime
test.

The helper was also compiled as an ARM64 kernel object through this kernel's
build system, using `vendor/sweet_defconfig`, Clang 18.1.3, the integrated
assembler and LLD. The C/header style check reported zero errors and zero
warnings. No complete kernel image was built or booted, and this object is
still absent from the normal link list.

Donor enum verification requires Python 3, `pyelftools`, `unicorn`, and the
original `libqcodec2_v4l2codec.so` supplied separately:

```sh
python3 tools/testing/selftests/media/qc2_compat/verify_donor_enums.py /path/to/libqcodec2_v4l2codec.so
```

The version-specific test refuses any SHA256 other than:

`068597b574985be80ff760973a4920e567a3cb21f14d108e0eb9401b634f1877`

## Unfinished requirements before integration

1. Select the new ABI explicitly per open instance or isolated video node;
   preserve the existing ABI and reject unsupported operations consistently.
2. Pin DMA-BUFs, validate negotiated formats, convert QBUF/DQBUF descriptors and
   manage references across queueing, flush, stop, error, close and fd reuse.
3. Implement real input/output metadata queues and frame association. Translate
   the actual metadata layout to/from the old firmware's extradata protocol,
   including tags, crops, color/HDR data and ownership/completion. Returning
   synthetic success for an absent metadata queue is not an implementation.
4. Translate query/get/set controls, events, resolution changes and drain/EOS
   behavior. Secure decoding and priority must remain rejected until their
   complete semantics and memory paths are verified.
5. Validate the service's Android linker namespace, API34 dependencies, CFI and
   gralloc compatibility before replacing or starting the service.
6. Build the integrated kernel, then verify actual playback, camera/encoding,
   repeated reconfiguration, suspend/resume and recovery on the device. Only
   then create a manual vendor payload and an explicit rollback set.

The available Qualcomm reference driver forwards separate metadata DMA buffers
to firmware. It does not expose a host-side conversion schema for the donor
payload in the files inspected. The old driver uses an extradata area with a
different lifecycle. Their compatibility cannot be established by copying a
structure name or accepting an unsupported ioctl.

The user's authorization is conditional on safety. It covers preparing and
checking this isolated work; it is not evidence that the unfinished transport
is safe to activate. No OMX service is disabled in this branch, and there is no
claim that the black wallpaper or video color instability has been fixed.

## References

- [Legacy kernel base](https://github.com/Mostafameesr/los_kernel_xiaomi_sm6150/tree/d73589d319c0e3f33048d99f69d1460b584feac7)
- [Qualcomm reference driver](https://github.com/qualcomm-linux/video-driver/tree/9cc7751521423dbeb424d8595b12e3582878b3f5)
- [DMA-BUF importing API](https://docs.kernel.org/userspace-api/media/v4l/dmabuf.html)
- [Metadata interface](https://docs.kernel.org/userspace-api/media/v4l/dev-meta.html)
