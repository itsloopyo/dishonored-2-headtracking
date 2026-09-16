// SPDX-License-Identifier: MIT
// Copyright (c) 2026 itsloopyo
#pragma once

#include <atomic>

namespace D2HT {

// Three floats and the flag that says whether they mean anything, published and
// read on the render thread, later in the same frame. The tracker feed and the
// mod each publish a rotation and a position through one of these; the units are
// the publisher's business and nothing here interprets them.
//
// The flag is stored last and loaded first, so a reader that sees it set has
// seen the three stores that came before it. The triple is NOT atomic as a
// unit - three separate loads can straddle two publishes - which is why the
// only consumer is the render thread reading back its own frame.
class PublishedTriple {
public:
    void Publish(float a, float b, float c, bool valid) {
        m_a.store(a, std::memory_order_release);
        m_b.store(b, std::memory_order_release);
        m_c.store(c, std::memory_order_release);
        m_valid.store(valid, std::memory_order_release);
    }

    void Invalidate() { m_valid.store(false, std::memory_order_release); }

    // False when nothing valid is published, in which case the outputs are left
    // exactly as the caller had them.
    bool Read(float& a, float& b, float& c) const {
        if (!m_valid.load(std::memory_order_acquire)) return false;
        a = m_a.load(std::memory_order_acquire);
        b = m_b.load(std::memory_order_acquire);
        c = m_c.load(std::memory_order_acquire);
        return true;
    }

private:
    std::atomic<float> m_a{0.0f};
    std::atomic<float> m_b{0.0f};
    std::atomic<float> m_c{0.0f};
    std::atomic<bool> m_valid{false};
};

}  // namespace D2HT
