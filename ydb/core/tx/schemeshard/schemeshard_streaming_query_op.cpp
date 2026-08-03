#include "schemeshard_impl.h"

namespace NKikimr {
namespace NSchemeShard {

void TSchemeShard::AddStreamingQueryOperation(const TStreamingQueryOperationInfo::TPtr& opInfo) {
    StreamingQueryOperations[opInfo->Id] = opInfo;
    if (opInfo->TargetPathId) {
        StreamingQueryOperationByPath[opInfo->TargetPathId] = opInfo->Id;
    }
}

void TSchemeShard::PersistStreamingQueryOperation(NIceDb::TNiceDb& db, const TStreamingQueryOperationInfo& opInfo) {
    db.Table<Schema::StreamingQueryOperations>().Key(opInfo.Id).Update(
        NIceDb::TUpdate<Schema::StreamingQueryOperations::OwnerPathId>(opInfo.TargetPathId.OwnerId),
        NIceDb::TUpdate<Schema::StreamingQueryOperations::LocalPathId>(opInfo.TargetPathId.LocalPathId),
        NIceDb::TUpdate<Schema::StreamingQueryOperations::Kind>(static_cast<ui8>(opInfo.Kind)),
        NIceDb::TUpdate<Schema::StreamingQueryOperations::State>(static_cast<ui8>(opInfo.State)),
        NIceDb::TUpdate<Schema::StreamingQueryOperations::Round>(opInfo.Round),
        NIceDb::TUpdate<Schema::StreamingQueryOperations::Request>(opInfo.Request.SerializeAsString()),
        NIceDb::TUpdate<Schema::StreamingQueryOperations::Issue>(opInfo.Issue),
        NIceDb::TUpdate<Schema::StreamingQueryOperations::StartTime>(opInfo.StartTime.Seconds()),
        NIceDb::TUpdate<Schema::StreamingQueryOperations::EndTime>(opInfo.EndTime.Seconds()),
        NIceDb::TUpdate<Schema::StreamingQueryOperations::Database>(opInfo.Database),
        NIceDb::TUpdate<Schema::StreamingQueryOperations::UserToken>(opInfo.UserToken),
        NIceDb::TUpdate<Schema::StreamingQueryOperations::PeerName>(opInfo.PeerName)
    );
}

void TSchemeShard::PersistStreamingQueryOperationState(NIceDb::TNiceDb& db, const TStreamingQueryOperationInfo& opInfo) {
    db.Table<Schema::StreamingQueryOperations>().Key(opInfo.Id).Update(
        NIceDb::TUpdate<Schema::StreamingQueryOperations::State>(static_cast<ui8>(opInfo.State)),
        NIceDb::TUpdate<Schema::StreamingQueryOperations::Round>(opInfo.Round),
        NIceDb::TUpdate<Schema::StreamingQueryOperations::Issue>(opInfo.Issue),
        NIceDb::TUpdate<Schema::StreamingQueryOperations::EndTime>(opInfo.EndTime.Seconds())
    );
}

void TSchemeShard::PersistRemoveStreamingQueryOperation(NIceDb::TNiceDb& db, const TStreamingQueryOperationInfo& opInfo) {
    if (opInfo.TargetPathId) {
        auto it = StreamingQueryOperationByPath.find(opInfo.TargetPathId);
        if (it != StreamingQueryOperationByPath.end() && it->second == opInfo.Id) {
            StreamingQueryOperationByPath.erase(it);
        }
    }
    StreamingQueryOperations.erase(opInfo.Id);

    db.Table<Schema::StreamingQueryOperations>().Key(opInfo.Id).Delete();
}

void TSchemeShard::ResumeStreamingQueryOperations(const TVector<ui64>& opIds, const TActorContext& ctx) {
    // P1: scaffolding only. The runner (re)launch + progress driver land in P2
    // (CreateTxProgressStreamingQueryOp). For now this is a no-op so that a freshly-added
    // (empty) operations table has no effect on behavior.
    for (const ui64 id : opIds) {
        LOG_NOTICE_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
            "TSchemeShard::ResumeStreamingQueryOperations: pending runner relaunch (P2)"
                << ", id# " << id);
    }
}

} // NSchemeShard
} // NKikimr
