#include "taxbroker/currency.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

namespace taxbroker {
namespace {

struct CurrencyEntry {
    Currency mCurrency{Currency::Unknown};
    std::string_view mCode;
};

// EUR and every currency the ECB published a reference rate for on or after 2020-01-01
// (Banka Slovenije uses the same rates). HRK ended on 2022-12-30 and RUB on 2022-03-01.
constexpr std::array kCurrencies{
    CurrencyEntry{Currency::EUR, "EUR"}, CurrencyEntry{Currency::USD, "USD"},
    CurrencyEntry{Currency::GBP, "GBP"}, CurrencyEntry{Currency::CHF, "CHF"},
    CurrencyEntry{Currency::JPY, "JPY"}, CurrencyEntry{Currency::AUD, "AUD"},
    CurrencyEntry{Currency::BGN, "BGN"}, CurrencyEntry{Currency::BRL, "BRL"},
    CurrencyEntry{Currency::CAD, "CAD"}, CurrencyEntry{Currency::CNY, "CNY"},
    CurrencyEntry{Currency::CZK, "CZK"}, CurrencyEntry{Currency::DKK, "DKK"},
    CurrencyEntry{Currency::HKD, "HKD"}, CurrencyEntry{Currency::HRK, "HRK"},
    CurrencyEntry{Currency::HUF, "HUF"}, CurrencyEntry{Currency::IDR, "IDR"},
    CurrencyEntry{Currency::ILS, "ILS"}, CurrencyEntry{Currency::INR, "INR"},
    CurrencyEntry{Currency::ISK, "ISK"}, CurrencyEntry{Currency::KRW, "KRW"},
    CurrencyEntry{Currency::MXN, "MXN"}, CurrencyEntry{Currency::MYR, "MYR"},
    CurrencyEntry{Currency::NOK, "NOK"}, CurrencyEntry{Currency::NZD, "NZD"},
    CurrencyEntry{Currency::PHP, "PHP"}, CurrencyEntry{Currency::PLN, "PLN"},
    CurrencyEntry{Currency::RON, "RON"}, CurrencyEntry{Currency::RUB, "RUB"},
    CurrencyEntry{Currency::SEK, "SEK"}, CurrencyEntry{Currency::SGD, "SGD"},
    CurrencyEntry{Currency::THB, "THB"}, CurrencyEntry{Currency::TRY, "TRY"},
    CurrencyEntry{Currency::ZAR, "ZAR"},
};

// Every enum value except Unknown needs a row; Unknown is the last enum value.
static_assert(kCurrencies.size() == static_cast<std::size_t>(Currency::Unknown));

} // namespace

std::string_view currencyCode(Currency aCurrency) noexcept {
    const auto found = std::ranges::find(kCurrencies, aCurrency, &CurrencyEntry::mCurrency);

    return found == kCurrencies.end() ? std::string_view{} : found->mCode;
}

Currency parseCurrencyCode(std::string_view aCode) noexcept {
    const auto found = std::ranges::find(kCurrencies, aCode, &CurrencyEntry::mCode);

    return found == kCurrencies.end() ? Currency::Unknown : found->mCurrency;
}

} // namespace taxbroker
