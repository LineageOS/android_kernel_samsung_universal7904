.. SPDX-License-Identifier: GPL-2.0

Legacy 32-bit Linux 4.4 ION ABI
================================

CONFIG_ION_LEGACY_4_4_ABI (default n) enables three additional ioctls on
/dev/ion. The structures below consist only of 32-bit words, in the listed
order; no native size_t or pointer is used. Native and compat callers of
these exact requests use the same layout. The 64-bit Linux 4.4 ALLOC layout
is not implemented.

* 0xc0144900: ALLOC, { u32 len, u32 align, u32 heap_id_mask, u32 flags,
  u32 handle }. Returns a handle, not an fd.
* 0xc0084904: MAP, { u32 handle, s32 fd }. Returns an O_CLOEXEC dma-buf fd,
  which can be mmap'ed through the existing ION dma-buf implementation.
* 0xc0044901: FREE, { u32 handle }. Drops only the handle reference.

The ioctl magic is ASCII 'I' (0x49). The modern 24-byte ALLOC request is
0xc0184900 and is unchanged, as are HEAP_QUERY and ABI_VERSION (still 2).
Command matching includes the direction and size, not just the number.

Handle lifetime and fd ownership
-------------------------------

Each open file description has a mutex-protected IDR mapping positive
32-bit cookies (1..INT_MAX) to owning dma_buf references. Handles are
unique among live allocations on that open file description, can be reused
after FREE, and are shared by dup/fork of the ION fd. Independent opens
have independent tables. Closing the final ION file reference drops all
remaining handles.

There is no modern handle namespace: modern ALLOC returns a process fd,
and only the distinct legacy commands consult the IDR. A cookie may be
numerically equal to an fd but can never select that fd or a modern
allocation. MAP/FREE must not be used on the output of modern ALLOC.

MAP reserves an fd, copies it to userspace, takes an additional dma_buf
reference and installs the file. Copyout failure releases the reserved fd;
ALLOC copyout failure removes the new handle and releases its reference.
Repeated MAP calls create independent fds. FREE and closing /dev/ion do not
invalidate already installed dma-buf fds or their mmap mappings. Missing,
zero, out-of-range and already freed handles return -EINVAL.

Heaps, flags and limits
-----------------------

The requested heap_id_mask is passed unchanged to ion_dmabuf_alloc(). The
existing allocator tries only matching registered heap IDs and returns
-ENODEV when none match a nonzero-length allocation. There is no heap-ID
translation, substitution with the system heap, or physical-address setup.
Other allocation errors, including invalid zero length, are preserved.

m20lte_defconfig enables only the ION system heap: heap ID 0, mask 0x1.
It does not enable ION_CMA_HEAP. The unchanged allocator can select other
registered heaps if separately supplied, but this compatibility layer does
not create any. The ACK CMA heap IDs, when enabled separately, are allocated
in the range 1..8 and are not Exynos secure-heap identities. Do not interpret
a numerical match as implementing an Exynos heap.

Alignment may be zero or a power of two no larger than PAGE_SIZE; larger
or malformed alignment returns -EINVAL because the ACK allocator has no
alignment parameter. Flags 0 and ION_FLAG_CACHED (bit 0) are supported. The
legacy CACHED_NEEDS_SYNC hint (bit 1) is accepted; ACK's existing dma-buf
cache maintenance remains in effect and only bit 0 is passed to allocation.
All other flags return -EOPNOTSUPP, including Exynos ION_FLAG_PROTECTED
(bit 4), rather than silently allocating ordinary memory for a secure use.
Use the dma-buf CPU-access synchronization ABI where required; the legacy
ION SYNC ioctl and Exynos cache/custom ioctls are not implemented.

Exynos protected/DRM heaps, their secure-world registration, vendor
contiguous-heap flag decoding and physical-address ioctls are not supplied.
This is an allocation/MAP/FREE ABI bridge, not working DRM or a complete
libexynosion replacement. Runtime behavior, 32-bit userspace mmap/cache
coherency and liboemcrypto interoperability require later hardware testing;
no phone access is part of this change.
