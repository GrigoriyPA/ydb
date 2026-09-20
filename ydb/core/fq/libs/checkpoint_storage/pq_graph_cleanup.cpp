#include "pq_graph_cleanup.h"

#include <ydb/library/yql/providers/pq/proto/dq_io.pb.h>
#include <ydb/public/sdk/cpp/adapters/issue/issue.h>

#include <util/string/builder.h>

namespace NFq {
namespace {

using namespace NThreading;
using namespace NYql;
using namespace NYdb::NTopic;

TIssues CleanupError(const TString& writerIdentity, const NYdb::TStatus& status) {
    TIssue issue(TStringBuilder() << "Failed to clean up publications for writer '" << writerIdentity << "', status: " << status.GetStatus());
    for (const auto& subIssue : status.GetIssues()) {
        issue.AddSubIssue(MakeIntrusive<TIssue>(NYdb::NAdapters::ToYqlIssue(subIssue)));
    }
    return {issue};
}

TFuture<TIssues> CleanupWriter(IDeferredPublishClient::TPtr client, const TString& writerIdentity, const TString& generation) {
    const TString publicationPrefix = TStringBuilder() << writerIdentity << ':' << generation << ':';
    return client->ListPublications(TListPublicationsSettings().WriterIdentity(writerIdentity)).Apply(
        [client, writerIdentity, publicationPrefix](const TAsyncListPublicationsResult& future) {
            const auto& result = future.GetValue();
            if (!result.IsSuccess()) {
                return MakeFuture(CleanupError(writerIdentity, result));
            }

            auto cleanup = MakeFuture(TIssues{});
            for (const auto& publication : result.GetPublications()) {
                // Writer identities are shared across generations. GC must only cancel
                // publications belonging to the graph whose checkpoints were removed.
                if (!TStringBuf(publication.ExtPublicationId).StartsWith(publicationPrefix)) {
                    continue;
                }
                cleanup = cleanup.Apply([client, writerIdentity, id = publication.IntPublicationId](const TFuture<TIssues>& future) {
                    if (future.GetValue()) {
                        return future;
                    }
                    return client->CancelPublication(TDeferredPublication(id)).Apply(
                        [client, writerIdentity](const TAsyncCancelPublicationResult& future) {
                            const auto& result = future.GetValue();
                            if (result.IsSuccess() || result.GetStatus() == NYdb::EStatus::NOT_FOUND) {
                                return TIssues{};
                            }
                            return CleanupError(writerIdentity, result);
                        });
                });
            }
            return cleanup;
        });
}

} // namespace

TCheckpointGraphCleanup CreatePqCheckpointGraphCleanup(
    IPqStaticGateway::TPtr pqGateway,
    NYdb::TDriver driver,
    IStructuredTokenCredentialsFactory::TPtr credentialsFactory)
{
    return [pqGateway = std::move(pqGateway), driver = std::move(driver), credentialsFactory = std::move(credentialsFactory)](
        const NProto::TCheckpointGraphDescription& graphDesc)
    {
        auto cleanup = MakeFuture(TIssues{});
        for (const auto& task : graphDesc.GetGraph().GetTasks()) {
            for (size_t outputIndex = 0; outputIndex < task.OutputsSize(); ++outputIndex) {
                const auto& output = task.GetOutputs(outputIndex);
                NYql::NPq::NProto::TDqPqTopicSink sink;
                if (!output.GetSink().GetSettings().UnpackTo(&sink) || !sink.GetDeferredPublicationExtIdPrefix()) {
                    continue;
                }

                const auto generation = task.GetTaskParams().find("current_execution_generation");
                if (generation == task.GetTaskParams().end()) {
                    return MakeFuture(TIssues{TIssue("Missing execution generation for checkpoint graph publication cleanup")});
                }
                const TString writerIdentity = TStringBuilder() << sink.GetDeferredPublicationExtIdPrefix()
                    << ':' << task.GetId() << ':' << outputIndex;
                TString token;
                if (const auto it = task.GetSecureParams().find(sink.GetToken().GetName()); it != task.GetSecureParams().end()) {
                    token = it->second;
                }
                cleanup = cleanup.Apply([pqGateway, driver, credentialsFactory, sink, token, writerIdentity, generation = generation->second](const TFuture<TIssues>& future) {
                    if (future.GetValue()) {
                        return future;
                    }
                    auto client = pqGateway->GetDeferredPublishClient(driver, NYdb::TCommonClientSettings()
                        .Database(sink.GetDatabase())
                        .DiscoveryEndpoint(sink.GetEndpoint())
                        .SslCredentials(NYdb::TSslCredentials(sink.GetUseSsl()))
                        .CredentialsProviderFactory(credentialsFactory->Create(token, sink.GetAddBearerToToken())));
                    return CleanupWriter(std::move(client), writerIdentity, generation);
                });
            }
        }
        return cleanup;
    };
}

} // namespace NFq
