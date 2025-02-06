#include "utils.hpp"

#include <library/cpp/colorizer/colors.h>

#include <util/stream/file.h>
#include <util/string/builder.h>

#include <ydb-cpp-sdk/client/result/result.h>
#include <ydb/public/lib/json_value/ydb_json_value.h>

#include <yql/essentials/public/issue/yql_issue_message.h>

namespace NKikimrRun {

namespace {

void TerminateHandler() {
    NColorizer::TColors colors = NColorizer::AutoColors(Cerr);

    Cerr << colors.Red() << "======= terminate() call stack ========" << colors.Default() << Endl;
    FormatBackTrace(&Cerr);
    Cerr << colors.Red() << "=======================================" << colors.Default() << Endl;

    abort();
}


void SegmentationFaultHandler(int) {
    NColorizer::TColors colors = NColorizer::AutoColors(Cerr);

    Cerr << colors.Red() << "======= segmentation fault call stack ========" << colors.Default() << Endl;
    FormatBackTrace(&Cerr);
    Cerr << colors.Red() << "==============================================" << colors.Default() << Endl;

    abort();
}

void FloatingPointExceptionHandler(int) {
    NColorizer::TColors colors = NColorizer::AutoColors(Cerr);

    Cerr << colors.Red() << "======= floating point exception call stack ========" << colors.Default() << Endl;
    FormatBackTrace(&Cerr);
    Cerr << colors.Red() << "====================================================" << colors.Default() << Endl;

    abort();
}

}  // nonymous namespace


TRequestResult::TRequestResult()
    : Status(Ydb::StatusIds::STATUS_CODE_UNSPECIFIED)
{}

TRequestResult::TRequestResult(Ydb::StatusIds::StatusCode status, const NYql::TIssues& issues)
    : Status(status)
    , Issues(issues)
{}

TRequestResult::TRequestResult(Ydb::StatusIds::StatusCode status, const google::protobuf::RepeatedPtrField<Ydb::Issue::IssueMessage>& issues)
    : Status(status)
{
    NYql::IssuesFromMessage(issues, Issues);
}

bool TRequestResult::IsSuccess() const {
    return Status == Ydb::StatusIds::SUCCESS;
}

TString TRequestResult::ToString() const {
    return TStringBuilder() << "Request finished with status: " << Status << "\nIssues:\n" << Issues.ToString() << "\n";
}

TString LoadFile(const TString& file) {
    return TFileInput(file).ReadAll();
}

NKikimrServices::EServiceKikimr GetLogService(const TString& serviceName) {
    NKikimrServices::EServiceKikimr service;
    if (!NKikimrServices::EServiceKikimr_Parse(serviceName, &service)) {
        ythrow yexception() << "Invalid kikimr service name " << serviceName;
    }
    return service;
}

void ModifyLogPriorities(std::unordered_map<NKikimrServices::EServiceKikimr, NActors::NLog::EPriority> logPriorities, NKikimrConfig::TLogConfig& logConfig) {
    for (auto& entry : *logConfig.MutableEntry()) {
        const auto it = logPriorities.find(GetLogService(entry.GetComponent()));
        if (it != logPriorities.end()) {
            entry.SetLevel(it->second);
            logPriorities.erase(it);
        }
    }
    for (const auto& [service, priority] : logPriorities) {
        auto* entry = logConfig.AddEntry();
        entry->SetComponent(NKikimrServices::EServiceKikimr_Name(service));
        entry->SetLevel(priority);
    }
}

void InitLogSettings(const NKikimrConfig::TLogConfig& logConfig, NActors::TTestActorRuntimeBase& runtime) {
    if (logConfig.HasDefaultLevel()) {
        auto priority = NActors::NLog::EPriority(logConfig.GetDefaultLevel());
        auto descriptor = NKikimrServices::EServiceKikimr_descriptor();
        for (int i = 0; i < descriptor->value_count(); ++i) {
            runtime.SetLogPriority(static_cast<NKikimrServices::EServiceKikimr>(descriptor->value(i)->number()), priority);
        }
    }

    for (const auto& setting : logConfig.get_arr_entry()) {
        runtime.SetLogPriority(GetLogService(setting.GetComponent()), NActors::NLog::EPriority(setting.GetLevel()));
    }
}

void SetupSignalActions() {
    std::set_terminate(&TerminateHandler);
    signal(SIGSEGV, &SegmentationFaultHandler);
    signal(SIGFPE, &FloatingPointExceptionHandler);
}

void PrintResultSet(EResultOutputFormat format, IOutputStream& output, const Ydb::ResultSet& resultSet) {
    switch (format) {
        case EResultOutputFormat::RowsJson: {
            NYdb::TResultSet result(resultSet);
            NYdb::TResultSetParser parser(result);
            while (parser.TryNextRow()) {
                NJsonWriter::TBuf writer(NJsonWriter::HEM_UNSAFE, &output);
                writer.SetWriteNanAsString(true);
                NYdb::FormatResultRowJson(parser, result.GetColumnsMeta(), writer, NYdb::EBinaryStringEncoding::Unicode);
                output << Endl;
            }
            break;
        }

        case EResultOutputFormat::FullJson: {
            resultSet.PrintJSON(output);
            output << Endl;
            break;
        }

        case EResultOutputFormat::FullProto: {
            TString resultSetString;
            google::protobuf::TextFormat::Printer printer;
            printer.SetSingleLineMode(false);
            printer.SetUseUtf8StringEscaping(true);
            printer.PrintToString(resultSet, &resultSetString);
            output << resultSetString;
            break;
        }
    }
}

}  // namespace NKikimrRun
