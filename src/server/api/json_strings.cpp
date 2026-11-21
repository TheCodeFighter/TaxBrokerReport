#include "json_strings.hpp"

#include <chrono>
#include <string>
#include <variant>

namespace taxbroker::api::detail {

std::string_view toString(Broker aBroker) {
    switch (aBroker)
    {
    case Broker::TradeRepublic:
        return "trade_republic";
    case Broker::InteractiveBrokers:
        return "interactive_brokers";
    case Broker::Unknown:
        return "unknown";
    }

    return "unknown";
}

std::string_view toString(DiagnosticSeverity aSeverity) {
    switch (aSeverity)
    {
    case DiagnosticSeverity::Warning:
        return "warning";
    case DiagnosticSeverity::Error:
        return "error";
    case DiagnosticSeverity::Info:
        return "info";
    }

    return "error";
}

std::string_view toString(DiagnosticCode aCode) {
    switch (aCode)
    {
    case DiagnosticCode::UnknownRowType:
        return "unknown_row_type";
    case DiagnosticCode::UnsupportedRowType:
        return "unsupported_row_type";
    case DiagnosticCode::UnsupportedAssetClass:
        return "unsupported_asset_class";
    case DiagnosticCode::MissingField:
        return "missing_field";
    case DiagnosticCode::InvalidValue:
        return "invalid_value";
    case DiagnosticCode::InconsistentValue:
        return "inconsistent_value";
    case DiagnosticCode::TradeAmountMismatch:
        return "trade_amount_mismatch";
    case DiagnosticCode::FieldCountMismatch:
        return "field_count_mismatch";
    case DiagnosticCode::LocalizedNumberFormat:
        return "localized_number_format";
    case DiagnosticCode::InvalidHeader:
        return "invalid_header";
    case DiagnosticCode::ParseError:
        return "parse_error";
    case DiagnosticCode::DuplicateRate:
        return "duplicate_rate";
    case DiagnosticCode::MixedRateYears:
        return "mixed_rate_years";
    case DiagnosticCode::DuplicateRateYear:
        return "duplicate_rate_year";
    case DiagnosticCode::NoRates:
        return "no_rates";
    case DiagnosticCode::InputFileTooLarge:
        return "input_file_too_large";
    case DiagnosticCode::InputFileOutOfMemory:
        return "input_file_out_of_memory";
    }

    return "parse_error";
}

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
    case ProcessingDiagnosticCode::InterestPayerNumberMissing:
        return "interest_payer_number_missing";
    case ProcessingDiagnosticCode::InterestPaymentNotPositive:
        return "interest_payment_not_positive";
    case ProcessingDiagnosticCode::SlovenianCouponTax:
        return "slovenian_coupon_tax";
    case ProcessingDiagnosticCode::CouponAccruedInterestNotDeducted:
        return "coupon_accrued_interest_not_deducted";
    case ProcessingDiagnosticCode::BondSaleAccruedInterestNotReported:
        return "bond_sale_accrued_interest_not_reported";
    case ProcessingDiagnosticCode::SlovenianPayerTaxNumberMissing:
        return "slovenian_payer_tax_number_missing";
    case ProcessingDiagnosticCode::DepositInterestAddOtherBanks:
        return "deposit_interest_add_other_banks";
    case ProcessingDiagnosticCode::DeveloperExportUnavailable:
        return "developer_export_unavailable";
    case ProcessingDiagnosticCode::TaxpayerTaxNumberCheckDigit:
        return "taxpayer_tax_number_check_digit";
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
    case RequestType::DeveloperExportConfirmationRequired:
        return "developer_export_confirmation_required";
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

std::string_view toString(FailureResolution aResolution) {
    switch (aResolution)
    {
    case FailureResolution::ExcludeFailedInstruments:
        return "exclude_failed_instruments";
    case FailureResolution::FailReport:
        return "fail_report";
    case FailureResolution::DeveloperExport:
        return "developer_export";
    }

    return "fail_report";
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

nlohmann::json messageParamsToJson(const std::vector<MessageParam>& aParams) {
    nlohmann::json params = nlohmann::json::object();

    for (const auto& param : aParams)
    {
        const auto toJson = [](const auto& aValue) { return nlohmann::json(aValue); };

        params[param.mName] = std::visit(toJson, param.mValue);
    }

    return params;
}

} // namespace taxbroker::api::detail
