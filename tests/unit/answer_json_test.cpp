#include "taxbroker/api/answer_json.hpp"

#include "taxbroker/text_format.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <variant>

namespace {
using namespace taxbroker;
using taxbroker::api::AnswerCall;
using taxbroker::api::CallBodyError;
using taxbroker::api::DeleteCall;
using taxbroker::api::parseAnswerCallJson;
using taxbroker::api::parseAnswerJson;
using taxbroker::api::parseDeleteCallJson;

/// A request of the given type and question data. The reader looks at nothing else.
DecisionRequest requestOf(RequestType aType, QuestionData aData) {
    return DecisionRequest{.mId = "request-0001",
                           .mType = aType,
                           .mRunRevision = 1,
                           .mData = std::move(aData)};
}

/// The answer, or nothing when the JSON does not fit the question.
std::optional<AnswerValue> fit(std::string_view aJson, const DecisionRequest& aRequest) {
    const auto parsed = parseAnswerJson(aJson, aRequest);

    if (std::holds_alternative<AnswerValue>(parsed))
    {
        return std::get<AnswerValue>(parsed);
    }

    return std::nullopt;
}

const DecisionRequest kLossRequest =
    requestOf(RequestType::LossEligibilityConfirmationRequired, LossEligibilityQuestion{});
const DecisionRequest kRatioRequest =
    requestOf(RequestType::CorporateActionRatioRequired, CorporateActionRatioQuestion{});

// ---- One test per row of the table of answer forms (docs/diagnostics.md)

TEST(AnswerJsonTest, LossQuestionReadsBothAnswersAndCancel) {
    EXPECT_EQ(fit(R"("no_other_activity")", kLossRequest), AnswerValue{NoOtherActivityAnswer{}});
    EXPECT_EQ(fit(R"("other_activity")", kLossRequest), AnswerValue{OtherActivityAnswer{}});
    EXPECT_EQ(fit(R"("cancel")", kLossRequest), AnswerValue{CancelAnswer{}});
}

TEST(AnswerJsonTest, RatioQuestionReadsTheRatioObject) {
    const auto answer = fit(R"({"newShares":2,"oldShares":1,"effectiveDate":"2024-06-01",
                                "confirmedUnits":"6.00000000"})",
                            kRatioRequest);

    // 6.00000000 units at 8 decimals are 600'000'000.
    const AnswerValue expected =
        SplitRatioAnswer{.mNewShares = 2,
                         .mOldShares = 1,
                         .mEffectiveDate = *parseCalendarDate("2024-06-01"),
                         .mConfirmedUnits = 600'000'000};

    EXPECT_EQ(answer, expected);
}

TEST(AnswerJsonTest, RatioQuestionReadsSeparateEventsAndCancel) {
    EXPECT_EQ(fit(R"("separate_events")", kRatioRequest), AnswerValue{SeparateEventsAnswer{}});
    EXPECT_EQ(fit(R"("cancel")", kRatioRequest), AnswerValue{CancelAnswer{}});
}

TEST(AnswerJsonTest, RatioUnitsWithFewerDecimalsAreAccepted) {
    const auto answer = fit(R"({"newShares":2,"oldShares":1,"effectiveDate":"2024-06-01",
                                "confirmedUnits":"6"})",
                            kRatioRequest);

    ASSERT_TRUE(answer.has_value());

    EXPECT_EQ(std::get<SplitRatioAnswer>(*answer).mConfirmedUnits, 600'000'000); // 6 units
}

TEST(AnswerJsonTest, RatioUnitsWithNineDecimalsAreRefused) {
    EXPECT_EQ(fit(R"({"newShares":2,"oldShares":1,"effectiveDate":"2024-06-01",
                      "confirmedUnits":"6.123456789"})",
                  kRatioRequest),
              std::nullopt);
}

TEST(AnswerJsonTest, ClassificationQuestionReadsBothAnswers) {
    const auto request =
        requestOf(RequestType::TaxClassificationRequired, TaxClassificationQuestion{});

    EXPECT_EQ(fit(R"("capital_gains")", request), AnswerValue{CapitalGainsAnswer{}});
    EXPECT_EQ(fit(R"("exclude_from_supported_reports")", request),
              AnswerValue{ExcludeFromReportsAnswer{}});
}

TEST(AnswerJsonTest, IncomeClassificationQuestionReadsTheFiveForms) {
    const auto request =
        requestOf(RequestType::TaxClassificationRequired, IncomeClassificationQuestion{});

    EXPECT_EQ(fit(R"("dividends")", request),
              AnswerValue{IncomeTreatmentAnswer{IncomeTreatment::Dividends}});
    EXPECT_EQ(fit(R"("bank_deposit_interest")", request),
              AnswerValue{IncomeTreatmentAnswer{IncomeTreatment::BankDepositInterest}});
    EXPECT_EQ(fit(R"({"choice":"other_interest","subtype":"broker_cash"})", request),
              AnswerValue{IncomeTreatmentAnswer{IncomeTreatment::BrokerCashInterest}});
    EXPECT_EQ(fit(R"({"choice":"other_interest","subtype":"bond_coupon"})", request),
              AnswerValue{IncomeTreatmentAnswer{IncomeTreatment::BondCoupon}});
    EXPECT_EQ(fit(R"("exclude_from_supported_reports")", request),
              AnswerValue{ExcludeFromReportsAnswer{}});
}

TEST(AnswerJsonTest, QuantityQuestionReadsTheCorrectedUnits) {
    const auto request =
        requestOf(RequestType::QuantityCorrectionRequired, QuantityCorrectionQuestion{});

    // 0.99999998 units at 8 decimals are 99'999'998.
    EXPECT_EQ(fit(R"({"correctedUnits":"0.99999998"})", request),
              AnswerValue{QuantityCorrectionAnswer{99'999'998}});
}

TEST(AnswerJsonTest, PayerQuestionReadsAnOptionOrSkip) {
    const auto request = requestOf(RequestType::IncomeDetailsRequired, PayerChoiceQuestion{});

    EXPECT_EQ(fit(R"({"option":1})", request), AnswerValue{PayerChoiceAnswer{std::size_t{1}}});
    EXPECT_EQ(fit(R"("skip")", request), AnswerValue{PayerChoiceAnswer{std::nullopt}});
}

TEST(AnswerJsonTest, DividendTreatyQuestionReadsATextOrNoClaim) {
    const auto request = requestOf(RequestType::IncomeDetailsRequired, TreatyClaimQuestion{});

    EXPECT_EQ(fit(R"({"statement":"Konvencija SI-US, 10. člen"})", request),
              AnswerValue{TreatyClaimAnswer{std::string{"Konvencija SI-US, 10. člen"}}});
    EXPECT_EQ(fit(R"("no_claim")", request), AnswerValue{TreatyClaimAnswer{std::nullopt}});
}

TEST(AnswerJsonTest, InterestTreatyQuestionReadsClaimOrNoClaim) {
    const auto request =
        requestOf(RequestType::IncomeDetailsRequired, InterestTreatyClaimQuestion{});

    EXPECT_EQ(fit(R"("claim")", request), AnswerValue{InterestTreatyClaimAnswer{true}});
    EXPECT_EQ(fit(R"("no_claim")", request), AnswerValue{InterestTreatyClaimAnswer{false}});
}

TEST(AnswerJsonTest, BondTypeQuestionReadsBothTypes) {
    const auto request = requestOf(RequestType::IncomeDetailsRequired, BondTypeQuestion{});

    EXPECT_EQ(fit(R"("ordinary")", request), AnswerValue{BondTypeAnswer{BondType::Ordinary}});
    EXPECT_EQ(fit(R"("discount_or_zero_coupon")", request),
              AnswerValue{BondTypeAnswer{BondType::DiscountOrZeroCoupon}});
}

TEST(AnswerJsonTest, DepositQuestionReadsBothAnswers) {
    const auto request =
        requestOf(RequestType::DepositInterestTotalRequired, DepositInterestTotalQuestion{});

    EXPECT_EQ(fit(R"("above_limit")", request), AnswerValue{DepositInterestTotalAnswer{true}});
    EXPECT_EQ(fit(R"("not_above_limit")", request), AnswerValue{DepositInterestTotalAnswer{false}});
}

TEST(AnswerJsonTest, CapitalFailureQuestionReadsThreeChoices) {
    const auto request =
        requestOf(RequestType::CapitalGainsFailureResolutionRequired, FailureResolutionQuestion{});

    EXPECT_EQ(fit(R"("exclude_failed_instruments")", request),
              AnswerValue{FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments}});
    EXPECT_EQ(fit(R"("fail_report")", request),
              AnswerValue{FailureResolutionAnswer{FailureResolution::FailReport}});
    EXPECT_EQ(fit(R"("developer_export")", request),
              AnswerValue{FailureResolutionAnswer{FailureResolution::DeveloperExport}});
}

TEST(AnswerJsonTest, DividendsAndOtherInterestFailureQuestionsReadTwoChoices) {
    const auto dividends =
        requestOf(RequestType::DividendsFailureResolutionRequired, FailureResolutionQuestion{});
    const auto otherInterest =
        requestOf(RequestType::OtherInterestFailureResolutionRequired, FailureResolutionQuestion{});

    EXPECT_EQ(fit(R"("exclude_failed_instruments")", dividends),
              AnswerValue{FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments}});
    EXPECT_EQ(fit(R"("fail_report")", dividends),
              AnswerValue{FailureResolutionAnswer{FailureResolution::FailReport}});
    EXPECT_EQ(fit(R"("exclude_failed_instruments")", otherInterest),
              AnswerValue{FailureResolutionAnswer{FailureResolution::ExcludeFailedInstruments}});
    EXPECT_EQ(fit(R"("fail_report")", otherInterest),
              AnswerValue{FailureResolutionAnswer{FailureResolution::FailReport}});
}

TEST(AnswerJsonTest, DeveloperExportQuestionReadsConfirm) {
    const auto request =
        requestOf(RequestType::DeveloperExportConfirmationRequired, DeveloperExportQuestion{});

    EXPECT_EQ(fit(R"("confirm")", request), AnswerValue{DeveloperExportConfirmAnswer{}});
    EXPECT_EQ(fit(R"("cancel")", request), AnswerValue{CancelAnswer{}});
}

// ---- Shapes that do not fit

TEST(AnswerJsonTest, AnswerOfAnotherQuestionTypeIsRefused) {
    const auto dividendTreaty =
        requestOf(RequestType::IncomeDetailsRequired, TreatyClaimQuestion{});
    const auto interestTreaty =
        requestOf(RequestType::IncomeDetailsRequired, InterestTreatyClaimQuestion{});
    const auto dividendsFailure =
        requestOf(RequestType::DividendsFailureResolutionRequired, FailureResolutionQuestion{});
    const auto deposit =
        requestOf(RequestType::DepositInterestTotalRequired, DepositInterestTotalQuestion{});

    EXPECT_EQ(fit(R"("no_claim")", kLossRequest), std::nullopt);
    EXPECT_EQ(fit(R"("claim")", dividendTreaty), std::nullopt);
    EXPECT_EQ(fit(R"({"statement":"x"})", interestTreaty), std::nullopt);
    EXPECT_EQ(fit(R"("developer_export")", dividendsFailure), std::nullopt);
    EXPECT_EQ(fit(R"("skip")", deposit), std::nullopt);
}

TEST(AnswerJsonTest, TreatyStatementThatIsNotATextIsRefused) {
    const auto request = requestOf(RequestType::IncomeDetailsRequired, TreatyClaimQuestion{});

    EXPECT_EQ(fit(R"({"statement":5})", request), std::nullopt);
}

TEST(AnswerJsonTest, RatioNumbersThatAreNotWholeInt64NumbersAreRefused) {
    // newShares as a text.
    EXPECT_EQ(fit(R"({"newShares":"2","oldShares":1,"effectiveDate":"2024-06-01",
                      "confirmedUnits":"6"})",
                  kRatioRequest),
              std::nullopt);
    // newShares with a fraction.
    EXPECT_EQ(fit(R"({"newShares":2.5,"oldShares":1,"effectiveDate":"2024-06-01",
                      "confirmedUnits":"6"})",
                  kRatioRequest),
              std::nullopt);
    // newShares is 2^63, one more than int64 holds.
    EXPECT_EQ(fit(R"({"newShares":9223372036854775808,"oldShares":1,"effectiveDate":"2024-06-01",
                      "confirmedUnits":"6"})",
                  kRatioRequest),
              std::nullopt);
}

TEST(AnswerJsonTest, RatioDateThatDoesNotExistIsRefused) {
    // There is no month 13.
    EXPECT_EQ(fit(R"({"newShares":2,"oldShares":1,"effectiveDate":"2024-13-01",
                      "confirmedUnits":"6"})",
                  kRatioRequest),
              std::nullopt);
    // 2024 is a leap year with 29 days in February, so the 30th does not exist.
    EXPECT_EQ(fit(R"({"newShares":2,"oldShares":1,"effectiveDate":"2024-02-30",
                      "confirmedUnits":"6"})",
                  kRatioRequest),
              std::nullopt);
}

TEST(AnswerJsonTest, RatioWithAnUnknownOrMissingKeyIsRefused) {
    EXPECT_EQ(fit(R"({"newShares":2,"oldShares":1,"effectiveDate":"2024-06-01"})", kRatioRequest),
              std::nullopt);
    EXPECT_EQ(fit(R"({"newShares":2,"oldShares":1,"effectiveDate":"2024-06-01",
                      "confirmedUnits":"6","extra":1})",
                  kRatioRequest),
              std::nullopt);
}

TEST(AnswerJsonTest, PayerOptionThatIsNotAWholeNonNegativeNumberIsRefused) {
    const auto request = requestOf(RequestType::IncomeDetailsRequired, PayerChoiceQuestion{});

    EXPECT_EQ(fit(R"({"option":-1})", request), std::nullopt);
    EXPECT_EQ(fit(R"({"option":1.5})", request), std::nullopt);
}

TEST(AnswerJsonTest, CorrectedUnitsAsAJsonNumberAreRefused) {
    const auto request =
        requestOf(RequestType::QuantityCorrectionRequired, QuantityCorrectionQuestion{});

    // A number would lose digits on the way.
    EXPECT_EQ(fit(R"({"correctedUnits":0.99999998})", request), std::nullopt);
}

TEST(AnswerJsonTest, UnknownOtherInterestSubtypeIsRefused) {
    const auto request =
        requestOf(RequestType::TaxClassificationRequired, IncomeClassificationQuestion{});

    EXPECT_EQ(fit(R"({"choice":"other_interest","subtype":"loan"})", request), std::nullopt);
}

TEST(AnswerJsonTest, OtherInterestChoiceThatIsNotOtherInterestIsRefused) {
    const auto request =
        requestOf(RequestType::TaxClassificationRequired, IncomeClassificationQuestion{});

    EXPECT_EQ(fit(R"({"choice":"dividends","subtype":"bond_coupon"})", request), std::nullopt);
    EXPECT_EQ(fit(R"({"choice":"other_interest"})", request), std::nullopt);
}

TEST(AnswerJsonTest, QuantityObjectWithAnotherKeyIsRefused) {
    const auto request =
        requestOf(RequestType::QuantityCorrectionRequired, QuantityCorrectionQuestion{});

    EXPECT_EQ(fit(R"({"units":"1"})", request), std::nullopt);
}

TEST(AnswerJsonTest, RequestWithoutQuestionDataCannotBeAnswered) {
    const auto request = requestOf(RequestType::ExchangeRatesRequired, std::monostate{});

    EXPECT_EQ(fit(R"("cancel")", request), std::nullopt);
}

TEST(AnswerJsonTest, TextThatIsNotJsonIsRefused) {
    EXPECT_EQ(fit("no_other_activity", kLossRequest), std::nullopt);
}

// ---- Values that fit but that the session checks

TEST(AnswerJsonTest, RatioOfZeroPassesTheReader) {
    const auto answer = fit(R"({"newShares":0,"oldShares":1,"effectiveDate":"2024-06-01",
                                "confirmedUnits":"6"})",
                            kRatioRequest);

    const AnswerValue expected =
        SplitRatioAnswer{.mNewShares = 0,
                         .mOldShares = 1,
                         .mEffectiveDate = *parseCalendarDate("2024-06-01"),
                         .mConfirmedUnits = 600'000'000};

    EXPECT_EQ(answer, expected);
}

TEST(AnswerJsonTest, CorrectionOfZeroPassesTheReader) {
    const auto request =
        requestOf(RequestType::QuantityCorrectionRequired, QuantityCorrectionQuestion{});

    EXPECT_EQ(fit(R"({"correctedUnits":"0.00000000"})", request),
              AnswerValue{QuantityCorrectionAnswer{0}});
}

// ---- The body of an answer call

/// A result at revision 2 with the loss request `request-0001` and the deposit request
/// `request-0002`.
ApplicationResult resultWithTwoRequests() {
    ApplicationResult result;
    result.mRunRevision = 2;
    result.mRequests = {
        DecisionRequest{.mId = "request-0001",
                        .mType = RequestType::LossEligibilityConfirmationRequired,
                        .mRunRevision = 2,
                        .mData = LossEligibilityQuestion{}},
        DecisionRequest{.mId = "request-0002",
                        .mType = RequestType::DepositInterestTotalRequired,
                        .mRunRevision = 2,
                        .mData = DepositInterestTotalQuestion{}},
    };

    return result;
}

TEST(AnswerCallJsonTest, ReadsTwoAnswersAgainstTheOpenRequests) {
    const auto result = resultWithTwoRequests();

    const auto parsed = parseAnswerCallJson(
        R"({"runRevision":2,"answers":[
              {"requestId":"request-0001","answer":"no_other_activity"},
              {"requestId":"request-0002","answer":"above_limit"}]})",
        result);

    ASSERT_TRUE(std::holds_alternative<AnswerCall>(parsed));

    const auto& call = std::get<AnswerCall>(parsed);

    ASSERT_EQ(call.mAnswers.size(), 2U);

    EXPECT_TRUE(call.mShapeRejections.empty());

    EXPECT_EQ(call.mAnswers[0].mRequestId, "request-0001");
    EXPECT_EQ(call.mAnswers[0].mRunRevision, 2U);
    EXPECT_EQ(call.mAnswers[0].mValue, AnswerValue{NoOtherActivityAnswer{}});

    EXPECT_EQ(call.mAnswers[1].mRequestId, "request-0002");
    EXPECT_EQ(call.mAnswers[1].mRunRevision, 2U);
    EXPECT_EQ(call.mAnswers[1].mValue, AnswerValue{DepositInterestTotalAnswer{true}});
}

TEST(AnswerCallJsonTest, StaleRevisionGivesAPlaceholderWithTheGivenRevision) {
    const auto result = resultWithTwoRequests();

    // The answer text would not fit the loss question, but the revision is checked first.
    const auto parsed = parseAnswerCallJson(
        R"({"runRevision":1,"answers":[{"requestId":"request-0001","answer":"above_limit"}]})",
        result);

    ASSERT_TRUE(std::holds_alternative<AnswerCall>(parsed));

    const auto& call = std::get<AnswerCall>(parsed);

    ASSERT_EQ(call.mAnswers.size(), 1U);

    EXPECT_TRUE(call.mShapeRejections.empty());
    EXPECT_EQ(call.mAnswers[0].mRequestId, "request-0001");
    EXPECT_EQ(call.mAnswers[0].mRunRevision, 1U);
    EXPECT_EQ(call.mAnswers[0].mValue, AnswerValue{CancelAnswer{}});
}

TEST(AnswerCallJsonTest, UnknownRequestGivesAPlaceholderWithTheGivenId) {
    const auto result = resultWithTwoRequests();

    const auto parsed = parseAnswerCallJson(
        R"({"runRevision":2,"answers":[{"requestId":"request-0099","answer":"anything"}]})",
        result);

    ASSERT_TRUE(std::holds_alternative<AnswerCall>(parsed));

    const auto& call = std::get<AnswerCall>(parsed);

    ASSERT_EQ(call.mAnswers.size(), 1U);

    EXPECT_TRUE(call.mShapeRejections.empty());
    EXPECT_EQ(call.mAnswers[0].mRequestId, "request-0099");
    EXPECT_EQ(call.mAnswers[0].mRunRevision, 2U);
    EXPECT_EQ(call.mAnswers[0].mValue, AnswerValue{CancelAnswer{}});
}

TEST(AnswerCallJsonTest, ShapeErrorIsRejectedAtItsPosition) {
    const auto result = resultWithTwoRequests();

    // The second answer, "skip", does not fit the deposit question.
    const auto parsed = parseAnswerCallJson(
        R"({"runRevision":2,"answers":[
              {"requestId":"request-0001","answer":"no_other_activity"},
              {"requestId":"request-0002","answer":"skip"}]})",
        result);

    ASSERT_TRUE(std::holds_alternative<AnswerCall>(parsed));

    const auto& call = std::get<AnswerCall>(parsed);

    ASSERT_EQ(call.mShapeRejections.size(), 1U);

    EXPECT_EQ(call.mShapeRejections[0].mIndex, 1U);
    EXPECT_EQ(call.mShapeRejections[0].mReason, AnswerRejection::AnswerDoesNotFit);
    EXPECT_EQ(call.mShapeRejections[0].mMessageKey, MessageKey::AnswerRejectedAnswerDoesNotFit);
    EXPECT_EQ(call.mShapeRejections[0].mMessageParams,
              (std::vector<MessageParam>{textParam("requestId", "request-0002")}));
}

TEST(AnswerCallJsonTest, EmptyAnswerListIsAccepted) {
    const auto result = resultWithTwoRequests();

    const auto parsed = parseAnswerCallJson(R"({"runRevision":2,"answers":[]})", result);

    ASSERT_TRUE(std::holds_alternative<AnswerCall>(parsed));

    EXPECT_TRUE(std::get<AnswerCall>(parsed).mAnswers.empty());
}

/// The error of a failed read of an answer call; the test fails when the body was accepted.
CallBodyError errorOfAnswerCall(std::string_view aBody) {
    const auto result = resultWithTwoRequests();
    const auto parsed = parseAnswerCallJson(aBody, result);

    EXPECT_TRUE(std::holds_alternative<CallBodyError>(parsed));

    return std::holds_alternative<CallBodyError>(parsed) ? std::get<CallBodyError>(parsed)
                                                         : CallBodyError::PathNotAllowed;
}

TEST(AnswerCallJsonTest, TextThatIsNotJsonIsBadRequest) {
    EXPECT_EQ(errorOfAnswerCall("runRevision"), CallBodyError::BadRequest);
}

TEST(AnswerCallJsonTest, MissingRunRevisionIsBadRequest) {
    EXPECT_EQ(errorOfAnswerCall(R"({"answers":[]})"), CallBodyError::BadRequest);
}

TEST(AnswerCallJsonTest, RunRevisionThatIsNotAWholeNonNegativeNumberIsBadRequest) {
    EXPECT_EQ(errorOfAnswerCall(R"({"runRevision":"2","answers":[]})"), CallBodyError::BadRequest);
    EXPECT_EQ(errorOfAnswerCall(R"({"runRevision":-1,"answers":[]})"), CallBodyError::BadRequest);
    EXPECT_EQ(errorOfAnswerCall(R"({"runRevision":2.5,"answers":[]})"), CallBodyError::BadRequest);
}

TEST(AnswerCallJsonTest, AnswersThatAreNotAListAreBadRequest) {
    EXPECT_EQ(errorOfAnswerCall(R"({"runRevision":2,"answers":{}})"), CallBodyError::BadRequest);
}

TEST(AnswerCallJsonTest, AnswerEntryWithoutAnAnswerIsBadRequest) {
    EXPECT_EQ(errorOfAnswerCall(R"({"runRevision":2,"answers":[{"requestId":"request-0001"}]})"),
              CallBodyError::BadRequest);
}

TEST(AnswerCallJsonTest, RequestIdThatIsNotATextIsBadRequest) {
    EXPECT_EQ(errorOfAnswerCall(R"({"runRevision":2,"answers":[{"requestId":1,"answer":"x"}]})"),
              CallBodyError::BadRequest);
}

TEST(AnswerCallJsonTest, UnknownKeyIsBadRequest) {
    EXPECT_EQ(errorOfAnswerCall(R"({"runRevision":2,"answers":[],"extra":1})"),
              CallBodyError::BadRequest);
    EXPECT_EQ(
        errorOfAnswerCall(
            R"({"runRevision":2,"answers":[{"requestId":"request-0001","answer":"x","extra":1}]})"),
        CallBodyError::BadRequest);
}

TEST(AnswerCallJsonTest, PathKeyIsRefused) {
    EXPECT_EQ(errorOfAnswerCall(R"({"runRevision":2,"answers":[],"path":"/home/x"})"),
              CallBodyError::PathNotAllowed);
}

TEST(AnswerCallJsonTest, NestingDeeperThanEightLevelsIsBadRequest) {
    EXPECT_EQ(errorOfAnswerCall(R"({"runRevision":2,"answers":[[[[[[[[[[]]]]]]]]]]})"),
              CallBodyError::BadRequest);
}

TEST(AnswerCallJsonTest, DeeplyNestedJsonIsBadRequest) {
    const std::string body = std::string(100000, '[') + std::string(100000, ']');

    EXPECT_EQ(errorOfAnswerCall(body), CallBodyError::BadRequest);
}

TEST(AnswerCallJsonTest, EveryErrorHasItsCode) {
    EXPECT_EQ(api::callBodyErrorCode(CallBodyError::BadRequest), "bad_request");
    EXPECT_EQ(api::callBodyErrorCode(CallBodyError::PathNotAllowed), "path_not_allowed");
}

// ---- The body of a delete call

/// The error of a failed read of a delete call; the test fails when the body was accepted.
CallBodyError errorOfDeleteCall(std::string_view aBody) {
    const auto parsed = parseDeleteCallJson(aBody);

    EXPECT_TRUE(std::holds_alternative<CallBodyError>(parsed));

    return std::holds_alternative<CallBodyError>(parsed) ? std::get<CallBodyError>(parsed)
                                                         : CallBodyError::PathNotAllowed;
}

TEST(DeleteCallJsonTest, ReadsTheDecisionIds) {
    const auto parsed =
        parseDeleteCallJson(R"({"runRevision":3,"decisionIds":["decision-0001","decision-0002"]})");

    ASSERT_TRUE(std::holds_alternative<DeleteCall>(parsed));

    const auto& call = std::get<DeleteCall>(parsed);

    EXPECT_EQ(call.mRunRevision, 3U);
    EXPECT_FALSE(call.mAll);
    EXPECT_EQ(call.mDecisionIds, (std::vector<std::string>{"decision-0001", "decision-0002"}));
}

TEST(DeleteCallJsonTest, ReadsDeleteAll) {
    const auto parsed = parseDeleteCallJson(R"({"runRevision":3,"all":true})");

    ASSERT_TRUE(std::holds_alternative<DeleteCall>(parsed));

    const auto& call = std::get<DeleteCall>(parsed);

    EXPECT_EQ(call.mRunRevision, 3U);
    EXPECT_TRUE(call.mAll);
    EXPECT_TRUE(call.mDecisionIds.empty());
}

TEST(DeleteCallJsonTest, AllThatIsFalseIsBadRequest) {
    EXPECT_EQ(errorOfDeleteCall(R"({"runRevision":3,"all":false})"), CallBodyError::BadRequest);
}

TEST(DeleteCallJsonTest, IdsAndAllTogetherAreBadRequest) {
    EXPECT_EQ(errorOfDeleteCall(R"({"runRevision":3,"all":true,"decisionIds":[]})"),
              CallBodyError::BadRequest);
}

TEST(DeleteCallJsonTest, NeitherIdsNorAllIsBadRequest) {
    EXPECT_EQ(errorOfDeleteCall(R"({"runRevision":3})"), CallBodyError::BadRequest);
}

TEST(DeleteCallJsonTest, IdsThatAreNotAListAreBadRequest) {
    EXPECT_EQ(errorOfDeleteCall(R"({"runRevision":3,"decisionIds":"decision-0001"})"),
              CallBodyError::BadRequest);
}

TEST(DeleteCallJsonTest, IdThatIsNotATextIsBadRequest) {
    EXPECT_EQ(errorOfDeleteCall(R"({"runRevision":3,"decisionIds":[1]})"),
              CallBodyError::BadRequest);
}

TEST(DeleteCallJsonTest, MissingRunRevisionIsBadRequest) {
    EXPECT_EQ(errorOfDeleteCall(R"({"decisionIds":["decision-0001"]})"), CallBodyError::BadRequest);
}

TEST(DeleteCallJsonTest, PathKeyIsRefused) {
    EXPECT_EQ(errorOfDeleteCall(R"({"runRevision":3,"decisionIds":[],"path":"/home/x"})"),
              CallBodyError::PathNotAllowed);
}

} // namespace
