#include "taxbroker/api/answer_json.hpp"
#include "taxbroker/api/request_json.hpp"

#include <spdlog/spdlog.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

/// The question types an answer can be read for, one request each.
std::vector<DecisionRequest> allRequests() {
    const auto make = [](RequestType aType, QuestionData aData) {
        return DecisionRequest{.mId = "request-0001",
                               .mType = aType,
                               .mRunRevision = 1,
                               .mData = std::move(aData)};
    };

    return {
        make(RequestType::ExchangeRatesRequired, std::monostate{}),
        make(RequestType::LossEligibilityConfirmationRequired, LossEligibilityQuestion{}),
        make(RequestType::CorporateActionIdentityRequired, CorporateActionIdentityQuestion{}),
        make(RequestType::CorporateActionIsinChangeRequired, IsinChangeQuestion{}),
        make(RequestType::CorporateActionRatioRequired, CorporateActionRatioQuestion{}),
        make(RequestType::TaxClassificationRequired, TaxClassificationQuestion{}),
        make(RequestType::TaxClassificationRequired, IncomeClassificationQuestion{}),
        make(RequestType::QuantityCorrectionRequired, QuantityCorrectionQuestion{}),
        make(RequestType::IncomeDetailsRequired, PayerChoiceQuestion{}),
        make(RequestType::IncomeDetailsRequired, PayerCountryQuestion{}),
        make(RequestType::IncomeDetailsRequired, TreatyClaimQuestion{}),
        make(RequestType::IncomeDetailsRequired, BondTypeQuestion{}),
        make(RequestType::DepositInterestTotalRequired, DepositInterestTotalQuestion{}),
        make(RequestType::CapitalGainsFailureResolutionRequired, FailureResolutionQuestion{}),
        make(RequestType::DividendsFailureResolutionRequired, FailureResolutionQuestion{}),
        make(RequestType::DeveloperExportConfirmationRequired, DeveloperExportQuestion{}),
    };
}

} // namespace

/// Turns logging off once, so the fuzzer output stays readable.
extern "C" int LLVMFuzzerInitialize(int*, char***) {
    spdlog::set_level(spdlog::level::off);

    return 0;
}

/// Reads one input as the JSON of an answer for every question type, as the body of an answer call
/// and of a delete call, and as the `request` part of an upload. None of them may crash.
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* aData, std::size_t aSize) {
    static const std::vector<DecisionRequest> requests = allRequests();

    const std::string_view text{reinterpret_cast<const char*>(aData), aSize};

    for (const DecisionRequest& request : requests)
    {
        (void)api::parseAnswerJson(text, request);
    }

    ApplicationResult result;
    result.mRunRevision = 1;
    result.mRequests = requests;

    for (std::size_t index = 0; index < result.mRequests.size(); ++index)
    {
        result.mRequests[index].mId = "request-" + std::to_string(index + 1);
    }

    (void)api::parseAnswerCallJson(text, result);
    (void)api::parseDeleteCallJson(text);

    const std::vector<api::UploadedPart> parts{
        {.mName = "request", .mFileName = std::nullopt, .mContent = text},
        {.mName = "file0", .mFileName = "export.csv", .mContent = "x"},
    };

    (void)api::buildApplicationRequest(parts);

    return 0;
}
