#include "server/multipart.hpp"

#include <gtest/gtest.h>

#include <string>
#include <variant>
#include <vector>

namespace {
using namespace taxbroker;

constexpr std::size_t kBigLimit = 1000;

/// The parts of a successful read; the test fails when the read failed.
std::vector<MultipartPart>
partsOf(const std::variant<std::vector<MultipartPart>, MultipartError>& aResult) {
    EXPECT_TRUE(std::holds_alternative<std::vector<MultipartPart>>(aResult));

    if (!std::holds_alternative<std::vector<MultipartPart>>(aResult))
    {
        return {};
    }

    return std::get<std::vector<MultipartPart>>(aResult);
}

/// The error of a failed read; the test fails when the read worked.
MultipartError errorOf(const std::variant<std::vector<MultipartPart>, MultipartError>& aResult) {
    EXPECT_TRUE(std::holds_alternative<MultipartError>(aResult));

    if (!std::holds_alternative<MultipartError>(aResult))
    {
        return MultipartError::TooLarge;
    }

    return std::get<MultipartError>(aResult);
}

TEST(MultipartTest, ReadsAFieldAndAFile) {
    const std::string body =
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"request\"\r\n"
        "\r\n"
        "{}\r\n"
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"file0\"; filename=\"export.csv\"\r\n"
        "Content-Type: text/csv\r\n"
        "\r\n"
        "a,b\n1,2\n\r\n"
        "--XyZ--\r\n";

    const auto parts = partsOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit));

    ASSERT_EQ(parts.size(), 2U);

    EXPECT_EQ(parts[0].mName, "request");
    EXPECT_FALSE(parts[0].mFileName.has_value());
    EXPECT_EQ(parts[0].mContent, "{}");

    EXPECT_EQ(parts[1].mName, "file0");
    EXPECT_EQ(parts[1].mFileName, "export.csv");
    EXPECT_EQ(parts[1].mContent, "a,b\n1,2\n");
}

TEST(MultipartTest, ReadsAQuotedBoundary) {
    const std::string body =
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"request\"\r\n"
        "\r\n"
        "{}\r\n"
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"file0\"; filename=\"export.csv\"\r\n"
        "\r\n"
        "a,b\n1,2\n\r\n"
        "--XyZ--\r\n";

    const auto parts =
        partsOf(readMultipart(body, "multipart/form-data; boundary=\"XyZ\"", kBigLimit));

    ASSERT_EQ(parts.size(), 2U);

    EXPECT_EQ(parts[0].mContent, "{}");
    EXPECT_EQ(parts[1].mContent, "a,b\n1,2\n");
}

TEST(MultipartTest, IgnoresTextBeforeTheFirstAndAfterTheLastBoundary) {
    const std::string body =
        "This is the preamble.\r\n"
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"request\"\r\n"
        "\r\n"
        "{}\r\n"
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"file0\"; filename=\"export.csv\"\r\n"
        "\r\n"
        "a,b\n1,2\n\r\n"
        "--XyZ--\r\n"
        "This is the epilogue.\r\n";

    const auto parts = partsOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit));

    ASSERT_EQ(parts.size(), 2U);

    EXPECT_EQ(parts[0].mContent, "{}");
    EXPECT_EQ(parts[1].mContent, "a,b\n1,2\n");
}

TEST(MultipartTest, BoundaryTextInsideTheContentStaysContent) {
    // "a--XyZb" has the boundary text in the middle of a line, and "\r\n--XyZa" has it at the
    // start of a line but followed by a letter: neither is a delimiter line.
    const std::string body =
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"file0\"; filename=\"export.csv\"\r\n"
        "\r\n"
        "a--XyZb\r\n"
        "--XyZa\r\n"
        "end\r\n"
        "--XyZ--\r\n";

    const auto parts = partsOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit));

    ASSERT_EQ(parts.size(), 1U);

    EXPECT_EQ(parts[0].mContent, "a--XyZb\r\n--XyZa\r\nend");
}

TEST(MultipartTest, EmptyFileHasEmptyContent) {
    const std::string body =
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"file0\"; filename=\"empty.csv\"\r\n"
        "\r\n"
        "\r\n"
        "--XyZ--\r\n";

    const auto parts = partsOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit));

    ASSERT_EQ(parts.size(), 1U);

    EXPECT_EQ(parts[0].mFileName, "empty.csv");
    EXPECT_EQ(parts[0].mContent, "");
}

TEST(MultipartTest, KeepsTheRawFileNameOfAWindowsPath) {
    const std::string body = "--XyZ\r\n"
                             "Content-Disposition: form-data; name=\"file0\"; "
                             "filename=\"C:\\Users\\someone\\export.csv\"\r\n"
                             "\r\n"
                             "x\r\n"
                             "--XyZ--\r\n";

    const auto parts = partsOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit));

    ASSERT_EQ(parts.size(), 1U);

    EXPECT_EQ(parts[0].mFileName, "C:\\Users\\someone\\export.csv");
}

TEST(MultipartTest, KeepsTheRawFileNameOfAParentFolderPath) {
    const std::string body =
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"file0\"; filename=\"../../export.csv\"\r\n"
        "\r\n"
        "x\r\n"
        "--XyZ--\r\n";

    const auto parts = partsOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit));

    ASSERT_EQ(parts.size(), 1U);

    EXPECT_EQ(parts[0].mFileName, "../../export.csv");
}

TEST(MultipartTest, ReadsAFileNameWithUtf8Bytes) {
    const std::string body =
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"file0\"; filename=\"davčni.csv\"\r\n"
        "\r\n"
        "x\r\n"
        "--XyZ--\r\n";

    const auto parts = partsOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit));

    ASSERT_EQ(parts.size(), 1U);

    EXPECT_EQ(parts[0].mFileName, "davčni.csv");
}

TEST(MultipartTest, BodyWithoutTheClosingLineIsMalformed) {
    const std::string body = "--XyZ\r\n"
                             "Content-Disposition: form-data; name=\"request\"\r\n"
                             "\r\n"
                             "{}\r\n";

    EXPECT_EQ(errorOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit)),
              MultipartError::Malformed);
}

TEST(MultipartTest, PartWithoutContentDispositionIsABadPartHeader) {
    const std::string body = "--XyZ\r\n"
                             "Content-Type: text/csv\r\n"
                             "\r\n"
                             "x\r\n"
                             "--XyZ--\r\n";

    EXPECT_EQ(errorOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit)),
              MultipartError::BadPartHeader);
}

TEST(MultipartTest, PartWithoutANameIsABadPartHeader) {
    const std::string body = "--XyZ\r\n"
                             "Content-Disposition: form-data; filename=\"export.csv\"\r\n"
                             "\r\n"
                             "x\r\n"
                             "--XyZ--\r\n";

    EXPECT_EQ(errorOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit)),
              MultipartError::BadPartHeader);
}

TEST(MultipartTest, ContentTypeWithoutABoundaryIsABadContentType) {
    const std::string body = "--XyZ\r\n"
                             "Content-Disposition: form-data; name=\"request\"\r\n"
                             "\r\n"
                             "{}\r\n"
                             "--XyZ--\r\n";

    EXPECT_EQ(errorOf(readMultipart(body, "multipart/form-data", kBigLimit)),
              MultipartError::BadContentType);
}

TEST(MultipartTest, BoundaryOf71CharactersIsABadContentType) {
    const std::string boundary(71, 'b');
    const std::string body = "--" + boundary +
                             "\r\n"
                             "Content-Disposition: form-data; name=\"request\"\r\n"
                             "\r\n"
                             "{}\r\n"
                             "--" +
                             boundary + "--\r\n";

    EXPECT_EQ(errorOf(readMultipart(body, "multipart/form-data; boundary=" + boundary, kBigLimit)),
              MultipartError::BadContentType);
}

TEST(MultipartTest, BoundaryOf70CharactersIsAccepted) {
    const std::string boundary(70, 'b');
    const std::string body = "--" + boundary +
                             "\r\n"
                             "Content-Disposition: form-data; name=\"request\"\r\n"
                             "\r\n"
                             "{}\r\n"
                             "--" +
                             boundary + "--\r\n";

    const auto parts =
        partsOf(readMultipart(body, "multipart/form-data; boundary=" + boundary, kBigLimit));

    ASSERT_EQ(parts.size(), 1U);

    EXPECT_EQ(parts[0].mContent, "{}");
}

TEST(MultipartTest, OtherContentTypeIsABadContentType) {
    EXPECT_EQ(errorOf(readMultipart("{}", "application/json", kBigLimit)),
              MultipartError::BadContentType);
}

TEST(MultipartTest, LineFeedOnlyLineEndsAreMalformed) {
    const std::string body = "--XyZ\n"
                             "Content-Disposition: form-data; name=\"request\"\n"
                             "\n"
                             "{}\n"
                             "--XyZ--\n";

    EXPECT_EQ(errorOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit)),
              MultipartError::Malformed);
}

TEST(MultipartTest, MoreThan33PartsAreTooManyParts) {
    std::string body;

    for (int index = 0; index < 34; ++index)
    {
        body += "--XyZ\r\nContent-Disposition: form-data; name=\"p" + std::to_string(index) +
                "\"\r\n\r\nx\r\n";
    }

    body += "--XyZ--\r\n";

    EXPECT_EQ(errorOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit)),
              MultipartError::TooManyParts);
}

TEST(MultipartTest, Exactly33PartsAreAccepted) {
    std::string body;

    for (int index = 0; index < 33; ++index)
    {
        body += "--XyZ\r\nContent-Disposition: form-data; name=\"p" + std::to_string(index) +
                "\"\r\n\r\nx\r\n";
    }

    body += "--XyZ--\r\n";

    const auto parts = partsOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit));

    EXPECT_EQ(parts.size(), 33U);
}

TEST(MultipartTest, ErrorReasonsAreFixedWords) {
    EXPECT_EQ(multipartErrorReason(MultipartError::BadContentType), "content_type");
    EXPECT_EQ(multipartErrorReason(MultipartError::Malformed), "body");
    EXPECT_EQ(multipartErrorReason(MultipartError::BadPartHeader), "part_header");
    EXPECT_EQ(multipartErrorReason(MultipartError::TooManyParts), "too_many_parts");
    EXPECT_EQ(multipartErrorReason(MultipartError::TooLarge), "too_large");
}

TEST(MultipartTest, FilePartOfTheLimitIsAcceptedAndOneByteMoreIsTooLarge) {
    const std::string tenBytes(10, 'x');
    const std::string elevenBytes(11, 'x');
    const std::string head =
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"file0\"; filename=\"export.csv\"\r\n"
        "\r\n";

    // The parts are views into the body, so the body must outlive them.
    const std::string bodyOfTen = head + tenBytes + "\r\n--XyZ--\r\n";
    const std::string bodyOfEleven = head + elevenBytes + "\r\n--XyZ--\r\n";

    const auto accepted =
        partsOf(readMultipart(bodyOfTen, "multipart/form-data; boundary=XyZ", 10));

    ASSERT_EQ(accepted.size(), 1U);

    EXPECT_EQ(accepted[0].mContent, tenBytes);

    EXPECT_EQ(errorOf(readMultipart(bodyOfEleven, "multipart/form-data; boundary=XyZ", 10)),
              MultipartError::TooLarge);
}

TEST(MultipartTest, FieldLongerThanTheFileLimitIsAccepted) {
    // The limit is for files; the request part is bounded by the body limit.
    const std::string body = "--XyZ\r\n"
                             "Content-Disposition: form-data; name=\"request\"\r\n"
                             "\r\n"
                             "0123456789ab\r\n"
                             "--XyZ--\r\n";

    const auto parts = partsOf(readMultipart(body, "multipart/form-data; boundary=XyZ", 10));

    ASSERT_EQ(parts.size(), 1U);

    EXPECT_EQ(parts[0].mContent, "0123456789ab");
}

TEST(MultipartTest, BoundaryAfterOtherParametersIsFound) {
    const std::string body = "--XyZ\r\n"
                             "Content-Disposition: form-data; name=\"request\"\r\n"
                             "\r\n"
                             "{}\r\n"
                             "--XyZ--\r\n";

    const auto parts =
        partsOf(readMultipart(body, "multipart/form-data; charset=utf-8; boundary=XyZ", kBigLimit));

    ASSERT_EQ(parts.size(), 1U);

    EXPECT_EQ(parts[0].mContent, "{}");
}

TEST(MultipartTest, BlanksAfterADelimiterAndAfterAParameterAreAllowed) {
    // Two spaces after the first delimiter, and a space after the name parameter.
    const std::string body = "--XyZ  \r\n"
                             "Content-Disposition: form-data; name=\"request\" \r\n"
                             "\r\n"
                             "{}\r\n"
                             "--XyZ--\r\n";

    const auto parts = partsOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit));

    ASSERT_EQ(parts.size(), 1U);

    EXPECT_EQ(parts[0].mName, "request");
    EXPECT_EQ(parts[0].mContent, "{}");
}

TEST(MultipartTest, PartHeaderWithoutALineEndIsABadPartHeader) {
    const std::string body = "--XyZ\r\n"
                             "Content-Disposition: form-data; name=\"request\"";

    EXPECT_EQ(errorOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit)),
              MultipartError::BadPartHeader);
}

TEST(MultipartTest, PartHeaderWithoutAColonIsABadPartHeader) {
    const std::string body = "--XyZ\r\n"
                             "this is not a header\r\n"
                             "\r\n"
                             "{}\r\n"
                             "--XyZ--\r\n";

    EXPECT_EQ(errorOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit)),
              MultipartError::BadPartHeader);
}

TEST(MultipartTest, DispositionThatIsNotFormDataIsABadPartHeader) {
    const std::string body = "--XyZ\r\n"
                             "Content-Disposition: attachment; name=\"request\"\r\n"
                             "\r\n"
                             "{}\r\n"
                             "--XyZ--\r\n";

    EXPECT_EQ(errorOf(readMultipart(body, "multipart/form-data; boundary=XyZ", kBigLimit)),
              MultipartError::BadPartHeader);
}

} // namespace
