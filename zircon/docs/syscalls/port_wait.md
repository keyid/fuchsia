# zx_port_wait

## NAME

<!-- Updated by update-docs-from-abigen, do not edit. -->

port_wait - wait for a packet arrival in a port

## SYNOPSIS

<!-- Updated by update-docs-from-abigen, do not edit. -->

```
#include <zircon/syscalls.h>
#include <zircon/syscalls/port.h>

zx_status_t zx_port_wait(zx_handle_t handle,
                         zx_time_t deadline,
                         zx_port_packet_t* packet);
```

## DESCRIPTION

`zx_port_wait()` is a blocking syscall which causes the caller to wait until at least
one packet is available.

Upon return, if successful *packet* will contain the earliest (in FIFO order)
available packet data.

The *deadline* indicates when to stop waiting for a packet (with respect to
**ZX_CLOCK_MONOTONIC**) and will be automatically adjusted according to the job's
[timer slack] policy. If no packet has arrived by the deadline,
**ZX_ERR_TIMED_OUT** is returned.  The value **ZX_TIME_INFINITE** will result in
waiting forever.  A value in the past will result in an immediate timeout,
unless a packet is already available for reading.

Unlike [`zx_object_wait_one()`] and [`zx_object_wait_many()`] only one
waiting thread is released (per available packet) which makes ports
amenable to be serviced by thread pools.

There are two classes of packets: packets queued by userspace with [`zx_port_queue()`]
and packets queued by the kernel when objects a port is registered with change state. In both
cases the packet is always of type `zx_port_packet_t`:

```
struct zx_port_packet_t {
    uint64_t key;
    uint32_t type;
    zx_status_t status;
    union {
        zx_packet_user_t user;
        zx_packet_signal_t signal;
        zx_packet_exception_t exception;
        zx_packet_guest_bell_t guest_bell;
        zx_packet_guest_mem_t guest_mem;
        zx_packet_guest_io_t guest_io;
        zx_packet_guest_vcpu_t guest_vcpu;
        zx_packet_interrupt_t interrupt;
    };
};
```

In the case of packets generated via [`zx_port_queue()`], *type* will be set to
**ZX_PKT_TYPE_USER**, and the caller of [`zx_port_queue()`] controls all other values in the
`zx_port_packet_t` structure. Access to the packet data is provided by the *user* member, with
type `zx_packet_user_t`:

```
typedef union zx_packet_user {
    uint64_t u64[4];
    uint32_t u32[8];
    uint16_t u16[16];
    uint8_t   c8[32];
} zx_packet_user_t;
```

For packets generated by the kernel, type can be one of the following values:

**ZX_PKT_TYPE_SIGNAL_ONE** or **ZX_PKT_TYPE_SIGNAL_REP** - generated by objects registered
via [`zx_object_wait_async()`].

**ZX_PKT_TYPE_EXCEPTION(n)** - generated by objects registered via
[`zx_task_bind_exception_port()`].

**ZX_PKT_TYPE_GUEST_BELL**, **ZX_PKT_TYPE_GUEST_MEM**, **ZX_PKT_TYPE_GUEST_IO**,
or **ZX_PKT_TYPE_GUEST_VCPU** - generated by objects registered via [`zx_guest_set_trap()`].

**ZX_PKT_TYPE_INTERRUPT** - generated by objects registered via [`zx_interrupt_bind()`].

All kernel queued packets will have *status* set to **ZX_OK** and *key* set to the
value provided to the registration syscall. For details on how to interpret the union, see
the corresponding registration syscall.

## RIGHTS

<!-- Updated by update-docs-from-abigen, do not edit. -->

*handle* must be of type **ZX_OBJ_TYPE_PORT** and have **ZX_RIGHT_READ**.

## RETURN VALUE

`zx_port_wait()` returns **ZX_OK** on successful packet dequeuing.

## ERRORS

**ZX_ERR_BAD_HANDLE** *handle* is not a valid handle.

**ZX_ERR_INVALID_ARGS** *packet* isn't a valid pointer

**ZX_ERR_ACCESS_DENIED** *handle* does not have **ZX_RIGHT_READ** and may
not be waited upon.

**ZX_ERR_TIMED_OUT** *deadline* passed and no packet was available.

## SEE ALSO

 - [timer slack]
 - [`zx_object_wait_async()`]
 - [`zx_port_create()`]
 - [`zx_port_queue()`]

[timer slack]: ../timer_slack.md

<!-- References updated by update-docs-from-abigen, do not edit. -->

[`zx_guest_set_trap()`]: guest_set_trap.md
[`zx_interrupt_bind()`]: interrupt_bind.md
[`zx_object_wait_async()`]: object_wait_async.md
[`zx_object_wait_many()`]: object_wait_many.md
[`zx_object_wait_one()`]: object_wait_one.md
[`zx_port_create()`]: port_create.md
[`zx_port_queue()`]: port_queue.md
[`zx_task_bind_exception_port()`]: task_bind_exception_port.md