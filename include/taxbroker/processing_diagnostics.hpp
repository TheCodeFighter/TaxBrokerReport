#pragma once

#include "taxbroker/diagnostic_messages.hpp"
#include "taxbroker/event_metadata.hpp"
#include "taxbroker/exact_arithmetic.hpp"

#include <optional>
#include <string>
#include <vector>

namespace taxbroker {

enum class PipelineStage {
    Parsing,
    Merging,
    HistoricalValidation,
    CorporateAction,
    FifoTax,
    XmlGeneration,
};

enum class ReportType {
    CapitalGains,
    Dividends,
    BankDepositInterest,
    OtherInterest,
};

enum class ProcessingDiagnosticCode {
    UnrepresentableValue,
    ArithmeticOverflow,
    InvalidExchangeRate,
    CorporateActionUnrepresentable,
    InvalidReportingYear,
    IncompleteHistory,
    XmlValueOutOfRange,
    /// An error: an XML writer stopped on an internal error; its form has no XML.
    XmlSerializationFailed,
    UnsupportedTaxTreatment,
    TaxClassificationUnknown,
    /// A corporate action that is applied but whose result cannot be reported yet, or that is not
    /// applied because its investment cannot reach Doh-KDVP.
    CorporateActionNotApplied,
    MissingExchangeRate,
    /// A sale with a loss-making FIFO portion while the loss rule is missing.
    LossEligibilityUnresolved,
    /// A corporate action without a known ratio, or several on one day whose identity and order
    /// are unknown.
    CorporateActionAmbiguous,
    CorporateActionMissingPosition,
    CorporateActionQuantityRounded,
    CorporateActionApplied,
    UnsupportedCorporateAction,
    TaxCategoryNotReported, ///< A warning for an instrument left out by a tax rule or the user.
    /// A warning: no ECB rate on the tax date, so the last earlier ECB rate was used.
    OfficialRateFromNearbyDate,
    /// A warning: a rate file starts after the beginning or ends before the end of its year.
    OfficialRateFileIncomplete,
    /// A warning: the years of the ECB rate files that missing rates need (loaded or replaced).
    OfficialRateFilesRequired,
    QuantityRoundingReconciled, ///< A warning: a sale was reduced to the open units (rounding).
    SourceQuantityCorrected,    ///< A warning: a sale was reduced to a quantity the user confirmed.
    /// A warning: the exact basis of a Doh-KDVP purchase row differs from its rounded F4 times F3.
    XmlBasisRoundingDifference,
    /// A warning: no reference row gave the payer of a dividend, so its facts are empty.
    DividendPayerNotFound,
    /// A warning: foreign tax was paid, so the user attaches the evidence when filing in eDavki.
    ForeignTaxEvidenceRequired,
    /// A warning: no reference row gave the payer of an interest payment, so its facts are empty.
    InterestPayerNotFound,
    /// A warning: an interest payment of 0 or less, or that rounds to 0.00, is not reported.
    InterestPaymentNotPositive,
    /// A warning: a coupon of a Slovenian issuer has tax, which is not foreign tax.
    SlovenianCouponTax,
    /// A warning: a written row's payer is in Slovenia, but no valid Slovenian tax number is known.
    SlovenianPayerTaxNumberMissing,
    /// A warning: the user adds the interest of the other banks in eDavki.
    DepositInterestAddOtherBanks,
    /// A warning: the user fills in the treaty paragraph and article of claimed Doh-Obr rows.
    TreatyClaimDetailsMissing,
    /// A warning: developer mode is on, but the developer export is not offered for an
    /// investment with missing history (see the message key for the reason).
    DeveloperExportUnavailable,
};

/// Caller-supplied failure scope and safe provenance; arithmetic helpers do not infer reports.
struct ProcessingContext {
    PipelineStage mStage{PipelineStage::FifoTax};
    std::vector<ReportType> mAffectedReports;
    std::vector<SourceReference> mSources;
    std::optional<Date> mTaxDate;
    std::optional<std::string> mField;
    std::optional<std::string> mInstrumentName;
    std::optional<Isin> mIsin;
    /// Set only on an error of one Doh-Obr cash-interest payment: the broker that holds the cash.
    std::optional<Broker> mCashInterestBroker;
};

/// Final application IDs and resolution status are assigned by later orchestration.
struct ProcessingDiagnostic {
    DiagnosticSeverity mSeverity{DiagnosticSeverity::Error};
    ProcessingDiagnosticCode mCode{ProcessingDiagnosticCode::UnrepresentableValue};
    ProcessingContext mContext;
    /// English sentence for logs, the CLI and developer tools; the frontend uses the key.
    std::string mMessage;
    std::optional<MessageKey> mMessageKey;
    std::vector<MessageParam> mMessageParams;
};

/// Maps a numeric error to a scoped error diagnostic without embedding raw financial values.
/// Corporate-action context overrides the generic code; reports and sources are
/// sorted/deduplicated.
[[nodiscard]] ProcessingDiagnostic numericDiagnostic(NumericError aError,
                                                     ProcessingContext aContext);

} // namespace taxbroker
