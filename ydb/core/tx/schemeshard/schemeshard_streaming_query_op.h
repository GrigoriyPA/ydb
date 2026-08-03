#pragma once

#include <ydb/core/base/events.h>
#include <ydb/core/protos/schemeshard/streaming_query_op.pb.h>

#include <ydb/library/actors/core/event_pb.h>

namespace NKikimr {
namespace NSchemeShard {

// Shared SchemeShard<->KQP contract for the durable CREATE/ALTER/DROP STREAMING QUERY operation.
// Both the SchemeShard driver and the KQP-layer runner include this header.
struct TEvStreamingQuery {
    enum EEv {
        EvCreateOperationRequest = EventSpaceBegin(TKikimrEvents::ES_STREAMING_QUERY_SERVICE),
        EvCreateOperationResponse,
        EvRunOperationRequest,
        EvOperationResult,

        EvEnd
    };

    static_assert(
        EvEnd < EventSpaceEnd(TKikimrEvents::ES_STREAMING_QUERY_SERVICE),
        "expect EvEnd < EventSpaceEnd(TKikimrEvents::ES_STREAMING_QUERY_SERVICE)"
    );

#ifdef DECLARE_EVENT_CLASS
#error DECLARE_EVENT_CLASS macro redefinition
#else
#define DECLARE_EVENT_CLASS(NAME) struct T##NAME: public TEventPB<T##NAME, NKikimrStreamingQueryOp::T##NAME, NAME>
#endif

    DECLARE_EVENT_CLASS(EvCreateOperationRequest) {
        TEvCreateOperationRequest() = default;
    };

    DECLARE_EVENT_CLASS(EvCreateOperationResponse) {
        TEvCreateOperationResponse() = default;
    };

    DECLARE_EVENT_CLASS(EvRunOperationRequest) {
        TEvRunOperationRequest() = default;
    };

    DECLARE_EVENT_CLASS(EvOperationResult) {
        TEvOperationResult() = default;
    };

#undef DECLARE_EVENT_CLASS

}; // TEvStreamingQuery

} // NSchemeShard
} // NKikimr
