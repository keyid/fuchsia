// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <object/job_dispatcher.h>

#include <err.h>

#include <zircon/rights.h>
#include <zircon/syscalls/policy.h>
#include <fbl/alloc_checker.h>
#include <fbl/auto_lock.h>
#include <fbl/mutex.h>
#include <object/process_dispatcher.h>

using fbl::AutoLock;

// The starting max_height value of the root job.
static constexpr uint32_t kRootJobMaxHeight = 32;

static constexpr char kRootJobName[] = "<superroot>";

fbl::RefPtr<JobDispatcher> JobDispatcher::CreateRootJob() {
    fbl::AllocChecker ac;
    auto job = fbl::AdoptRef(new (&ac) JobDispatcher(0u, nullptr, kPolicyEmpty));
    if (!ac.check())
        return nullptr;
    job->set_name(kRootJobName, sizeof(kRootJobName));
    return job;
}

zx_status_t JobDispatcher::Create(uint32_t flags,
                                  fbl::RefPtr<JobDispatcher> parent,
                                  fbl::RefPtr<Dispatcher>* dispatcher,
                                  zx_rights_t* rights) {
    if (parent != nullptr && parent->max_height() == 0) {
        // The parent job cannot have children.
        return ZX_ERR_OUT_OF_RANGE;
    }

    fbl::AllocChecker ac;
    fbl::RefPtr<JobDispatcher> job =
        fbl::AdoptRef(new (&ac) JobDispatcher(flags, parent, parent->GetPolicy()));
    if (!ac.check())
        return ZX_ERR_NO_MEMORY;

    if (!parent->AddChildJob(job.get())) {
        return ZX_ERR_BAD_STATE;
    }

    *rights = ZX_DEFAULT_JOB_RIGHTS;
    *dispatcher = fbl::move(job);
    return ZX_OK;
}

JobDispatcher::JobDispatcher(uint32_t /*flags*/,
                             fbl::RefPtr<JobDispatcher> parent,
                             pol_cookie_t policy)
    : Dispatcher(ZX_JOB_NO_PROCESSES | ZX_JOB_NO_JOBS),
      parent_(fbl::move(parent)),
      max_height_(parent_ ? parent_->max_height() - 1 : kRootJobMaxHeight),
      state_(State::READY),
      process_count_(0u),
      job_count_(0u),
      importance_(parent != nullptr
                      ? ZX_JOB_IMPORTANCE_INHERITED
                      : ZX_JOB_IMPORTANCE_MAX),
      policy_(policy) {

    // Set the initial relative importance.
    // Tries to make older jobs closer to the root more important.
    if (parent_ == nullptr) {
        // Root job is the most important.
        AutoLock lock(&importance_lock_);
        importance_list_.push_back(this);
    } else {
        AutoLock plock(&parent_->lock_);
        JobDispatcher* neighbor;
        if (!parent_->jobs_.is_empty()) {
            // Our youngest sibling.
            //
            // IMPORTANT: We must hold the parent's lock during list insertion
            // to ensure that our sibling stays alive until we're done with it.
            // The sibling may be in its dtor right now, trying to remove itself
            // from parent_->jobs_ but blocked on parent_->lock_, and could be
            // freed if we released the lock.
            neighbor = &parent_->jobs_.back();

            // This can't be us: we aren't added to our parent's child list
            // until after construction.
            DEBUG_ASSERT(!dll_job_raw_.InContainer());
            DEBUG_ASSERT(neighbor != this);
        } else {
            // Our parent.
            neighbor = parent_.get();
        }

        // Make ourselves slightly less important than our neighbor.
        AutoLock lock(&importance_lock_);
        importance_list_.insert( // insert before
            importance_list_.make_iterator(*neighbor), this);
    }
}

JobDispatcher::~JobDispatcher() {
    if (parent_)
        parent_->RemoveChildJob(this);

    {
        AutoLock lock(&importance_lock_);
        DEBUG_ASSERT(dll_importance_.InContainer());
        importance_list_.erase(*this);
    }
}

void JobDispatcher::on_zero_handles() {
    canary_.Assert();
}

zx_koid_t JobDispatcher::get_related_koid() const {
    return parent_ ? parent_->get_koid() : 0u;
}

bool JobDispatcher::AddChildProcess(ProcessDispatcher* process) {
    canary_.Assert();

    AutoLock lock(&lock_);
    if (state_ != State::READY)
        return false;
    procs_.push_back(process);
    ++process_count_;
    UpdateSignalsIncrementLocked();
    return true;
}

bool JobDispatcher::AddChildJob(JobDispatcher* job) {
    canary_.Assert();

    AutoLock lock(&lock_);
    if (state_ != State::READY)
        return false;

    jobs_.push_back(job);
    ++job_count_;
    UpdateSignalsIncrementLocked();
    return true;
}

void JobDispatcher::RemoveChildProcess(ProcessDispatcher* process) {
    canary_.Assert();

    AutoLock lock(&lock_);
    // The process dispatcher can call us in its destructor, Kill(),
    // or RemoveThread().
    if (!ProcessDispatcher::JobListTraitsRaw::node_state(*process).InContainer())
        return;
    procs_.erase(*process);
    --process_count_;
    UpdateSignalsDecrementLocked();
}

void JobDispatcher::RemoveChildJob(JobDispatcher* job) {
    canary_.Assert();

    AutoLock lock(&lock_);
    if (!JobDispatcher::ListTraitsRaw::node_state(*job).InContainer())
        return;
    jobs_.erase(*job);
    --job_count_;
    UpdateSignalsDecrementLocked();
}

void JobDispatcher::UpdateSignalsDecrementLocked() {
    canary_.Assert();

    DEBUG_ASSERT(lock_.IsHeld());
    // removing jobs or processes.
    zx_signals_t set = 0u;
    if (process_count_ == 0u) {
        DEBUG_ASSERT(procs_.is_empty());
        set |= ZX_JOB_NO_PROCESSES;
    }
    if (job_count_ == 0u) {
        DEBUG_ASSERT(jobs_.is_empty());
        set |= ZX_JOB_NO_JOBS;
    }

    if ((job_count_ == 0) && (process_count_ == 0)) {
        if (state_ == State::KILLING)
            state_ = State::READY;
        if (!parent_)
            panic("No user processes left!\n");
    }

    UpdateState(0u, set);
}

void JobDispatcher::UpdateSignalsIncrementLocked() {
    canary_.Assert();

    DEBUG_ASSERT(lock_.IsHeld());
    // Adding jobs or processes.
    zx_signals_t clear = 0u;
    if (process_count_ == 1u) {
        DEBUG_ASSERT(!procs_.is_empty());
        clear |= ZX_JOB_NO_PROCESSES;
    }
    if (job_count_ == 1u) {
        DEBUG_ASSERT(!jobs_.is_empty());
        clear |= ZX_JOB_NO_JOBS;
    }
    UpdateState(clear, 0u);
}

pol_cookie_t JobDispatcher::GetPolicy() {
    AutoLock lock(&lock_);
    return policy_;
}

void JobDispatcher::Kill() {
    canary_.Assert();

    JobList jobs_to_kill;
    ProcessList procs_to_kill;

    {
        AutoLock lock(&lock_);
        if (state_ != State::READY)
            return;

        // Short circuit if there is nothing to do. Notice |state_|
        // does not change.
        if ((job_count_ == 0u) && (process_count_ == 0u))
            return;

        state_ = State::KILLING;

        // Safely gather refs to the children.
        ForEachChildInLocked(jobs_, [&](fbl::RefPtr<JobDispatcher> job) {
            jobs_to_kill.push_front(fbl::move(job));
            return ZX_OK;
        });
        ForEachChildInLocked(procs_, [&](fbl::RefPtr<ProcessDispatcher> proc) {
            procs_to_kill.push_front(fbl::move(proc));
            return ZX_OK;
        });
    }

    // Since we kill the child jobs first we have a depth-first massacre.
    while (!jobs_to_kill.is_empty()) {
        // TODO(cpu): This recursive call can overflow the stack.
        jobs_to_kill.pop_front()->Kill();
    }

    while (!procs_to_kill.is_empty()) {
        procs_to_kill.pop_front()->Kill();
    }
}

zx_status_t JobDispatcher::SetPolicy(
    uint32_t mode, const zx_policy_basic* in_policy, size_t policy_count) {
    // Can't set policy when there are active processes or jobs.
    AutoLock lock(&lock_);

    if (!procs_.is_empty() || !jobs_.is_empty())
        return ZX_ERR_BAD_STATE;

    pol_cookie_t new_policy;
    auto status = GetSystemPolicyManager()->AddPolicy(
        mode, policy_, in_policy, policy_count, &new_policy);

    if (status < 0)
        return status;

    policy_ = new_policy;
    return ZX_OK;
}

bool JobDispatcher::EnumerateChildren(JobEnumerator* je, bool recurse) {
    canary_.Assert();

    AutoLock lock(&lock_);

    zx_status_t s = ForEachChildInLocked(
        procs_, [&](fbl::RefPtr<ProcessDispatcher> proc) {
            return je->OnProcess(proc.get()) ? ZX_OK : ZX_ERR_STOP;
        });
    if (s != ZX_OK) {
        return false;
    }
    s = ForEachChildInLocked(jobs_, [&](fbl::RefPtr<JobDispatcher> job) {
        if (!je->OnJob(job.get())) {
            return ZX_ERR_STOP;
        }
        if (recurse) {
            // TODO(kulakowski): This recursive call can overflow the stack.
            return job->EnumerateChildren(je, /* recurse */ true)
                       ? ZX_OK
                       : ZX_ERR_STOP;
        }
        return ZX_OK;
    });
    return s == ZX_OK;
}

fbl::RefPtr<ProcessDispatcher>
JobDispatcher::LookupProcessById(zx_koid_t koid) {
    canary_.Assert();

    AutoLock lock(&lock_);
    fbl::RefPtr<ProcessDispatcher> found_proc;
    ForEachChildInLocked(procs_, [&](fbl::RefPtr<ProcessDispatcher> proc) {
        if (proc->get_koid() == koid) {
            found_proc = fbl::move(proc);
            return ZX_ERR_STOP;
        }
        return ZX_OK;
    });
    return found_proc; // Null if not found.
}

fbl::RefPtr<JobDispatcher>
JobDispatcher::LookupJobById(zx_koid_t koid) {
    canary_.Assert();

    AutoLock lock(&lock_);
    fbl::RefPtr<JobDispatcher> found_job;
    ForEachChildInLocked(jobs_, [&](fbl::RefPtr<JobDispatcher> job) {
        if (job->get_koid() == koid) {
            found_job = fbl::move(job);
            return ZX_ERR_STOP;
        }
        return ZX_OK;
    });
    return found_job; // Null if not found.
}

void JobDispatcher::get_name(char out_name[ZX_MAX_NAME_LEN]) const {
    canary_.Assert();

    name_.get(ZX_MAX_NAME_LEN, out_name);
}

zx_status_t JobDispatcher::set_name(const char* name, size_t len) {
    canary_.Assert();

    return name_.set(name, len);
}

zx_status_t JobDispatcher::get_importance(zx_job_importance_t* out) const {
    canary_.Assert();
    DEBUG_ASSERT(out != nullptr);

    // Find the importance value that we inherit.
    const JobDispatcher* job = this;
    do {
        zx_job_importance_t imp = job->GetRawImportance();
        if (imp != ZX_JOB_IMPORTANCE_INHERITED) {
            *out = imp;
            return ZX_OK;
        }
        // Don't need to use RefPtrs or grab locks: our caller has a reference
        // to |this|, and there's a const RefPtr chain from |this| to all
        // ancestors.
        job = job->parent_.get();
    } while (job != nullptr);

    // The root job should always have a non-INHERITED importance.
    PANIC("Could not find inherited importance of job %" PRIu64 "\n",
          get_koid());
}

// Does not resolve ZX_JOB_IMPORTANCE_INHERITED.
zx_job_importance_t JobDispatcher::GetRawImportance() const {
    canary_.Assert();
    AutoLock lock(&lock_);
    return importance_;
}

zx_status_t JobDispatcher::set_importance(zx_job_importance_t importance) {
    canary_.Assert();

    if ((importance < ZX_JOB_IMPORTANCE_MIN ||
         importance > ZX_JOB_IMPORTANCE_MAX) &&
        importance != ZX_JOB_IMPORTANCE_INHERITED) {
        return ZX_ERR_OUT_OF_RANGE;
    }
    AutoLock lock(&lock_);
    // No-one is allowed to change the importance of the root job.  Note that
    // the actual root job ("<superroot>") typically isn't seen by userspace, so
    // no userspace program should see this error.  The job that userspace calls
    // "root" is actually a child of the real (super) root job.
    if (parent_ == nullptr) {
        return ZX_ERR_ACCESS_DENIED;
    }
    importance_ = importance;
    return ZX_OK;
}

// Global importance ranking. Note that this is independent of
// zx_task_importance_t-style importance as far as JobDispatcher is concerned;
// some other entity will choose how to order importance_list_.
fbl::Mutex JobDispatcher::importance_lock_;
JobDispatcher::JobImportanceList JobDispatcher::importance_list_;

zx_status_t JobDispatcher::MakeMoreImportantThan(
    fbl::RefPtr<JobDispatcher> other) {

    canary_.Assert();
    if (other != nullptr)
        other->canary_.Assert();

    // Update this job's position in the global job importance list.
    AutoLock lock(&importance_lock_);
    DEBUG_ASSERT(dll_importance_.InContainer());
    DEBUG_ASSERT(other == nullptr || other->dll_importance_.InContainer());

    importance_list_.erase(*this);
    DEBUG_ASSERT(!dll_importance_.InContainer());
    if (other == nullptr) {
        // Make this the least important.
        importance_list_.push_front(this);
    } else {
        // Insert just after the less-important other job.
        importance_list_.insert_after(
            importance_list_.make_iterator(*other), this);
    }
    DEBUG_ASSERT(dll_importance_.InContainer());

    return ZX_OK;
}

zx_status_t JobDispatcher::SetExceptionPort(fbl::RefPtr<ExceptionPort> eport) {
    canary_.Assert();

    DEBUG_ASSERT(eport->type() == ExceptionPort::Type::JOB);

    AutoLock lock(&lock_);
    if (exception_port_)
        return ZX_ERR_BAD_STATE;
    exception_port_ = fbl::move(eport);

    return ZX_OK;
}

class OnExceptionPortRemovalEnumerator final : public JobEnumerator {
public:
    OnExceptionPortRemovalEnumerator(fbl::RefPtr<ExceptionPort> eport)
        : eport_(fbl::move(eport)) {}
    OnExceptionPortRemovalEnumerator(const OnExceptionPortRemovalEnumerator&) = delete;

private:
    bool OnProcess(ProcessDispatcher* process) override {
        process->OnExceptionPortRemoval(eport_);
        // Keep looking.
        return true;
    }

    fbl::RefPtr<ExceptionPort> eport_;
};

bool JobDispatcher::ResetExceptionPort(bool quietly) {
    canary_.Assert();

    fbl::RefPtr<ExceptionPort> eport;
    {
        AutoLock lock(&lock_);
        exception_port_.swap(eport);
        if (eport == nullptr) {
            // Attempted to unbind when no exception port is bound.
            return false;
        }
        // This method must guarantee that no caller will return until
        // OnTargetUnbind has been called on the port-to-unbind.
        // This becomes important when a manual unbind races with a
        // PortDispatcher::on_zero_handles auto-unbind.
        //
        // If OnTargetUnbind were called outside of the lock, it would lead to
        // a race (for threads A and B):
        //
        //   A: Calls ResetExceptionPort; acquires the lock
        //   A: Sees a non-null exception_port_, swaps it into the eport local.
        //      exception_port_ is now null.
        //   A: Releases the lock
        //
        //   B: Calls ResetExceptionPort; acquires the lock
        //   B: Sees a null exception_port_ and returns. But OnTargetUnbind()
        //      hasn't yet been called for the port.
        //
        // So, call it before releasing the lock.
        eport->OnTargetUnbind();
    }

    if (!quietly) {
        OnExceptionPortRemovalEnumerator remover(eport);
        if (!EnumerateChildren(&remover, true)) {
            DEBUG_ASSERT(false);
        }
    }
    return true;
}

fbl::RefPtr<ExceptionPort> JobDispatcher::exception_port() {
    AutoLock lock(&lock_);
    return exception_port_;
}
