#include "server/start_failure_page.hpp"

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>

namespace {
using namespace taxbroker;

bool contains(const std::string& aText, const std::string& aPart) {
    return aText.find(aPart) != std::string::npos;
}

TEST(StartFailurePageTest, ErrorLineWithHtmlCharactersIsEscaped) {
    const std::string page = makeStartFailurePage("bad <b> & \"x\" 'y'", "2.0.0");

    EXPECT_TRUE(contains(page, "<code>bad &lt;b&gt; &amp; &quot;x&quot; &#39;y&#39;</code>"));
    EXPECT_FALSE(contains(page, "<b>"));
}

TEST(StartFailurePageTest, MailLinkHasTheVersionSubjectAndTheEncodedError) {
    const std::string page = makeStartFailurePage("port in use & more", "2.0.0");

    // The subject is "TaxBrokerReport 2.0.0 did not start"; the body is the error line. The `&`
    // between the two parts is written as the entity `&amp;`; the `&` of the error as `%26`.
    EXPECT_TRUE(contains(page,
                         "href=\"mailto:tax.brokerage.report@gmail.com"
                         "?subject=TaxBrokerReport%202.0.0%20did%20not%20start"
                         "&amp;body=port%20in%20use%20%26%20more\""));
}

TEST(StartFailurePageTest, PageIsSlovenianFirstThenEnglishWithTheDisclaimerAndNoScript) {
    const std::string page = makeStartFailurePage("an error", "2.0.0");

    const std::size_t slovenian = page.find("TaxBrokerReport se ni zagnal</h1>");
    const std::size_t english = page.find("TaxBrokerReport did not start</h1>");

    ASSERT_NE(slovenian, std::string::npos);
    ASSERT_NE(english, std::string::npos);

    EXPECT_LT(slovenian, english);
    EXPECT_TRUE(contains(page, "Različica 2.0.0"));
    EXPECT_TRUE(contains(page, "Version 2.0.0"));
    EXPECT_TRUE(contains(page, "~/.local/state/taxbroker/taxbroker.log"));
    EXPECT_TRUE(contains(page, "razvijalec ne odgovarja za davčne napake"));
    EXPECT_TRUE(contains(page, "no liability for tax errors, fines or omissions"));
    EXPECT_FALSE(contains(page, "<script"));
    EXPECT_FALSE(contains(page, "src="));
}

TEST(StartFailurePageTest, WithoutAnyFolderNoPageIsWrittenAndRemovingOneIsHarmless) {
    // No XDG_RUNTIME_DIR, no XDG_STATE_HOME and no HOME: there is no folder for the page.
    const EnvLookup noFolders = [](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
    };

    EXPECT_FALSE(writeStartFailurePage(noFolders, "an error", "2.0.0").has_value());

    removeStartFailurePage(noFolders);
}

} // namespace
