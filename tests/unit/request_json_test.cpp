#include "taxbroker/api/request_json.hpp"

#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;
using taxbroker::api::buildApplicationRequest;
using taxbroker::api::RunRequestError;
using taxbroker::api::RunRequestRefusal;
using taxbroker::api::UploadedPart;

/// The error of a failed build; the test fails when the request was accepted.
RunRequestError errorOf(const std::variant<ApplicationRequest, RunRequestRefusal>& aResult) {
    EXPECT_TRUE(std::holds_alternative<RunRequestRefusal>(aResult));

    if (!std::holds_alternative<RunRequestRefusal>(aResult))
    {
        return RunRequestError::NoBrokerFile;
    }

    return std::get<RunRequestRefusal>(aResult).mError;
}

TEST(RequestJsonTest, ReadsTheYearThePartsAndTheTaxpayer) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "developerMode": false,
        "taxpayer": {"taxNumber": "12345679", "isResident": true, "name": "Ana Novak"},
        "files": [
            {"part": "file0", "kind": "broker", "broker": "trade_republic"},
            {"part": "file1", "kind": "ecb_rates"},
            {"part": "file2", "kind": "reference"}
        ]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export_2023_2024.csv", .mContent = "broker bytes"},
        {.mName = "file1", .mFileName = "rates.csv", .mContent = "rate bytes"},
        {.mName = "file2", .mFileName = "payers.csv", .mContent = "reference bytes"},
    };

    const auto result = buildApplicationRequest(parts);

    ASSERT_TRUE(std::holds_alternative<ApplicationRequest>(result));

    const auto& request = std::get<ApplicationRequest>(result);

    EXPECT_EQ(request.mReportingYear, 2024);
    EXPECT_EQ(request.mFilingType, FilingType::Original);
    EXPECT_FALSE(request.mDeveloperMode);
    EXPECT_EQ(request.mTaxpayer.mTaxNumber, "12345679");
    EXPECT_EQ(request.mTaxpayer.mName, "Ana Novak");

    ASSERT_EQ(request.mBrokerFiles.size(), 1U);

    EXPECT_EQ(request.mBrokerFiles[0].mBroker, Broker::TradeRepublic);
    EXPECT_EQ(request.mBrokerFiles[0].mFile.mName, "export_2023_2024.csv");
    EXPECT_EQ(request.mBrokerFiles[0].mFile.mContent, "broker bytes");

    ASSERT_EQ(request.mEcbRateFiles.size(), 1U);

    EXPECT_EQ(request.mEcbRateFiles[0].mName, "rates.csv");
    EXPECT_EQ(request.mEcbRateFiles[0].mContent, "rate bytes");

    ASSERT_EQ(request.mReferenceFiles.size(), 1U);

    EXPECT_EQ(request.mReferenceFiles[0].mName, "payers.csv");
    EXPECT_EQ(request.mReferenceFiles[0].mContent, "reference bytes");
}

TEST(RequestJsonTest, KeepsTheOrderOfTheBrokerFilesAsListed) {
    // file1 comes first in the list although file0 comes first in the upload.
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [
            {"part": "file1", "kind": "broker", "broker": "trade_republic"},
            {"part": "file0", "kind": "broker", "broker": "trade_republic"}
        ]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "first_in_upload.csv", .mContent = "a"},
        {.mName = "file1", .mFileName = "first_in_list.csv", .mContent = "b"},
    };

    const auto result = buildApplicationRequest(parts);

    ASSERT_TRUE(std::holds_alternative<ApplicationRequest>(result));

    const auto& request = std::get<ApplicationRequest>(result);

    ASSERT_EQ(request.mBrokerFiles.size(), 2U);

    EXPECT_EQ(request.mBrokerFiles[0].mFile.mName, "first_in_list.csv");
    EXPECT_EQ(request.mBrokerFiles[1].mFile.mName, "first_in_upload.csv");
}

TEST(RequestJsonTest, ReadsASelfReportInDeveloperMode) {
    const std::string json = R"({
        "reportingYear": 2025,
        "filingType": "self_report",
        "developerMode": true,
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    const auto result = buildApplicationRequest(parts);

    ASSERT_TRUE(std::holds_alternative<ApplicationRequest>(result));

    EXPECT_EQ(std::get<ApplicationRequest>(result).mReportingYear, 2025);
    EXPECT_EQ(std::get<ApplicationRequest>(result).mFilingType, FilingType::SelfReport);
    EXPECT_TRUE(std::get<ApplicationRequest>(result).mDeveloperMode);
}

TEST(RequestJsonTest, YearBoundsAre2000And9999) {
    const std::string first = R"({
        "reportingYear": 2000,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::string last = R"({
        "reportingYear": 9999,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> firstParts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = first},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };
    const std::vector<UploadedPart> lastParts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = last},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    const auto firstResult = buildApplicationRequest(firstParts);
    const auto lastResult = buildApplicationRequest(lastParts);

    ASSERT_TRUE(std::holds_alternative<ApplicationRequest>(firstResult));
    ASSERT_TRUE(std::holds_alternative<ApplicationRequest>(lastResult));

    EXPECT_EQ(std::get<ApplicationRequest>(firstResult).mReportingYear, 2000);
    EXPECT_EQ(std::get<ApplicationRequest>(lastResult).mReportingYear, 9999);
}

TEST(RequestJsonTest, YearBelow2000IsRefused) {
    const std::string json = R"({
        "reportingYear": 1999,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, YearAbove9999IsRefused) {
    const std::string json = R"({
        "reportingYear": 10000,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, YearAsTextIsRefused) {
    const std::string json = R"({
        "reportingYear": "2024",
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, UnknownFilingTypeIsRefused) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "amended",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, MissingFilingTypeIsRefused) {
    const std::string json = R"({
        "reportingYear": 2024,
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, DeveloperModeAsTextIsRefused) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "developerMode": "yes",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, TaxpayerThatTheTaxpayerReaderRefusesIsBadTaxpayerWithItsField) {
    // The tax number has seven digits.
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "1234567", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    const auto result = buildApplicationRequest(parts);

    EXPECT_EQ(errorOf(result), RunRequestError::BadTaxpayer);
    EXPECT_EQ(std::get<RunRequestRefusal>(result).mField, "taxNumber");
}

TEST(RequestJsonTest, TaxpayerWithAnUnknownKeyIsBadTaxpayerWithoutAField) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true, "shoeSize": "42"},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    const auto result = buildApplicationRequest(parts);

    EXPECT_EQ(errorOf(result), RunRequestError::BadTaxpayer);
    EXPECT_EQ(std::get<RunRequestRefusal>(result).mField, "");
}

TEST(RequestJsonTest, TextThatIsNotJsonIsBadRequest) {
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = "not json"},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, UploadWithoutTheRequestPartIsBadRequest) {
    const std::vector<UploadedPart> parts{
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, PathKeyAtTheTopIsRefused) {
    const std::string json = R"({
        "path": "/home/x",
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::PathNotAllowed);
}

TEST(RequestJsonTest, PathKeyInTheTaxpayerIsRefused) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true, "path": "/home/x"},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::PathNotAllowed);
}

TEST(RequestJsonTest, PathKeyInAFileEntryIsRefused) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic", "Path": "/home/x"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::PathNotAllowed);
}

TEST(RequestJsonTest, UnknownTopLevelKeyIsRefused) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "extra": 1,
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, UnknownKeyInAFileEntryIsRefused) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic", "extra": 1}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, NamedPartThatIsNotUploadedIsMissing) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file9", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::MissingPart);
}

TEST(RequestJsonTest, UploadedPartThatNoEntryNamesIsUnreferenced) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
        {.mName = "file1", .mFileName = "other.csv", .mContent = "b"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::UnreferencedPart);
}

TEST(RequestJsonTest, TwoEntriesForOnePartAreADuplicate) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [
            {"part": "file0", "kind": "broker", "broker": "trade_republic"},
            {"part": "file0", "kind": "ecb_rates"}
        ]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::DuplicatePart);
}

TEST(RequestJsonTest, TwoUploadedPartsWithOneNameAreADuplicate) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
        {.mName = "file0", .mFileName = "again.csv", .mContent = "b"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::DuplicatePart);
}

TEST(RequestJsonTest, BrokerThatIsNotReadIsUnsupported) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "ibkr"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::UnsupportedBroker);
}

TEST(RequestJsonTest, BrokerFileWithoutABrokerIsBadRequest) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, RateFileWithABrokerIsBadRequest) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "ecb_rates", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "rates.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, UnknownFileKindIsBadRequest) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "picture"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, UploadWithOnlyRateFilesHasNoBrokerFile) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "ecb_rates"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "rates.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::NoBrokerFile);
}

TEST(RequestJsonTest, ExactlyThe32FilesOfTheLimitAreAccepted) {
    std::string files;
    std::vector<UploadedPart> parts;
    std::string json;

    for (int index = 0; index < 32; ++index)
    {
        files += std::string(index == 0 ? "" : ",") + R"({"part": "f)" + std::to_string(index) +
                 R"(", "kind": "broker", "broker": "trade_republic"})";
    }

    json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [)" +
           files + "]}";
    parts.push_back({.mName = "request", .mFileName = std::nullopt, .mContent = json});

    for (int index = 0; index < 32; ++index)
    {
        parts.push_back(
            {.mName = "f" + std::to_string(index), .mFileName = "a.csv", .mContent = "a"});
    }

    const auto result = buildApplicationRequest(parts);

    ASSERT_TRUE(std::holds_alternative<ApplicationRequest>(result));
    EXPECT_EQ(std::get<ApplicationRequest>(result).mBrokerFiles.size(), 32U);
}

TEST(RequestJsonTest, MoreThan32FilesAreTooManyFiles) {
    std::string files;

    for (int index = 0; index < 33; ++index)
    {
        files += std::string(index == 0 ? "" : ",") + R"({"part": "f)" + std::to_string(index) +
                 R"(", "kind": "broker", "broker": "trade_republic"})";
    }

    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [)" + files +
                             "]}";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::TooManyFiles);
}

TEST(RequestJsonTest, DeeplyNestedJsonIsBadRequestAndDoesNotOverflowTheStack) {
    const std::string json = std::string(100000, '[') + std::string(100000, ']');
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, FileNameWithAWindowsPathKeepsOnlyTheLastPart) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "C:\\Users\\someone\\export.csv", .mContent = "a"},
    };

    const auto result = buildApplicationRequest(parts);

    ASSERT_TRUE(std::holds_alternative<ApplicationRequest>(result));

    EXPECT_EQ(std::get<ApplicationRequest>(result).mBrokerFiles[0].mFile.mName, "export.csv");
}

TEST(RequestJsonTest, FileNameWithAUnixPathKeepsOnlyTheLastPart) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "/home/someone/export.csv", .mContent = "a"},
    };

    const auto result = buildApplicationRequest(parts);

    ASSERT_TRUE(std::holds_alternative<ApplicationRequest>(result));

    EXPECT_EQ(std::get<ApplicationRequest>(result).mBrokerFiles[0].mFile.mName, "export.csv");
}

TEST(RequestJsonTest, FileNameThatIsOnlyAFolderBecomesEmpty) {
    // The run reports an empty name as an invalid file name; the request does not decide.
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "/home/someone/", .mContent = "a"},
    };

    const auto result = buildApplicationRequest(parts);

    ASSERT_TRUE(std::holds_alternative<ApplicationRequest>(result));

    EXPECT_EQ(std::get<ApplicationRequest>(result).mBrokerFiles[0].mFile.mName, "");
}

TEST(RequestJsonTest, NestingDeeperThanEightLevelsIsBadRequest) {
    const std::string json = R"({"extra":[[[[[[[[[[1]]]]]]]]]]})";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, MissingTaxpayerIsBadRequest) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, FileEntryWithoutAKindIsBadRequest) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, BrokerThatIsNotATextIsBadRequest) {
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": 5}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "a"},
    };

    EXPECT_EQ(errorOf(buildApplicationRequest(parts)), RunRequestError::BadRequest);
}

TEST(RequestJsonTest, FilePartWithoutAFileNameGetsAnEmptyName) {
    // The run reports an empty name as an invalid file name; the request does not decide.
    const std::string json = R"({
        "reportingYear": 2024,
        "filingType": "original",
        "taxpayer": {"taxNumber": "12345679", "isResident": true},
        "files": [{"part": "file0", "kind": "broker", "broker": "trade_republic"}]
    })";
    const std::vector<UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = json},
        {.mName = "file0", .mFileName = std::nullopt, .mContent = "a"},
    };

    const auto result = buildApplicationRequest(parts);

    ASSERT_TRUE(std::holds_alternative<ApplicationRequest>(result));

    EXPECT_EQ(std::get<ApplicationRequest>(result).mBrokerFiles[0].mFile.mName, "");
}

TEST(RequestJsonTest, EveryErrorHasItsCode) {
    EXPECT_EQ(api::runRequestErrorCode(RunRequestError::BadRequest), "bad_request");
    EXPECT_EQ(api::runRequestErrorCode(RunRequestError::PathNotAllowed), "path_not_allowed");
    EXPECT_EQ(api::runRequestErrorCode(RunRequestError::NoBrokerFile), "no_broker_file");
    EXPECT_EQ(api::runRequestErrorCode(RunRequestError::MissingPart), "missing_part");
    EXPECT_EQ(api::runRequestErrorCode(RunRequestError::UnreferencedPart), "unreferenced_part");
    EXPECT_EQ(api::runRequestErrorCode(RunRequestError::DuplicatePart), "duplicate_part");
    EXPECT_EQ(api::runRequestErrorCode(RunRequestError::TooManyFiles), "too_many_files");
    EXPECT_EQ(api::runRequestErrorCode(RunRequestError::UnsupportedBroker), "unsupported_broker");
    EXPECT_EQ(api::runRequestErrorCode(RunRequestError::BadTaxpayer), "bad_taxpayer");
}

} // namespace
