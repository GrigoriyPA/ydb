#pragma once

#include <library/cpp/threading/future/future.h>

#include <functional>
#include <memory>

namespace NKikimr::NMiniKQL {

// Lightweight resume-on-future hook installed by the compute actor.
//
// Comp nodes performing async work (e.g. talking to a query actor) get back
// NThreading::TFuture<...> values from the work and Yield while waiting. To
// avoid the compute actor going idle and never re-polling, every such future
// must be Subscribe()-d to a wake-up callback that posts a resume event back
// to the compute actor (the same `[actorSystem, selfId]() { Send(TEvResumeExecution) }`
// closure that spilling already builds in kqp_pure_compute_actor.cpp).
//
// This factory's only job is to install that subscription on a future. Comp
// nodes do not construct the wake-up themselves; they just hand any future
// they want to wait on through SubscribeWakeUp() and continue.
class IComputeActorAsyncResume {
public:
    using TPtr = std::shared_ptr<IComputeActorAsyncResume>;

    virtual ~IComputeActorAsyncResume() = default;

    template <typename T>
    void SubscribeWakeUp(const NThreading::TFuture<T>& future) const {
        const auto cb = MakeWakeUpCallback();
        future.Subscribe([cb](const auto&) { cb(); });
    }

    virtual std::function<void()> MakeWakeUpCallback() const = 0;
};

} // namespace NKikimr::NMiniKQL
