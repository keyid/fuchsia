// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <object/port_dispatcher.h>

#include <assert.h>
#include <err.h>
#include <platform.h>
#include <pow2.h>

#include <fbl/alloc_checker.h>
#include <fbl/arena.h>
#include <fbl/auto_lock.h>
#include <lib/counters.h>
#include <object/excp_port.h>
#include <object/handle.h>
#include <object/process_dispatcher.h>
#include <object/thread_dispatcher.h>
#include <zircon/compiler.h>
#include <zircon/rights.h>
#include <zircon/syscalls/port.h>
#include <zircon/types.h>

// All port sub-packets must be exactly 32 bytes
static_assert(sizeof(zx_packet_user_t) == 32, "incorrect size for zx_packet_signal_t");
static_assert(sizeof(zx_packet_signal_t) == 32, "incorrect size for zx_packet_signal_t");
static_assert(sizeof(zx_packet_exception_t) == 32, "incorrect size for zx_packet_exception_t");
static_assert(sizeof(zx_packet_guest_bell_t) == 32, "incorrect size for zx_packet_guest_bell_t");
static_assert(sizeof(zx_packet_guest_mem_t) == 32, "incorrect size for zx_packet_guest_mem_t");
static_assert(sizeof(zx_packet_guest_io_t) == 32, "incorrect size for zx_packet_guest_io_t");
static_assert(sizeof(zx_packet_guest_vcpu_t) == 32, "incorrect size for zx_packet_guest_vcpu_t");
static_assert(sizeof(zx_packet_interrupt_t) == 32, "incorrect size for zx_packet_interrupt_t");
static_assert(sizeof(zx_packet_page_request_t) == 32,
              "incorrect size for zx_packet_page_request_t");

KCOUNTER(port_arena_count, "port.arena.count");
KCOUNTER(port_full_count, "port.full.count");
KCOUNTER(dispatcher_port_create_count, "dispatcher.port.create");
KCOUNTER(dispatcher_port_destroy_count, "dispatcher.port.destroy");

class ArenaPortAllocator final : public PortAllocator {
public:
    zx_status_t Init();
    virtual ~ArenaPortAllocator() = default;

    virtual PortPacket* Alloc();
    virtual void Free(PortPacket* port_packet);

private:
    fbl::TypedArena<PortPacket, fbl::Mutex> arena_;
};

namespace {
constexpr size_t kMaxPendingPacketCount = 16 * 1024u;

// TODO(maniscalco): Enforce this limit per process via the job policy.
constexpr size_t kMaxPendingPacketCountPerPort = kMaxPendingPacketCount / 8;
ArenaPortAllocator port_allocator;
} // namespace.

zx_status_t ArenaPortAllocator::Init() {
    return arena_.Init("packets", kMaxPendingPacketCount);
}

PortPacket* ArenaPortAllocator::Alloc() {
    PortPacket* packet = arena_.New(nullptr, this);
    if (packet == nullptr) {
        printf("WARNING: Could not allocate new port packet\n");
        return nullptr;
    }
    kcounter_add(port_arena_count, 1);
    return packet;
}

void ArenaPortAllocator::Free(PortPacket* port_packet) {
    arena_.Delete(port_packet);
    kcounter_add(port_arena_count, -1);
}

PortPacket::PortPacket(const void* handle, PortAllocator* allocator)
    : packet{}, handle(handle), observer(nullptr), allocator(allocator) {
    // Note that packet is initialized to zeros.
    if (handle) {
        // Currently |handle| is only valid if the packets are not ephemeral
        // which means that PortObserver always uses the kernel heap.
        DEBUG_ASSERT(allocator == nullptr);
    }
}

PortObserver::PortObserver(uint32_t type, const Handle* handle, fbl::RefPtr<PortDispatcher> port,
                           uint64_t key, zx_signals_t signals)
    : type_(type),
      trigger_(signals),
      packet_(handle, nullptr),
      port_(ktl::move(port)) {

    DEBUG_ASSERT(handle != nullptr);

    auto& packet = packet_.packet;
    packet.status = ZX_OK;
    packet.key = key;
    packet.type = type_;
    packet.signal.trigger = trigger_;
}

StateObserver::Flags PortObserver::OnInitialize(zx_signals_t initial_state,
                                                const StateObserver::CountInfo* cinfo) {
    uint64_t count = 1u;

    if (cinfo) {
        for (const auto& entry : cinfo->entry) {
            if ((entry.signal & trigger_) && (entry.count > 0u)) {
                count = entry.count;
                break;
            }
        }
    }
    return MaybeQueue(initial_state, count);
}

StateObserver::Flags PortObserver::OnStateChange(zx_signals_t new_state) {
    return MaybeQueue(new_state, 1u);
}

StateObserver::Flags PortObserver::OnCancel(const Handle* handle) {
    if (packet_.handle == handle) {
        return kHandled | kNeedRemoval;
    } else {
        return 0;
    }
}

StateObserver::Flags PortObserver::OnCancelByKey(const Handle* handle, const void* port, uint64_t key) {
    if ((packet_.handle != handle) || (packet_.key() != key) || (port_.get() != port))
        return 0;
    return kHandled | kNeedRemoval;
}

void PortObserver::OnRemoved() {
    // If observer ends up being non-null, it is ourself, and thus our
    // responsibility to delete ourself.
    ktl::unique_ptr<PortObserver> observer =
        port_->MaybeReap(ktl::unique_ptr<PortObserver>(this), &packet_);
}

StateObserver::Flags PortObserver::MaybeQueue(zx_signals_t new_state, uint64_t count) {
    // Always called with the object state lock being held.
    if ((trigger_ & new_state) == 0u)
        return 0;

    // TODO(cpu): Queue() can fail and we don't propagate this information
    // here properly. Now, this failure is self inflicted because we constrain
    // the packet arena size artificially.  See ZX-2166 for details.
    auto status = port_->Queue(&packet_, new_state, count);

    if ((type_ == ZX_PKT_TYPE_SIGNAL_ONE) || (status != ZX_OK))
        return kNeedRemoval;

    return 0;
}

/////////////////////////////////////////////////////////////////////////////////////////

void PortDispatcher::Init() {
    port_allocator.Init();
}

PortAllocator* PortDispatcher::DefaultPortAllocator() {
    return &port_allocator;
}

zx_status_t PortDispatcher::Create(uint32_t options, fbl::RefPtr<Dispatcher>* dispatcher,
                                   zx_rights_t* rights) {
    if (options && options != ZX_PORT_BIND_TO_INTERRUPT) {
        return ZX_ERR_INVALID_ARGS;
    }
    fbl::AllocChecker ac;
    auto disp = new (&ac) PortDispatcher(options);
    if (!ac.check())
        return ZX_ERR_NO_MEMORY;

    *rights = default_rights();
    *dispatcher = fbl::AdoptRef<Dispatcher>(disp);
    return ZX_OK;
}

PortDispatcher::PortDispatcher(uint32_t options)
    : options_(options), zero_handles_(false), num_packets_(0u) {
    kcounter_add(dispatcher_port_create_count, 1);
}

PortDispatcher::~PortDispatcher() {
    DEBUG_ASSERT(zero_handles_);
    DEBUG_ASSERT(num_packets_ == 0u);
    kcounter_add(dispatcher_port_destroy_count, 1);
}

void PortDispatcher::on_zero_handles() {
    canary_.Assert();

    Guard<fbl::Mutex> guard{get_lock()};
    zero_handles_ = true;

    // Unlink and unbind exception ports.
    while (!eports_.is_empty()) {
        auto eport = eports_.pop_back();

        // Tell the eport to unbind itself, then drop our ref to it. Called
        // unlocked because the eport may call our ::UnlinkExceptionPort.
        guard.CallUnlocked([&eport]() { eport->OnPortZeroHandles(); });
    }

    // Free any queued packets.
    while (!packets_.is_empty()) {
        auto packet = packets_.pop_front();
        --num_packets_;

        // If the packet is ephemeral, free it outside of the lock. Otherwise,
        // reset the observer if it is present.
        if (packet->is_ephemeral()) {
            guard.CallUnlocked([packet]() {
                    packet->Free();
            });
        } else {
            // The reference to the port that the observer holds cannot be the last one
            // because another reference was used to call on_zero_handles, so we don't
            // need to worry about destroying ourselves.
            packet->observer.reset();
        }
    }
}

zx_status_t PortDispatcher::QueueUser(const zx_port_packet_t& packet) {
    canary_.Assert();

    auto port_packet = port_allocator.Alloc();
    if (!port_packet)
        return ZX_ERR_NO_MEMORY;

    port_packet->packet = packet;
    port_packet->packet.type = ZX_PKT_TYPE_USER;

    auto status = Queue(port_packet, 0u, 0u);
    if (status != ZX_OK)
        port_packet->Free();
    return status;
}

bool PortDispatcher::RemoveInterruptPacket(PortInterruptPacket* port_packet) {
    Guard<SpinLock, IrqSave> guard{&spinlock_};
    if (port_packet->InContainer()) {
        interrupt_packets_.erase(*port_packet);
        return true;
    }
    return false;
}

bool PortDispatcher::QueueInterruptPacket(PortInterruptPacket* port_packet, zx_time_t timestamp) {
    Guard<SpinLock, IrqSave> guard{&spinlock_};
    if (port_packet->InContainer()) {
        return false;
    } else {
        port_packet->timestamp = timestamp;
        interrupt_packets_.push_back(port_packet);
        sema_.Post();
        return true;
    }
}

zx_status_t PortDispatcher::Queue(PortPacket* port_packet, zx_signals_t observed, uint64_t count) {
    canary_.Assert();

    AutoReschedDisable resched_disable; // Must come before the lock guard.
    Guard<fbl::Mutex> guard{get_lock()};
    if (zero_handles_)
        return ZX_ERR_BAD_STATE;

    if (num_packets_ > kMaxPendingPacketCountPerPort) {
        kcounter_add(port_full_count, 1);
        return ZX_ERR_SHOULD_WAIT;
    }

    if (observed) {
        if (port_packet->InContainer()) {
            port_packet->packet.signal.observed |= observed;
            // |count| is deliberately left as is.
            return ZX_OK;
        }
        port_packet->packet.signal.observed = observed;
        port_packet->packet.signal.count = count;
    }
    packets_.push_back(port_packet);
    ++num_packets_;
    // This Disable() call must come before Post() to be useful, but doing
    // it earlier would also be OK.
    resched_disable.Disable();
    sema_.Post();

    return ZX_OK;
}

zx_status_t PortDispatcher::Dequeue(const Deadline& deadline,
                                    zx_port_packet_t* out_packet) {
    canary_.Assert();

    while (true) {
        if (options_ == ZX_PORT_BIND_TO_INTERRUPT) {
            Guard<SpinLock, IrqSave> guard{&spinlock_};
            PortInterruptPacket* port_interrupt_packet = interrupt_packets_.pop_front();
            if (port_interrupt_packet != nullptr) {
                *out_packet = {};
                out_packet->key = port_interrupt_packet->key;
                out_packet->type = ZX_PKT_TYPE_INTERRUPT;
                out_packet->status = ZX_OK;
                out_packet->interrupt.timestamp = port_interrupt_packet->timestamp;
                return ZX_OK;
            }
        }
        {
            Guard<fbl::Mutex> guard{get_lock()};
            PortPacket* port_packet = packets_.pop_front();
            if (port_packet != nullptr) {
                --num_packets_;
                *out_packet = port_packet->packet;

                bool is_ephemeral = port_packet->is_ephemeral();
                // The reference to the port that the observer holds cannot be the last one
                // because another reference was used to call Dequeue, so we don't need to
                // worry about destroying ourselves.
                port_packet->observer.reset();
                guard.Release();

                // If the packet is ephemeral, free it outside of the lock. We need to read
                // is_ephemeral inside the lock because it's possible for a non-ephemeral packet
                // to get deleted after a call to |MaybeReap| as soon as we release the lock.
                if (is_ephemeral) {
                    port_packet->Free();
                }
                return ZX_OK;
            }
        }

        {
            ThreadDispatcher::AutoBlocked by(ThreadDispatcher::Blocked::PORT);
            zx_status_t st = sema_.Wait(deadline);
            if (st != ZX_OK)
                return st;
        }
    }
}

ktl::unique_ptr<PortObserver> PortDispatcher::MaybeReap(ktl::unique_ptr<PortObserver> observer,
                                                        PortPacket* port_packet) {
    canary_.Assert();
    DEBUG_ASSERT(!port_packet->is_ephemeral());

    Guard<fbl::Mutex> guard{get_lock()};
    if (port_packet->InContainer()) {
        // The destruction will happen when the packet is dequeued or in CancelQueued()
        DEBUG_ASSERT(port_packet->observer == nullptr);
        port_packet->observer = ktl::move(observer);
    }
    return observer;
}

zx_status_t PortDispatcher::MakeObserver(uint32_t options, Handle* handle, uint64_t key,
                                         zx_signals_t signals) {
    canary_.Assert();

    // Called under the handle table lock.

    auto dispatcher = handle->dispatcher();
    if (!dispatcher->is_waitable())
        return ZX_ERR_NOT_SUPPORTED;

    uint32_t type;
    switch (options) {
        case ZX_WAIT_ASYNC_ONCE:
            type = ZX_PKT_TYPE_SIGNAL_ONE;
            break;
        case ZX_WAIT_ASYNC_REPEATING:
            type = ZX_PKT_TYPE_SIGNAL_REP;
            break;
        default:
            return ZX_ERR_INVALID_ARGS;
    }

    fbl::AllocChecker ac;
    auto observer = new (&ac) PortObserver(type, handle, fbl::RefPtr<PortDispatcher>(this), key,
                                           signals);
    if (!ac.check())
        return ZX_ERR_NO_MEMORY;

    dispatcher->add_observer(observer);
    return ZX_OK;
}

bool PortDispatcher::CancelQueued(const void* handle, uint64_t key) {
    canary_.Assert();

    Guard<fbl::Mutex> guard{get_lock()};

    // This loop can take a while if there are many items.
    // In practice, the number of pending signal packets is
    // approximately the number of signaled _and_ watched
    // objects plus the number of pending user-queued
    // packets.
    //
    // There are two strategies to deal with too much
    // looping here if that is seen in practice.
    //
    // 1. Swap the |packets_| list for an empty list and
    //    release the lock. New arriving packets are
    //    added to the empty list while the loop happens.
    //    Readers will be blocked but the watched objects
    //    will be fully operational. Once processing
    //    is done the lists are appended.
    //
    // 2. Segregate user packets from signal packets
    //    and deliver them in order via timestamps or
    //    a side structure.

    bool packet_removed = false;

    for (auto it = packets_.begin(); it != packets_.end();) {
        if ((it->handle == handle) && (it->key() == key)) {
            auto to_remove = it++;
            // Destroyed as we go around the loop.
            ktl::unique_ptr<const PortObserver> observer =
                ktl::move(packets_.erase(to_remove)->observer);
            --num_packets_;
            packet_removed = true;
        } else {
            ++it;
        }
    }

    return packet_removed;
}

bool PortDispatcher::CancelQueued(PortPacket* port_packet) {
    canary_.Assert();

    Guard<fbl::Mutex> guard{get_lock()};

    if (port_packet->InContainer()) {
        packets_.erase(*port_packet)->observer.reset();
        --num_packets_;
        return true;
    }

    return false;
}

void PortDispatcher::LinkExceptionPortEportLocked(ExceptionPort* eport) {
    canary_.Assert();

    Guard<fbl::Mutex> guard{get_lock()};
    DEBUG_ASSERT_COND(eport->PortMatchesLocked(this, /* allow_null */ false));
    DEBUG_ASSERT(!eport->InContainer());
    eports_.push_back(ktl::move(AdoptRef(eport)));
}

void PortDispatcher::UnlinkExceptionPortEportLocked(ExceptionPort* eport) {
    canary_.Assert();

    Guard<fbl::Mutex> guard{get_lock()};
    DEBUG_ASSERT_COND(eport->PortMatchesLocked(this, /* allow_null */ true));
    if (eport->InContainer()) {
        eports_.erase(*eport);
    }
}
