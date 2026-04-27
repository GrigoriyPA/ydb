#pragma once

#include "dq_compute_actor_async_io.h"

#include <ydb/library/yql/dq/runtime/dq_channel_storage.h>

#include <yql/essentials/minikql/computation/mkql_compute_actor_async_resume.h>

#include <utility>

namespace NYql::NDq {

// Concrete IComputeActorAsyncResume backed by the same wake-up callback that
// spilling already uses (kqp_pure_compute_actor.cpp:92-95):
//
//   [actorSystem, selfId]() {
//       actorSystem->Send(selfId, new TEvDqCompute::TEvResumeExecution{
//           EResumeSource::CAWakeupCallback});
//   }
//
// The compute actor constructs that closure once and hands it here.
class TDqComputeActorAsyncResume : public NKikimr::NMiniKQL::IComputeActorAsyncResume {
public:
    explicit TDqComputeActorAsyncResume(TWakeUpCallback wakeUpCallback)
        : WakeUpCallback_(std::move(wakeUpCallback))
    {
    }

    std::function<void()> MakeWakeUpCallback() const override {
        return WakeUpCallback_;
    }

private:
    const TWakeUpCallback WakeUpCallback_;
};

} // namespace NYql::NDq
