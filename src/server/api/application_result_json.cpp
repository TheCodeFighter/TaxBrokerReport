#include "taxbroker/api/application_result_json.hpp"
#include "json_strings.hpp"
#include "taxbroker/application_result.hpp"
#include "taxbroker/currency.hpp"
#include "utils/numbered_id.hpp"
#include "utils/numeric_util.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

using namespace taxbroker;
using api::detail::messageParamsToJson;
using api::detail::toString;

/// Keeps the keys in insertion order, which is the documented order of the application JSON.
using Json = nlohmann::ordered_json;

std::string_view toString(PipelineStage aStage) {
    switch (aStage)
    {
    case PipelineStage::Parsing:
        return "parsing";
    case PipelineStage::Merging:
        return "merging";
    case PipelineStage::HistoricalValidation:
        return "historical_validation";
    case PipelineStage::CorporateAction:
        return "corporate_action";
    case PipelineStage::FifoTax:
        return "fifo_tax";
    case PipelineStage::XmlGeneration:
        return "xml_generation";
    }

    return "parsing";
}

std::string_view toString(ReportType aType) {
    switch (aType)
    {
    case ReportType::CapitalGains:
        return "capital_gains";
    case ReportType::Dividends:
        return "dividends";
    case ReportType::BankDepositInterest:
        return "bank_deposit_interest";
    case ReportType::OtherInterest:
        return "other_interest";
    }

    return "capital_gains";
}

std::string_view reportKey(ReportType aType) {
    switch (aType)
    {
    case ReportType::CapitalGains:
        return "capitalGains";
    case ReportType::Dividends:
        return "dividends";
    case ReportType::BankDepositInterest:
        return "bankInterest";
    case ReportType::OtherInterest:
        return "otherInterest";
    }

    return "capitalGains";
}

std::string_view toString(MergeDiagnosticCode aCode) {
    switch (aCode)
    {
    case MergeDiagnosticCode::DuplicateSourceIndex:
        return "duplicate_source_index";
    case MergeDiagnosticCode::InconsistentSourceIndex:
        return "inconsistent_source_index";
    case MergeDiagnosticCode::ConflictingDuplicate:
        return "conflicting_duplicate";
    case MergeDiagnosticCode::InstrumentNameConflict:
        return "instrument_name_conflict";
    case MergeDiagnosticCode::InstrumentAssetClassConflict:
        return "instrument_asset_class_conflict";
    case MergeDiagnosticCode::DuplicateTransactionInFile:
        return "duplicate_transaction_in_file";
    }

    return "conflicting_duplicate";
}

std::string_view toString(ProcessingDiagnosticCode aCode) {
    switch (aCode)
    {
    case ProcessingDiagnosticCode::UnrepresentableValue:
        return "unrepresentable_value";
    case ProcessingDiagnosticCode::ArithmeticOverflow:
        return "arithmetic_overflow";
    case ProcessingDiagnosticCode::InvalidExchangeRate:
        return "invalid_exchange_rate";
    case ProcessingDiagnosticCode::CorporateActionUnrepresentable:
        return "corporate_action_unrepresentable";
    case ProcessingDiagnosticCode::InvalidReportingYear:
        return "invalid_reporting_year";
    case ProcessingDiagnosticCode::IncompleteHistory:
        return "incomplete_history";
    case ProcessingDiagnosticCode::XmlValueOutOfRange:
        return "xml_value_out_of_range";
    case ProcessingDiagnosticCode::XmlSerializationFailed:
        return "xml_serialization_failed";
    case ProcessingDiagnosticCode::UnsupportedTaxTreatment:
        return "unsupported_tax_treatment";
    case ProcessingDiagnosticCode::TaxClassificationUnknown:
        return "tax_classification_unknown";
    case ProcessingDiagnosticCode::CorporateActionNotApplied:
        return "corporate_action_not_applied";
    case ProcessingDiagnosticCode::MissingExchangeRate:
        return "missing_exchange_rate";
    case ProcessingDiagnosticCode::LossEligibilityUnresolved:
        return "loss_eligibility_unresolved";
    case ProcessingDiagnosticCode::CorporateActionAmbiguous:
        return "corporate_action_ambiguous";
    case ProcessingDiagnosticCode::CorporateActionMissingPosition:
        return "corporate_action_missing_position";
    case ProcessingDiagnosticCode::CorporateActionQuantityRounded:
        return "corporate_action_quantity_rounded";
    case ProcessingDiagnosticCode::CorporateActionApplied:
        return "corporate_action_applied";
    case ProcessingDiagnosticCode::UnsupportedCorporateAction:
        return "unsupported_corporate_action";
    case ProcessingDiagnosticCode::TaxCategoryNotReported:
        return "tax_category_not_reported";
    case ProcessingDiagnosticCode::OfficialRateFromNearbyDate:
        return "official_rate_from_nearby_date";
    case ProcessingDiagnosticCode::OfficialRateFileIncomplete:
        return "official_rate_file_incomplete";
    case ProcessingDiagnosticCode::OfficialRateFilesRequired:
        return "official_rate_files_required";
    case ProcessingDiagnosticCode::QuantityRoundingReconciled:
        return "quantity_rounding_reconciled";
    case ProcessingDiagnosticCode::SourceQuantityCorrected:
        return "source_quantity_corrected";
    case ProcessingDiagnosticCode::XmlBasisRoundingDifference:
        return "xml_basis_rounding_difference";
    case ProcessingDiagnosticCode::DividendPayerNotFound:
        return "dividend_payer_not_found";
    case ProcessingDiagnosticCode::ForeignTaxEvidenceRequired:
        return "foreign_tax_evidence_required";
    case ProcessingDiagnosticCode::InterestPayerNotFound:
        return "interest_payer_not_found";
    case ProcessingDiagnosticCode::InterestPaymentNotPositive:
        return "interest_payment_not_positive";
    case ProcessingDiagnosticCode::SlovenianCouponTax:
        return "slovenian_coupon_tax";
    case ProcessingDiagnosticCode::DepositInterestAddOtherBanks:
        return "deposit_interest_add_other_banks";
    case ProcessingDiagnosticCode::TreatyClaimDetailsMissing:
        return "treaty_claim_details_missing";
    }

    return "unrepresentable_value";
}

std::string_view toString(RequestType aType) {
    switch (aType)
    {
    case RequestType::ExchangeRatesRequired:
        return "exchange_rates_required";
    case RequestType::CorporateActionIdentityRequired:
        return "corporate_action_identity_required";
    case RequestType::CorporateActionRatioRequired:
        return "corporate_action_ratio_required";
    case RequestType::QuantityCorrectionRequired:
        return "quantity_correction_required";
    case RequestType::LossEligibilityConfirmationRequired:
        return "loss_eligibility_confirmation_required";
    case RequestType::TaxClassificationRequired:
        return "tax_classification_required";
    case RequestType::IncomeDetailsRequired:
        return "income_details_required";
    case RequestType::CapitalGainsFailureResolutionRequired:
        return "capital_gains_failure_resolution_required";
    case RequestType::DividendsFailureResolutionRequired:
        return "dividends_failure_resolution_required";
    case RequestType::OtherInterestFailureResolutionRequired:
        return "other_interest_failure_resolution_required";
    case RequestType::DepositInterestTotalRequired:
        return "deposit_interest_total_required";
    }

    return "tax_classification_required";
}

std::string_view toString(ReportStatus aStatus) {
    switch (aStatus)
    {
    case ReportStatus::Generated:
        return "generated";
    case ReportStatus::NoData:
        return "no_data";
    case ReportStatus::NeedsInput:
        return "needs_input";
    case ReportStatus::Failed:
        return "failed";
    }

    return "failed";
}

std::string_view toString(OverallStatus aStatus) {
    switch (aStatus)
    {
    case OverallStatus::Success:
        return "success";
    case OverallStatus::CompletedWithWarnings:
        return "completed_with_warnings";
    case OverallStatus::PartialSuccess:
        return "partial_success";
    case OverallStatus::NeedsInput:
        return "needs_input";
    case OverallStatus::Failed:
        return "failed";
    }

    return "failed";
}

std::string_view toString(NoDataReason aReason) {
    switch (aReason)
    {
    case NoDataReason::BelowDepositInterestThreshold:
        return "below_deposit_interest_threshold";
    case NoDataReason::NoSelectedYearDisposals:
        return "no_selected_year_disposals";
    }

    return "no_selected_year_disposals";
}

std::string_view codeString(const ApplicationDiagnosticCode& aCode) {
    const auto toCodeString = [](const auto& aValue) { return toString(aValue); };

    return std::visit(toCodeString, aCode);
}

std::string isoDate(const Date& aDate) {
    const std::chrono::sys_days days{
        std::chrono::duration_cast<std::chrono::days>(aDate.time_since_epoch())};
    const std::chrono::year_month_day date{days};

    auto month = std::to_string(static_cast<unsigned>(date.month()));
    auto day = std::to_string(static_cast<unsigned>(date.day()));

    if (month.size() < 2)
    {
        month.insert(0, "0");
    }
    if (day.size() < 2)
    {
        day.insert(0, "0");
    }

    return std::to_string(static_cast<int>(date.year())) + "-" + month + "-" + day;
}

Json reportTypesToJson(const std::vector<ReportType>& aTypes) {
    Json types = Json::array();

    std::transform(aTypes.begin(), aTypes.end(), std::back_inserter(types), [](ReportType aType) {
        return Json(toString(aType));
    });

    return types;
}

Json sourcesToJson(const std::vector<ApplicationSource>& aSources) {
    Json sources = Json::array();

    for (const auto& source : aSources)
    {
        Json serialized{
            {"broker", toString(source.mBroker)},
            {"sourceId", numberedId("source", source.mSourceIndex + 1)},
        };

        // Every filename passes through the sanitizer again; a name that fails is left out.
        if (source.mFilename)
        {
            try
            {
                serialized["file"] = SourceFilename::fromPath(source.mFilename->value()).value();
            } catch (const std::invalid_argument&)
            {
                // The source ID still identifies the file.
            }
        }
        if (source.mRow)
        {
            serialized["row"] = *source.mRow;
        }
        if (source.mTransactionId)
        {
            serialized["transactionId"] = *source.mTransactionId;
        }

        sources.push_back(std::move(serialized));
    }

    return sources;
}

Json diagnosticToJson(const ApplicationDiagnostic& aDiagnostic) {
    Json serialized{
        {"id", aDiagnostic.mId},
        {"severity", toString(aDiagnostic.mSeverity)},
        {"stage", toString(aDiagnostic.mStage)},
        {"code", codeString(aDiagnostic.mCode)},
        {"message", aDiagnostic.mMessage},
    };

    // The keys follow the order of the JSON example in diagnostics.md.
    if (aDiagnostic.mMessageKey)
    {
        serialized["messageKey"] = messageDefinition(*aDiagnostic.mMessageKey).mId;
        serialized["messageParams"] = messageParamsToJson(aDiagnostic.mMessageParams);
    }

    serialized["affectedReports"] = reportTypesToJson(aDiagnostic.mAffectedReports);
    serialized["blocking"] = aDiagnostic.mBlocking;
    serialized["sources"] = sourcesToJson(aDiagnostic.mSources);

    if (aDiagnostic.mTaxDate)
    {
        serialized["taxDate"] = isoDate(*aDiagnostic.mTaxDate);
    }
    if (aDiagnostic.mField)
    {
        serialized["field"] = *aDiagnostic.mField;
    }
    if (aDiagnostic.mInstrumentName || aDiagnostic.mIsin)
    {
        Json instrument = Json::object();

        if (aDiagnostic.mInstrumentName)
        {
            instrument["name"] = *aDiagnostic.mInstrumentName;
        }
        if (aDiagnostic.mIsin)
        {
            instrument["isin"] = *aDiagnostic.mIsin;
        }

        serialized["instrument"] = std::move(instrument);
    }
    if (aDiagnostic.mRequestId)
    {
        serialized["requestId"] = *aDiagnostic.mRequestId;
    }

    return serialized;
}

Json reasonCodesToJson(const std::vector<ApplicationDiagnosticCode>& aCodes) {
    Json codes = Json::array();

    std::transform(aCodes.begin(), aCodes.end(), std::back_inserter(codes), [](const auto& aCode) {
        return Json(codeString(aCode));
    });

    return codes;
}

std::string_view toString(FailureResolution aResolution) {
    switch (aResolution)
    {
    case FailureResolution::ExcludeFailedInstruments:
        return "exclude_failed_instruments";
    case FailureResolution::FailReport:
        return "fail_report";
    }

    return "fail_report";
}

/// Units as an exact decimal string with eight decimals: 4 units is `4.00000000`.
std::string unitsString(Units aUnits) {
    constexpr std::size_t kDecimals = 8;

    return formatFixedPoint(aUnits, UNITS_SCALE, kDecimals);
}

/// The answer for a treatment: a string, or the choice `other_interest` with its subtype.
Json incomeTreatmentToJson(IncomeTreatment aTreatment) {
    switch (aTreatment)
    {
    case IncomeTreatment::Dividends:
        return "dividends";
    case IncomeTreatment::BankDepositInterest:
        return "bank_deposit_interest";
    case IncomeTreatment::BrokerCashInterest:
        return Json{{"choice", "other_interest"}, {"subtype", "broker_cash"}};
    case IncomeTreatment::BondCoupon:
        break;
    }

    return Json{{"choice", "other_interest"}, {"subtype", "bond_coupon"}};
}

/// A choice is a string; a ratio is an object with its reduced numbers, its effective date and
/// the confirmed position after the action.
Json answerToJson(const AnswerValue& aAnswer) {
    const auto toAnswerJson = [](const auto& aValue) -> Json {
        using Value = std::decay_t<decltype(aValue)>;

        if constexpr (std::is_same_v<Value, FailureResolutionAnswer>)
        {
            return toString(aValue.mChoice);
        }
        else if constexpr (std::is_same_v<Value, CancelAnswer>)
        {
            return "cancel";
        }
        else if constexpr (std::is_same_v<Value, NoOtherActivityAnswer>)
        {
            return "no_other_activity";
        }
        else if constexpr (std::is_same_v<Value, OtherActivityAnswer>)
        {
            return "other_activity";
        }
        else if constexpr (std::is_same_v<Value, SeparateEventsAnswer>)
        {
            return "separate_events";
        }
        else if constexpr (std::is_same_v<Value, CapitalGainsAnswer>)
        {
            return "capital_gains";
        }
        else if constexpr (std::is_same_v<Value, ExcludeFromReportsAnswer>)
        {
            return "exclude_from_supported_reports";
        }
        else if constexpr (std::is_same_v<Value, QuantityCorrectionAnswer>)
        {
            return Json{{"correctedUnits", unitsString(aValue.mCorrectedUnits)}};
        }
        else if constexpr (std::is_same_v<Value, DepositInterestTotalAnswer>)
        {
            return aValue.mAboveLimit ? "above_limit" : "not_above_limit";
        }
        else if constexpr (std::is_same_v<Value, IncomeTreatmentAnswer>)
        {
            return incomeTreatmentToJson(aValue.mTreatment);
        }
        else if constexpr (std::is_same_v<Value, BondTypeAnswer>)
        {
            return aValue.mType == BondType::Ordinary ? "ordinary" : "discount_or_zero_coupon";
        }
        else if constexpr (std::is_same_v<Value, TreatyClaimAnswer>)
        {
            // No claim is a string; a claim is its treaty text.
            if (!aValue.mStatement)
            {
                return "no_claim";
            }

            return Json{{"statement", *aValue.mStatement}};
        }
        else if constexpr (std::is_same_v<Value, InterestTreatyClaimAnswer>)
        {
            return aValue.mClaim ? "claim" : "no_claim";
        }
        else if constexpr (std::is_same_v<Value, PayerChoiceAnswer>)
        {
            // A skip is a string; a pick is the position of the row among the options.
            if (!aValue.mPickedOption)
            {
                return "skip";
            }

            return Json{{"option", *aValue.mPickedOption}};
        }
        else
        {
            static_assert(std::is_same_v<Value, SplitRatioAnswer>);
            return Json{
                {"newShares", aValue.mNewShares},
                {"oldShares", aValue.mOldShares},
                {"effectiveDate", isoDate(aValue.mEffectiveDate)},
                {"confirmedUnits", unitsString(aValue.mConfirmedUnits)},
            };
        }
    };

    return std::visit(toAnswerJson, aAnswer);
}

/// The question data of a loss question: the ISIN and the facts of each open sale.
Json lossCheckToJson(const LossEligibilityQuestion& aQuestion) {
    Json sales = Json::array();

    for (const auto& sale : aQuestion.mSales)
    {
        Json replacements = Json::array();

        std::transform(sale.mKnownReplacements.begin(),
                       sale.mKnownReplacements.end(),
                       std::back_inserter(replacements),
                       [](const KnownReplacement& aReplacement) {
                           return Json{{"date", isoDate(aReplacement.mDate)},
                                       {"units", unitsString(aReplacement.mUnits)}};
                       });

        Json serialized{{"saleDate", isoDate(sale.mSaleDate)}};

        if (sale.mSaleTransaction)
        {
            serialized["transaction"] = Json{
                {"broker", toString(sale.mSaleTransaction->mBroker)},
                {"transactionId", sale.mSaleTransaction->mTransactionId},
            };
        }

        serialized["saleUnits"] = unitsString(sale.mSaleUnits);
        serialized["lossUnits"] = unitsString(sale.mLossUnits);
        serialized["windowStart"] = isoDate(sale.mWindowStart);
        serialized["windowEnd"] = isoDate(sale.mWindowEnd);
        serialized["knownReplacements"] = std::move(replacements);
        serialized["knownIneligibleUnits"] = unitsString(sale.mKnownIneligibleUnits);
        serialized["openUnits"] = unitsString(sale.mOpenUnits);
        sales.push_back(std::move(serialized));
    }

    return Json{{"name", aQuestion.mName}, {"isin", aQuestion.mIsin}, {"sales", std::move(sales)}};
}

/// The question data of a ratio question: the action, every source record, the position before
/// it on its row date (or the earlier sale it must come before) and the date ranges an answer may
/// give, each with its position before and the lowest change after. A source value is shown
/// read-only; its meaning is not verified.
Json ratioCheckToJson(const CorporateActionRatioQuestion& aQuestion) {
    Json sources = Json::array();

    for (const auto& source : aQuestion.mSources)
    {
        Json serialized{{"broker", toString(source.mBroker)}, {"date", isoDate(source.mDate)}};

        if (source.mTransactionId)
        {
            serialized["transactionId"] = *source.mTransactionId;
        }
        if (source.mSourceValue)
        {
            serialized["sourceValue"] = *source.mSourceValue;
        }

        sources.push_back(std::move(serialized));
    }

    Json dateRanges = Json::array();

    std::transform(aQuestion.mDateRanges.begin(),
                   aQuestion.mDateRanges.end(),
                   std::back_inserter(dateRanges),
                   [](const EffectiveDateRange& aRange) {
                       return Json{
                           {"earliestDate", isoDate(aRange.mEarliestDate)},
                           {"latestDate", isoDate(aRange.mLatestDate)},
                           {"positionBefore", unitsString(aRange.mPositionBefore)},
                           {"lowestChangeAfter", unitsString(aRange.mLowestChangeAfter)},
                       };
                   });

    Json check{
        {"name", aQuestion.mName},
        {"isin", aQuestion.mIsin},
        {"effectiveDate", isoDate(aQuestion.mEffectiveDate)},
        {"sources", std::move(sources)},
    };

    // A question before a sale with too few units has no position on the row date.
    if (aQuestion.mPositionBefore)
    {
        check["positionBefore"] = unitsString(*aQuestion.mPositionBefore);
    }
    else
    {
        check["earlierSaleDate"] = isoDate(aQuestion.mEarlierSaleDate.value());
    }

    check["dateRanges"] = std::move(dateRanges);
    check["askSameEvent"] = aQuestion.mAskSameEvent;

    return check;
}

/// The question data of a classification question: the ISIN, the name and the class labels.
Json classificationCheckToJson(const TaxClassificationQuestion& aQuestion) {
    return Json{
        {"name", aQuestion.mName},
        {"isin", aQuestion.mIsin},
        {"classLabels", aQuestion.mClassLabels},
    };
}

/// An exact EUR amount as a fraction of two whole numbers in strings; the values do not fit a
/// JSON number.
Json eurFractionToJson(const ExactRational& aAmount) {
    return Json{{"numerator", aAmount.numerator().str()},
                {"denominator", aAmount.denominator().str()}};
}

/// The question data of a quantity question: the sale, the proposed quantity and what the EUR
/// impact of a correction is valued with.
Json quantityCheckToJson(const QuantityCorrectionQuestion& aQuestion) {
    Json check{{"name", aQuestion.mName},
               {"isin", aQuestion.mIsin},
               {"saleDate", isoDate(aQuestion.mSaleDate)}};

    if (aQuestion.mSaleTransaction)
    {
        check["transaction"] = Json{
            {"broker", toString(aQuestion.mSaleTransaction->mBroker)},
            {"transactionId", aQuestion.mSaleTransaction->mTransactionId},
        };
    }

    check["originalUnits"] = unitsString(aQuestion.mOriginalUnits);
    check["proposedUnits"] = unitsString(aQuestion.mOpenUnits);
    check["unitValueEur"] = eurFractionToJson(aQuestion.mUnitValue);
    check["budgetUsedEur"] = eurFractionToJson(aQuestion.mBudgetUsed);

    return check;
}

std::string_view toString(CatalogueOrigin aOrigin) {
    switch (aOrigin)
    {
    case CatalogueOrigin::BuiltIn:
        return "built_in";
    case CatalogueOrigin::Supplied:
        break;
    }

    return "supplied";
}

/// The question data of a payer choice question: the ISIN and the reference rows to pick from,
/// each with the facts of the payer and the row it came from.
Json payerCheckToJson(const PayerChoiceQuestion& aQuestion) {
    Json options = Json::array();

    for (const auto& option : aQuestion.mOptions)
    {
        Json serialized{{"name", option.mName},
                        {"address", option.mAddress},
                        {"country", option.mCountry},
                        {"source",
                         Json{{"source", option.mSource.mSource},
                              {"sourceRevision", option.mSource.mSourceRevision},
                              {"file", option.mSource.mFile},
                              {"row", option.mSource.mRowNumber},
                              {"origin", toString(option.mSource.mOrigin)}}}};

        if (option.mTaxNumber)
        {
            serialized["taxNumber"] = *option.mTaxNumber;
        }

        options.push_back(std::move(serialized));
    }

    return Json{{"name", aQuestion.mName}, {"isin", aQuestion.mIsin}, {"options", options}};
}

/// The question data of a treaty claim question: the country and its taxed ISINs.
Json treatyCheckToJson(const TreatyClaimQuestion& aQuestion) {
    return Json{{"year", aQuestion.mYear},
                {"country", aQuestion.mCountry},
                {"isins", aQuestion.mIsins}};
}

/// The question data of an interest treaty claim question: the country, its taxed coupon ISINs and
/// whether its cash interest has foreign tax too.
Json interestTreatyCheckToJson(const InterestTreatyClaimQuestion& aQuestion) {
    return Json{{"year", aQuestion.mYear},
                {"country", aQuestion.mCountry},
                {"isins", aQuestion.mIsins},
                {"cashInterest", aQuestion.mCashInterest}};
}

/// The options the question offers: no dividends and no bond coupon without an ISIN.
Json incomeOptionsToJson(const IncomeClassificationQuestion& aQuestion) {
    Json options = Json::array();

    if (aQuestion.offers(IncomeTreatment::Dividends))
    {
        options.push_back("dividends");
    }

    options.push_back("bank_deposit_interest");
    options.push_back("other_interest");
    options.push_back("exclude_from_supported_reports");

    return options;
}

/// The subtypes of the choice `other_interest` that the question offers.
Json incomeSubtypesToJson(const IncomeClassificationQuestion& aQuestion) {
    Json subtypes = Json::array({"broker_cash"});

    if (aQuestion.offers(IncomeTreatment::BondCoupon))
    {
        subtypes.push_back("bond_coupon");
    }

    return subtypes;
}

/// The question data of an income classification question: the payment as the file gave it.
Json incomeCheckToJson(const IncomeClassificationQuestion& aQuestion) {
    constexpr std::size_t kDecimals = 4;
    const auto kind = aQuestion.mEventKind == IncomeEventKind::Dividend ? "dividend" : "interest";
    Json check{{"eventKind", kind},
               {"receiptDate", isoDate(aQuestion.mDate)},
               {"name", aQuestion.mName},
               {"classLabels", aQuestion.mClassLabels},
               {"grossAmount", formatFixedPoint(aQuestion.mGrossAmount, MONEY_SCALE, kDecimals)},
               {"currency", std::string{currencyCode(aQuestion.mCurrency)}}};

    if (aQuestion.mIsin)
    {
        check["isin"] = *aQuestion.mIsin;
    }
    if (aQuestion.mTransaction)
    {
        check["transaction"] = Json{
            {"broker", toString(aQuestion.mTransaction->mBroker)},
            {"transactionId", aQuestion.mTransaction->mTransactionId},
        };
    }

    return check;
}

/// The question data of a deposit interest total question: the year and the imported qualifying
/// interest in EUR with four decimals.
Json depositCheckToJson(const DepositInterestTotalQuestion& aQuestion) {
    constexpr std::size_t kDecimals = 4;

    return Json{{"year", aQuestion.mYear},
                {"importedInterestEur", formatFixedPoint(aQuestion.mTotal, MONEY_SCALE, kDecimals)},
                {"limitEur", formatFixedPoint(kDepositInterestAllowance, MONEY_SCALE, kDecimals)}};
}

/// The question data of a bond type question: the bond and its sales and maturities of the year.
Json bondCheckToJson(const BondTypeQuestion& aQuestion) {
    Json check{{"isin", aQuestion.mIsin},
               {"name", aQuestion.mName},
               {"soldUnits", unitsString(aQuestion.mSoldUnits)},
               {"maturedUnits", unitsString(aQuestion.mMaturedUnits)}};

    if (aQuestion.mMaturityDate)
    {
        check["maturityDate"] = isoDate(*aQuestion.mMaturityDate);
    }

    return check;
}

/// The failed brokers of a question or an exclusion, with the IDs of their diagnostics.
Json brokersToJson(const std::vector<RequestBroker>& aBrokers) {
    Json brokers = Json::array();

    std::transform(aBrokers.begin(),
                   aBrokers.end(),
                   std::back_inserter(brokers),
                   [](const RequestBroker& aBroker) {
                       return Json{
                           {"broker", toString(aBroker.mBroker)},
                           {"reasonCodes", reasonCodesToJson(aBroker.mReasonCodes)},
                           {"diagnosticIds", aBroker.mDiagnosticIds},
                       };
                   });

    return brokers;
}

Json requestToJson(const DecisionRequest& aRequest) {
    // The keys follow the order of the JSON example in diagnostics.md.
    Json serialized{
        {"id", aRequest.mId},
        {"type", toString(aRequest.mType)},
        {"runRevision", aRequest.mRunRevision},
        {"affectedReports", reportTypesToJson(aRequest.mAffectedReports)},
        {"sources", sourcesToJson(aRequest.mSources)},
    };

    if (aRequest.mTaxDate)
    {
        serialized["taxDate"] = isoDate(*aRequest.mTaxDate);
    }
    if (aRequest.mIsin)
    {
        serialized["isin"] = *aRequest.mIsin;
    }

    if (const auto* loss = std::get_if<LossEligibilityQuestion>(&aRequest.mData))
    {
        serialized["options"] = Json::array({"no_other_activity", "other_activity"});
        serialized["lossCheck"] = lossCheckToJson(*loss);
    }

    if (const auto* ratio = std::get_if<CorporateActionRatioQuestion>(&aRequest.mData))
    {
        serialized["ratioCheck"] = ratioCheckToJson(*ratio);
    }

    if (const auto* classification = std::get_if<TaxClassificationQuestion>(&aRequest.mData))
    {
        serialized["options"] = Json::array({"capital_gains", "exclude_from_supported_reports"});
        serialized["classificationCheck"] = classificationCheckToJson(*classification);
    }

    if (const auto* quantity = std::get_if<QuantityCorrectionQuestion>(&aRequest.mData))
    {
        serialized["quantityCheck"] = quantityCheckToJson(*quantity);
    }

    if (const auto* payer = std::get_if<PayerChoiceQuestion>(&aRequest.mData))
    {
        serialized["payerCheck"] = payerCheckToJson(*payer);
    }

    if (const auto* treaty = std::get_if<TreatyClaimQuestion>(&aRequest.mData))
    {
        serialized["treatyCheck"] = treatyCheckToJson(*treaty);
    }

    if (const auto* treaty = std::get_if<InterestTreatyClaimQuestion>(&aRequest.mData))
    {
        serialized["interestTreatyCheck"] = interestTreatyCheckToJson(*treaty);
    }

    if (const auto* deposit = std::get_if<DepositInterestTotalQuestion>(&aRequest.mData))
    {
        serialized["depositCheck"] = depositCheckToJson(*deposit);
    }

    if (const auto* income = std::get_if<IncomeClassificationQuestion>(&aRequest.mData))
    {
        serialized["options"] = incomeOptionsToJson(*income);
        serialized["otherInterestSubtypes"] = incomeSubtypesToJson(*income);
        serialized["incomeCheck"] = incomeCheckToJson(*income);
    }

    if (const auto* bond = std::get_if<BondTypeQuestion>(&aRequest.mData))
    {
        serialized["options"] = Json::array({"ordinary", "discount_or_zero_coupon"});
        serialized["bondCheck"] = bondCheckToJson(*bond);
    }

    if (std::holds_alternative<FailureResolutionQuestion>(aRequest.mData))
    {
        Json investments = Json::array();

        std::transform(aRequest.mInvestments.begin(),
                       aRequest.mInvestments.end(),
                       std::back_inserter(investments),
                       [](const RequestInvestment& aInvestment) {
                           return Json{
                               {"name", aInvestment.mName},
                               {"isin", aInvestment.mIsin},
                               {"reasonCodes", reasonCodesToJson(aInvestment.mReasonCodes)},
                               {"diagnosticIds", aInvestment.mDiagnosticIds},
                           };
                       });

        serialized["options"] = Json::array({toString(FailureResolution::ExcludeFailedInstruments),
                                             toString(FailureResolution::FailReport)});
        serialized["investments"] = std::move(investments);

        // Only the Doh-Obr question has brokers, so the other questions keep their data as it is.
        if (!aRequest.mBrokers.empty())
        {
            serialized["brokers"] = brokersToJson(aRequest.mBrokers);
        }
    }

    return serialized;
}

std::string_view toString(ExcludedCategory aCategory) {
    switch (aCategory)
    {
    case ExcludedCategory::Bond:
        return "bond";
    case ExcludedCategory::Crypto:
        return "crypto";
    case ExcludedCategory::Dividend:
        return "dividend";
    case ExcludedCategory::Interest:
        return "interest";
    case ExcludedCategory::Unknown:
        break;
    }

    return "unknown";
}

std::string_view toString(ExcludedReason aReason) {
    switch (aReason)
    {
    case ExcludedReason::DebtSecurity:
        return "debt_security";
    case ExcludedReason::CryptoNotReported:
        return "crypto_not_reported";
    case ExcludedReason::UserChoice:
        return "user_choice";
    case ExcludedReason::SlovenianTaxSettled:
        return "slovenian_tax_settled";
    }

    return "user_choice";
}

/// An item left out of the supported reports: its identity, category, reason, whether a tax rule
/// or the user chose it, the original class labels, the bond units and the safe sources.
Json excludedItemToJson(const ExcludedItem& aItem) {
    Json serialized{
        {"name", aItem.mName},
        {"category", toString(aItem.mCategory)},
        {"reason", toString(aItem.mReason)},
        {"basis", aItem.isTaxRule() ? "tax_rule" : "user_choice"},
        {"classLabels", aItem.mClassLabels},
        {"sources", sourcesToJson(aItem.mSources)},
        {"diagnosticIds", aItem.mDiagnosticIds},
    };

    // A payment the file gave no ISIN for has no isin key.
    if (!aItem.mIsin.empty())
    {
        serialized["isin"] = aItem.mIsin;
    }

    if (aItem.mBond)
    {
        Json bond{
            {"boughtUnits", unitsString(aItem.mBond->mBoughtUnits)},
            {"soldUnits", unitsString(aItem.mBond->mSoldUnits)},
            {"maturedUnits", unitsString(aItem.mBond->mMaturedUnits)},
        };

        if (aItem.mBond->mMaturityDate)
        {
            bond["maturityDate"] = isoDate(*aItem.mBond->mMaturityDate);
        }

        serialized["bond"] = std::move(bond);
    }

    return serialized;
}

/// A recorded answer: its type, question data and answer. It has no sources, because they are
/// run-local and the answer may come from an earlier run.
Json decisionToJson(const RecordedDecision& aDecision) {
    Json serialized{
        {"id", aDecision.mId},
        {"type", toString(aDecision.mType)},
        {"fromEarlierRun", aDecision.mFromEarlierRun},
        {"used", aDecision.mUsed},
    };

    if (const auto* failure = std::get_if<FailureResolutionQuestion>(&aDecision.mData))
    {
        Json investments = Json::array();

        std::transform(failure->mInvestments.begin(),
                       failure->mInvestments.end(),
                       std::back_inserter(investments),
                       [](const FailedInvestment& aInvestment) {
                           return Json{
                               {"name", aInvestment.mName},
                               {"isin", aInvestment.mIsin},
                               {"reasonCodes", reasonCodesToJson(aInvestment.mReasonCodes)},
                           };
                       });

        serialized["investments"] = std::move(investments);

        if (!failure->mBrokers.empty())
        {
            Json brokers = Json::array();

            std::transform(failure->mBrokers.begin(),
                           failure->mBrokers.end(),
                           std::back_inserter(brokers),
                           [](const FailedBroker& aBroker) {
                               return Json{
                                   {"broker", toString(aBroker.mBroker)},
                                   {"reasonCodes", reasonCodesToJson(aBroker.mReasonCodes)},
                               };
                           });
            serialized["brokers"] = std::move(brokers);
        }
    }

    if (const auto* loss = std::get_if<LossEligibilityQuestion>(&aDecision.mData))
    {
        serialized["lossCheck"] = lossCheckToJson(*loss);
    }

    if (const auto* ratio = std::get_if<CorporateActionRatioQuestion>(&aDecision.mData))
    {
        serialized["ratioCheck"] = ratioCheckToJson(*ratio);
    }

    if (const auto* classification = std::get_if<TaxClassificationQuestion>(&aDecision.mData))
    {
        serialized["classificationCheck"] = classificationCheckToJson(*classification);
    }

    if (const auto* quantity = std::get_if<QuantityCorrectionQuestion>(&aDecision.mData))
    {
        serialized["quantityCheck"] = quantityCheckToJson(*quantity);
    }

    if (const auto* payer = std::get_if<PayerChoiceQuestion>(&aDecision.mData))
    {
        serialized["payerCheck"] = payerCheckToJson(*payer);
    }

    if (const auto* treaty = std::get_if<TreatyClaimQuestion>(&aDecision.mData))
    {
        serialized["treatyCheck"] = treatyCheckToJson(*treaty);
    }

    if (const auto* treaty = std::get_if<InterestTreatyClaimQuestion>(&aDecision.mData))
    {
        serialized["interestTreatyCheck"] = interestTreatyCheckToJson(*treaty);
    }

    if (const auto* deposit = std::get_if<DepositInterestTotalQuestion>(&aDecision.mData))
    {
        serialized["depositCheck"] = depositCheckToJson(*deposit);
    }

    if (const auto* income = std::get_if<IncomeClassificationQuestion>(&aDecision.mData))
    {
        serialized["incomeCheck"] = incomeCheckToJson(*income);
    }

    if (const auto* bond = std::get_if<BondTypeQuestion>(&aDecision.mData))
    {
        serialized["bondCheck"] = bondCheckToJson(*bond);
    }

    serialized["answer"] = answerToJson(aDecision.mAnswer);

    return serialized;
}

Json reportToJson(ReportType aType, const ReportResult& aReport) {
    Json serialized{
        {"status", toString(aReport.mStatus)},
        {"diagnosticIds", aReport.mDiagnosticIds},
        {"requestIds", aReport.mRequestIds},
    };

    // Only the two forms with a failure choice list excluded investments.
    if (aType == ReportType::CapitalGains || aType == ReportType::OtherInterest)
    {
        Json excluded = Json::array();

        std::transform(aReport.mExcludedInvestments.begin(),
                       aReport.mExcludedInvestments.end(),
                       std::back_inserter(excluded),
                       [](const RequestInvestment& aInvestment) {
                           return Json{
                               {"name", aInvestment.mName},
                               {"isin", aInvestment.mIsin},
                               {"reasonCodes", reasonCodesToJson(aInvestment.mReasonCodes)},
                               {"diagnosticIds", aInvestment.mDiagnosticIds},
                           };
                       });
        serialized["excludedInvestments"] = std::move(excluded);

        // Only a Doh-Obr exclusion can list brokers; an empty list stays out like for every form.
        if (!aReport.mExcludedBrokers.empty())
        {
            serialized["excludedBrokers"] = brokersToJson(aReport.mExcludedBrokers);
        }
    }

    if (aReport.mFailureResolution)
    {
        serialized["failureResolution"] = toString(*aReport.mFailureResolution);
    }

    serialized["unsafe"] = aReport.mUnsafe;

    if (aReport.mStatus == ReportStatus::Generated)
    {
        serialized["downloadAvailable"] = true;
    }
    if (aReport.mNoDataReason)
    {
        serialized["reason"] = toString(*aReport.mNoDataReason);
    }

    return serialized;
}

} // namespace

namespace taxbroker::api {

std::string_view diagnosticCodeName(const ApplicationDiagnosticCode& aCode) {
    return codeString(aCode);
}

std::string_view requestTypeName(RequestType aType) {
    return toString(aType);
}

std::string serializeApplicationResultJson(const ApplicationResult& aResult, int aIndent) {
    Json reports = Json::object();

    for (std::size_t index = 0; index < aResult.mReports.size(); ++index)
    {
        const auto type = static_cast<ReportType>(index);

        reports[std::string{reportKey(type)}] = reportToJson(type, aResult.mReports[index]);
    }

    Json diagnostics = Json::array();

    std::transform(aResult.mDiagnostics.begin(),
                   aResult.mDiagnostics.end(),
                   std::back_inserter(diagnostics),
                   diagnosticToJson);

    Json requests = Json::array();

    std::transform(aResult.mRequests.begin(),
                   aResult.mRequests.end(),
                   std::back_inserter(requests),
                   requestToJson);

    Json decisions = Json::array();

    std::transform(aResult.mDecisions.begin(),
                   aResult.mDecisions.end(),
                   std::back_inserter(decisions),
                   decisionToJson);

    Json excludedItems = Json::array();

    std::transform(aResult.mExcludedItems.begin(),
                   aResult.mExcludedItems.end(),
                   std::back_inserter(excludedItems),
                   excludedItemToJson);

    const Json response{
        {"schemaVersion", kApplicationResultSchemaVersion},
        {"status", toString(aResult.mStatus)},
        {"runRevision", aResult.mRunRevision},
        {"reports", std::move(reports)},
        {"excludedItems", std::move(excludedItems)},
        {"diagnostics", std::move(diagnostics)},
        {"requests", std::move(requests)},
        {"decisions", std::move(decisions)},
    };

    return response.dump(aIndent, ' ', false, Json::error_handler_t::replace);
}

} // namespace taxbroker::api
