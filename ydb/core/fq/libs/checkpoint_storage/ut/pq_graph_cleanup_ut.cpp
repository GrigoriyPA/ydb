#include <ydb/core/fq/libs/checkpoint_storage/pq_graph_cleanup.h>
#include <ydb/library/yql/providers/pq/proto/dq_io.pb.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NFq {
namespace {

using namespace NThreading;
using namespace NYdb;
using namespace NYdb::NTopic;
using namespace NYql;

class TClient : public IDeferredPublishClient {
public:
    std::vector<TPublicationSummary> Publications;
    TVector<ui64> Canceled;
    EStatus ListStatus = EStatus::SUCCESS;
    EStatus CancelStatus = EStatus::SUCCESS;

    TAsyncBeginPublicationResult BeginPublication(const TString&, const TBeginPublicationSettings&) override {
        UNIT_FAIL("Unexpected BeginPublication");
        return {};
    }

    TAsyncPublishResult Publish(const TDeferredPublication&, const TPublishSettings&) override {
        UNIT_FAIL("Unexpected Publish");
        return {};
    }

    TAsyncListPublicationsResult ListPublications(const TListPublicationsSettings& settings) override {
        UNIT_ASSERT(settings.WriterIdentity_);
        std::vector<TPublicationSummary> result;
        for (const auto& publication : Publications) {
            if (publication.WriterIdentity == settings.WriterIdentity_) {
                result.push_back(publication);
            }
        }
        return MakeFuture(TListPublicationsResult(TStatus(ListStatus, {}), std::move(result)));
    }

    TAsyncCancelPublicationResult CancelPublication(const TDeferredPublication& publication, const TCancelPublicationSettings&) override {
        Canceled.push_back(publication.IntPublicationId);
        return MakeFuture(TCancelPublicationResult(TStatus(CancelStatus, {})));
    }
};

class TGateway : public IPqStaticGateway {
public:
    const TIntrusivePtr<TClient> Client = MakeIntrusive<TClient>();

    IDeferredPublishClient::TPtr GetDeferredPublishClient(const TDriver&, const TCommonClientSettings& settings) override {
        UNIT_ASSERT_VALUES_EQUAL(*settings.Database_, "database");
        UNIT_ASSERT_VALUES_EQUAL((*settings.CredentialsProviderFactory_)->CreateProvider()->GetAuthInfo(), "test-token");
        return Client;
    }

    ITopicClient::TPtr GetTopicClient(const TDriver&, const TTopicClientSettings&) override { return {}; }
    IFederatedTopicClient::TPtr GetFederatedTopicClient(const TDriver&, const NFederatedTopic::TFederatedTopicClientSettings&) override { return {}; }
    TTopicClientSettings GetTopicClientSettings() const override { return {}; }
    NFederatedTopic::TFederatedTopicClientSettings GetFederatedTopicClientSettings() const override { return {}; }
};

NProto::TCheckpointGraphDescription MakeGraph() {
    NProto::TCheckpointGraphDescription graph;
    for (ui64 taskId : {1, 2}) {
        auto& task = *graph.MutableGraph()->AddTasks();
        task.SetId(taskId);
        (*task.MutableTaskParams())["current_execution_generation"] = "3";
        (*task.MutableSecureParams())["sink-token"] = TStructuredTokenBuilder().SetIAMToken("test-token").ToJson();
        for (int outputIndex = 0; outputIndex < 2; ++outputIndex) {
            NYql::NPq::NProto::TDqPqTopicSink sink;
            sink.SetDatabase("database");
            sink.MutableToken()->SetName("sink-token");
            sink.SetDeferredPublicationExtIdPrefix("query");
            task.AddOutputs()->MutableSink()->MutableSettings()->PackFrom(sink);
        }
        task.AddOutputs(); // Non-sink output.
        task.AddOutputs()->MutableSink()->MutableSettings()->PackFrom(NYql::NPq::NProto::TDqPqTopicSink{});
    }
    return graph;
}

} // namespace

Y_UNIT_TEST_SUITE(TPqCheckpointGraphCleanup) {
    Y_UNIT_TEST(CancelsAllGraphWritersAndPreservesOtherGenerations) {
        const auto gateway = MakeIntrusive<TGateway>();
        ui64 id = 0;
        for (const auto& writer : {"query:1:0:3", "query:1:0:3", "query:1:1:3", "query:2:0:3", "query:2:1:3",
                                  "query:1:0:4", "query:1:0:2", "other-query:1:0:3", "query:3:0:3"}) {
            TPublicationSummary publication;
            publication.IntPublicationId = ++id;
            publication.WriterIdentity = writer;
            gateway->Client->Publications.push_back(std::move(publication));
        }
        auto cleanup = CreatePqCheckpointGraphCleanup(gateway, TDriver(TDriverConfig{}), CreateStructuredTokenCredentialsFactory());
        const auto issues = cleanup(MakeGraph()).GetValueSync();
        UNIT_ASSERT_C(issues.Empty(), issues.ToString());
        UNIT_ASSERT_VALUES_EQUAL(gateway->Client->Canceled, (TVector<ui64>{1, 2, 3, 4, 5}));
    }

    Y_UNIT_TEST(PropagatesListingFailure) {
        const auto gateway = MakeIntrusive<TGateway>();
        gateway->Client->ListStatus = EStatus::UNAUTHORIZED;
        auto cleanup = CreatePqCheckpointGraphCleanup(gateway, TDriver(TDriverConfig{}), CreateStructuredTokenCredentialsFactory());
        UNIT_ASSERT(cleanup(MakeGraph()).GetValueSync());
        UNIT_ASSERT(gateway->Client->Canceled.empty());
    }

    Y_UNIT_TEST(CancellationCanBeRetriedAndIgnoresMissingPublications) {
        const auto gateway = MakeIntrusive<TGateway>();
        TPublicationSummary publication;
        publication.IntPublicationId = 1;
        publication.WriterIdentity = "query:1:0:3";
        gateway->Client->Publications.push_back(publication);
        gateway->Client->CancelStatus = EStatus::UNAVAILABLE;
        auto cleanup = CreatePqCheckpointGraphCleanup(gateway, TDriver(TDriverConfig{}), CreateStructuredTokenCredentialsFactory());
        UNIT_ASSERT(cleanup(MakeGraph()).GetValueSync());
        gateway->Client->CancelStatus = EStatus::NOT_FOUND;
        const auto issues = cleanup(MakeGraph()).GetValueSync();
        UNIT_ASSERT_C(issues.Empty(), issues.ToString());
        UNIT_ASSERT_VALUES_EQUAL(gateway->Client->Canceled, (TVector<ui64>{1, 1}));
    }
}

} // namespace NFq
