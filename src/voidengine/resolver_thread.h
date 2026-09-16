// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

namespace D2HT::voidengine {

// The worker behind the engine-singleton resolver. It runs `sweep` on its own
// thread whenever nothing is published, and publishes whatever that returns.
//
// A sweep reads a gigabyte and more of committed memory, which is nothing once
// per map load and a visible hitch if it happens on the render thread. So the
// render thread only ever reads the published pointer, and clearing that pointer
// is what asks for another sweep.
//
// The sweep is handed in at construction and must own everything it touches by
// value. Nothing about the resolver that owns this object is reachable from the
// worker, which is what makes the detached thread in the destructor safe: the
// sweep was once a lambda capturing the resolver's `this`, and a teardown
// landing mid-sweep read its shape out of freed storage.
class ResolverThread {
public:
    // Returns the object it found, or null. `cancelled` goes true when teardown
    // wants the worker out of this module; a sweep that ignores it keeps the
    // module's code executing for the length of a whole scan.
    using Sweep = std::function<void*(const std::atomic<bool>& cancelled)>;

    explicit ResolverThread(Sweep sweep) : m_state(std::make_shared<State>()) {
        m_state->sweep = std::move(sweep);
        m_thread = std::thread(&ResolverThread::Worker, m_state);
    }

    // Note that in the shipped .asi this never runs: Mod is a deliberately leaked
    // singleton with a deleted destructor, so nothing above ever destroys the
    // resolver and the worker keeps sweeping for the life of the process. That
    // is the intended arrangement - the alternative is joining threads under the
    // loader lock - and what it means in practice is that `stop` is only ever
    // set by the unit tests. The correctness this destructor is written for is
    // therefore the test's, and the shipped build relies instead on the module
    // being pinned so the worker's code is never unmapped under it.
    ~ResolverThread() {
        m_state->stop.store(true);
        // Detached, never joined. This can be reached while the game is tearing
        // down, and joining a sweep from there is a wait of unbounded length in
        // a place that must not wait. The shared block below outlives whichever
        // of the two ends first, and the sweep observes `stop` per region, so
        // the worker leaves this module's code promptly and touches nothing that
        // has been freed on the way out.
        if (m_thread.joinable()) m_thread.detach();
    }

    ResolverThread(const ResolverThread&) = delete;
    ResolverThread& operator=(const ResolverThread&) = delete;

    void* Published() const { return m_state->object.load(); }
    void Clear() { m_state->object.store(nullptr); }

    // Publishes an object the caller resolved some other way. The worker only
    // sweeps while nothing is published, so this is also what stops it sweeping
    // every three seconds for an object that is already in hand.
    void Publish(void* object) { m_state->object.store(object); }

private:
    // Held by the worker as well as by the resolver, so it outlives whichever
    // of the two ends first.
    struct State {
        Sweep sweep;
        std::atomic<void*> object{nullptr};
        std::atomic<bool> stop{false};
    };

    // How long the worker waits between sweeps while it has nothing to publish,
    // and how often it looks at the stop flag while waiting. Sleeping the whole
    // interval in one go leaves a detached worker running inside this DLL for
    // up to three seconds after teardown asked it to stop.
    static constexpr std::chrono::milliseconds kSweepInterval{3000};
    static constexpr std::chrono::milliseconds kStopPollInterval{50};

    static void Worker(std::shared_ptr<State> state) {
        while (!state->stop.load()) {
            if (state->object.load() == nullptr) {
                if (void* found = state->sweep(state->stop)) state->object.store(found);
            }
            for (std::chrono::milliseconds waited{0};
                 waited < kSweepInterval && !state->stop.load(); waited += kStopPollInterval) {
                std::this_thread::sleep_for(kStopPollInterval);
            }
        }
    }

    std::shared_ptr<State> m_state;
    std::thread m_thread;
};

}  // namespace D2HT::voidengine
