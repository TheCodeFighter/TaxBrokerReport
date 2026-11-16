#include "../support/log_capture.hpp"
#include "server/profile_routes.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

namespace {
using namespace taxbroker;

const std::string kLocation = "~/.local/share/taxbroker/profile.json";

/// The routes with a store in a temp folder (`data/profile.json`) and the location text of a
/// user whose data folder is the default one.
class ProfileRoutesTest : public ::testing::Test {
  protected:
    ProfileRoutesTest()
        : mFolder(std::filesystem::temp_directory_path() /
                  (std::string("tbr_profile_routes_") +
                   ::testing::UnitTest::GetInstance()->current_test_info()->name())),
          mFile(mFolder / "data" / "profile.json"), mRoutes(ProfileStore(mFile, kLocation)) {}

    void SetUp() override {
        std::filesystem::remove_all(mFolder);
        std::filesystem::create_directories(mFolder);
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(mFolder, ignored);
    }

    /// Puts a file with this content in place of the profile.
    void writeProfileFile(const std::string& aContent) const {
        std::filesystem::create_directories(mFile.parent_path());
        std::ofstream(mFile) << aContent;
    }

    HttpResponse get(const std::string& aPath) const {
        return mRoutes.handle(HttpRequest{.mMethod = "GET", .mTarget = aPath}, aPath);
    }

    HttpResponse post(const std::string& aPath, const std::string& aBody) const {
        return mRoutes.handle(HttpRequest{.mMethod = "POST", .mTarget = aPath, .mBody = aBody},
                              aPath);
    }

    std::filesystem::path mFolder;
    std::filesystem::path mFile;
    ProfileRoutes mRoutes;
};

std::string readAll(const std::filesystem::path& aPath) {
    std::ifstream file(aPath, std::ios::binary);
    std::ostringstream content;
    content << file.rdbuf();

    return content.str();
}

const std::string kAnaBody = R"({"taxNumber":"12345678","isResident":true,"name":"Ana Novak"})";

TEST_F(ProfileRoutesTest, WithoutAFileTheAnswerIsNotSavedWithTheLocation) {
    const HttpResponse response = get("/api/profile");

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(response.mBody,
              R"({"location":"~/.local/share/taxbroker/profile.json","saved":false})");
}

TEST_F(ProfileRoutesTest, WithoutAFileAndWithoutALocationTheAnswerHasNoLocation) {
    const ProfileRoutes routes{ProfileStore(mFile, std::nullopt)};

    const HttpResponse response =
        routes.handle(HttpRequest{.mMethod = "GET", .mTarget = "/api/profile"}, "/api/profile");

    EXPECT_EQ(response.mBody, R"({"saved":false})");
}

TEST_F(ProfileRoutesTest, ASavedProfileIsLoadedAndLogged) {
    LogCapture log;

    ASSERT_EQ(post("/api/profile", kAnaBody).mStatus, 204U);

    const HttpResponse response = get("/api/profile");

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(response.mBody,
              R"({"ignoredKeys":false,"invalidFields":[],)"
              R"("location":"~/.local/share/taxbroker/profile.json","saved":true,)"
              R"("taxpayer":{"isResident":true,"name":"Ana Novak","taxNumber":"12345678"},)"
              R"("unreadable":false})");
    EXPECT_EQ(log.count("Profile saved"), 1U);
    EXPECT_EQ(log.count("Profile loaded (fields 3, invalid 0)"), 1U);
}

TEST_F(ProfileRoutesTest, TheFileHoldsTheCheckedFieldsInKeyOrderAndNoMore) {
    // The keys come in another order, with spaces.
    ASSERT_EQ(post("/api/profile",
                   R"({ "name": "Ana Novak", "isResident": true, "taxNumber": "12345678" })")
                  .mStatus,
              204U);

    EXPECT_EQ(readAll(mFile),
              R"({"isResident":true,"name":"Ana Novak","taxNumber":"12345678"})"
              "\n");
}

TEST_F(ProfileRoutesTest, ASaveReplacesTheEarlierProfile) {
    ASSERT_EQ(post("/api/profile", kAnaBody).mStatus, 204U);
    ASSERT_EQ(post("/api/profile", R"({"taxNumber":"87654321","isResident":true})").mStatus, 204U);

    EXPECT_EQ(readAll(mFile),
              R"({"isResident":true,"taxNumber":"87654321"})"
              "\n");
}

TEST_F(ProfileRoutesTest, AnInvalidTaxNumberIsRefusedWithTheFieldAndNothingIsWritten) {
    LogCapture log;

    // Seven digits instead of eight.
    const HttpResponse response =
        post("/api/profile", R"({"taxNumber":"1234567","isResident":true})");

    EXPECT_EQ(response.mStatus, 400U);
    EXPECT_EQ(response.mBody, R"({"error":"bad_taxpayer","field":"taxNumber"})");
    EXPECT_FALSE(std::filesystem::exists(mFile));
    EXPECT_EQ(log.count("Profile request refused: 400 bad_taxpayer (taxNumber)"), 1U);
}

TEST_F(ProfileRoutesTest, AnUnknownKeyIsRefusedWithoutAFieldAndWithoutTheKeyInTheLog) {
    LogCapture log;

    const HttpResponse response =
        post("/api/profile", R"({"taxNumber":"12345678","isResident":true,"shoeSize":"42"})");

    EXPECT_EQ(response.mStatus, 400U);
    EXPECT_EQ(response.mBody, R"({"error":"bad_taxpayer"})");
    EXPECT_EQ(log.count("Profile request refused: 400 bad_taxpayer"), 1U);
    EXPECT_EQ(log.count("shoeSize"), 0U);
}

TEST_F(ProfileRoutesTest, ABodyThatIsNotJsonIsRefused) {
    const HttpResponse response = post("/api/profile", "not json");

    EXPECT_EQ(response.mStatus, 400U);
    EXPECT_EQ(response.mBody, R"({"error":"bad_taxpayer"})");
}

TEST_F(ProfileRoutesTest, ABodyOf32769BytesIsRefusedAndOneOf32768BytesIsAccepted) {
    const std::string object = R"({"taxNumber":"12345678","isResident":true})";
    const std::string tooLong = object + std::string(32769 - object.size(), ' ');
    const std::string longest = object + std::string(32768 - object.size(), ' ');

    const HttpResponse refused = post("/api/profile", tooLong);

    EXPECT_EQ(refused.mStatus, 413U);
    EXPECT_EQ(refused.mBody, R"({"error":"request_too_large"})");
    EXPECT_FALSE(std::filesystem::exists(mFile));

    EXPECT_EQ(post("/api/profile", longest).mStatus, 204U);
}

TEST_F(ProfileRoutesTest, SlovenianLettersAreSavedAsTheyAreAndLoadedBack) {
    const std::string body = R"({"taxNumber":"12345678","isResident":true,)"
                             R"("name":"Žiga Šuštaršič-Čeh","address":"Črnuška cesta 12",)"
                             R"("city":"Črnomelj","postName":"Škofja Loka"})";

    ASSERT_EQ(post("/api/profile", body).mStatus, 204U);

    // The keys are in key order and the letters are UTF-8, not \u escapes.
    EXPECT_EQ(readAll(mFile),
              R"({"address":"Črnuška cesta 12","city":"Črnomelj","isResident":true,)"
              R"("name":"Žiga Šuštaršič-Čeh","postName":"Škofja Loka","taxNumber":"12345678"})"
              "\n");

    const HttpResponse response = get("/api/profile");

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_NE(response.mBody.find(R"("name":"Žiga Šuštaršič-Čeh")"), std::string::npos);
    EXPECT_NE(response.mBody.find(R"("postName":"Škofja Loka")"), std::string::npos);
}

TEST_F(ProfileRoutesTest, ANameOfSlovenianLettersFillingTheBodyTo32768BytesIsAccepted) {
    const std::string head = R"({"taxNumber":"12345678","isResident":true,"name":")";
    const std::string tail = R"("})";
    std::string name;

    // Each "Č" is two bytes; plain "a" letters fill the rest.
    const std::size_t filler = 32768 - head.size() - tail.size();

    for (std::size_t index = 0; index < filler / 2; ++index)
    {
        name += "Č";
    }

    name += std::string(filler % 2, 'a');

    const std::string fits = head + name + tail;
    const std::string tooLarge = head + name + "a" + tail;

    ASSERT_EQ(fits.size(), 32768U);
    ASSERT_EQ(tooLarge.size(), 32769U);

    EXPECT_EQ(post("/api/profile", tooLarge).mStatus, 413U);
    EXPECT_FALSE(std::filesystem::exists(mFile));

    ASSERT_EQ(post("/api/profile", fits).mStatus, 204U);
    EXPECT_NE(get("/api/profile").mBody.find(name), std::string::npos);
}

TEST_F(ProfileRoutesTest, ALinkInPlaceOfTheFileGivesFiveHundredWithoutThePath) {
    LogCapture log;
    const std::filesystem::path target = mFolder / "target.txt";

    std::ofstream(target) << "target content";
    std::filesystem::create_directories(mFile.parent_path());
    std::filesystem::create_symlink(target, mFile);

    const HttpResponse response = post("/api/profile", kAnaBody);

    EXPECT_EQ(response.mStatus, 500U);
    EXPECT_EQ(response.mBody, R"({"error":"profile_not_saved"})");
    EXPECT_EQ(readAll(target), "target content");
    EXPECT_EQ(log.count("Profile not saved (std::runtime_error)"), 1U);
    EXPECT_EQ(log.count(mFolder.string()), 0U);
}

TEST_F(ProfileRoutesTest, AFileThatIsNotJsonIsUnreadable) {
    LogCapture log;

    writeProfileFile("not json");

    const HttpResponse response = get("/api/profile");

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(response.mBody,
              R"({"ignoredKeys":false,"invalidFields":[],)"
              R"("location":"~/.local/share/taxbroker/profile.json","saved":true,)"
              R"("unreadable":true})");
    EXPECT_EQ(log.count("Profile unreadable"), 1U);
}

TEST_F(ProfileRoutesTest, ALinkInPlaceOfTheFileIsUnreadable) {
    const std::filesystem::path target = mFolder / "target.txt";

    std::ofstream(target) << kAnaBody;
    std::filesystem::create_directories(mFile.parent_path());
    std::filesystem::create_symlink(target, mFile);

    const HttpResponse response = get("/api/profile");

    EXPECT_EQ(response.mStatus, 200U);
    EXPECT_EQ(response.mBody,
              R"({"ignoredKeys":false,"invalidFields":[],)"
              R"("location":"~/.local/share/taxbroker/profile.json","saved":true,)"
              R"("unreadable":true})");
}

TEST_F(ProfileRoutesTest, InvalidAndUnknownKeysAreReportedAndTheRestIsLoaded) {
    LogCapture log;

    writeProfileFile(
        R"({"taxNumber":"12345678","isResident":true,"birthDate":"1980-02-30","filingType":"original"})");

    const HttpResponse response = get("/api/profile");

    EXPECT_EQ(response.mBody,
              R"({"ignoredKeys":true,"invalidFields":["birthDate"],)"
              R"("location":"~/.local/share/taxbroker/profile.json","saved":true,)"
              R"("taxpayer":{"isResident":true,"taxNumber":"12345678"},"unreadable":false})");
    EXPECT_EQ(log.count("Profile loaded (fields 2, invalid 1)"), 1U);
}

TEST_F(ProfileRoutesTest, DeleteRemovesTheFileAndTheSecondDeleteIsTheSame) {
    LogCapture log;

    ASSERT_EQ(post("/api/profile", kAnaBody).mStatus, 204U);

    const HttpResponse first = post("/api/profile/delete", "");
    const HttpResponse second = post("/api/profile/delete", "");

    EXPECT_EQ(first.mStatus, 200U);
    EXPECT_EQ(first.mBody, R"({"location":"~/.local/share/taxbroker/profile.json","saved":false})");
    EXPECT_FALSE(std::filesystem::exists(mFile));
    EXPECT_EQ(second.mStatus, 200U);
    EXPECT_EQ(second.mBody, first.mBody);
    EXPECT_EQ(log.count("Profile deleted"), 2U);
}

TEST_F(ProfileRoutesTest, AFolderInPlaceOfTheFileCannotBeDeleted) {
    LogCapture log;

    std::filesystem::create_directories(mFile);

    const HttpResponse response = post("/api/profile/delete", "");

    EXPECT_EQ(response.mStatus, 500U);
    EXPECT_EQ(response.mBody, R"({"error":"profile_not_deleted"})");
    EXPECT_EQ(log.count("Profile not deleted (std::runtime_error)"), 1U);
}

TEST_F(ProfileRoutesTest, NoLogLineAndNoAnswerHoldsAValueOrAPath) {
    LogCapture log;
    LogCapture::showDebug();

    const HttpResponse saved = post("/api/profile", kAnaBody);
    const HttpResponse loaded = get("/api/profile");
    const HttpResponse deleted = post("/api/profile/delete", "");

    // The log holds neither a value nor a folder of the machine.
    EXPECT_EQ(log.text().find("Ana"), std::string::npos);
    EXPECT_EQ(log.text().find("12345678"), std::string::npos);
    EXPECT_EQ(log.text().find(mFolder.string()), std::string::npos);

    // The save and the delete answers hold no value and no folder.
    EXPECT_EQ(saved.mBody, "");
    EXPECT_EQ(deleted.mBody.find("Ana"), std::string::npos);
    EXPECT_EQ(deleted.mBody.find(mFolder.string()), std::string::npos);

    // The load answer holds the values on purpose, but no folder of the machine.
    EXPECT_NE(loaded.mBody.find("Ana Novak"), std::string::npos);
    EXPECT_EQ(loaded.mBody.find(mFolder.string()), std::string::npos);
}

TEST_F(ProfileRoutesTest, WithoutADataFolderLoadAndDeleteAreEmptyAndSaveGivesFiveHundred) {
    const ProfileRoutes routes{ProfileStore(std::nullopt, std::nullopt)};
    LogCapture log;

    const HttpResponse loaded =
        routes.handle(HttpRequest{.mMethod = "GET", .mTarget = "/api/profile"}, "/api/profile");
    const HttpResponse saved =
        routes.handle(HttpRequest{.mMethod = "POST", .mTarget = "/api/profile", .mBody = kAnaBody},
                      "/api/profile");
    const HttpResponse deleted =
        routes.handle(HttpRequest{.mMethod = "POST", .mTarget = "/api/profile/delete"},
                      "/api/profile/delete");

    EXPECT_EQ(loaded.mStatus, 200U);
    EXPECT_EQ(loaded.mBody, R"({"saved":false})");

    EXPECT_EQ(saved.mStatus, 500U);
    EXPECT_EQ(saved.mBody, R"({"error":"profile_not_saved"})");
    EXPECT_EQ(log.count("Profile not saved (std::runtime_error)"), 1U);

    EXPECT_EQ(deleted.mStatus, 200U);
    EXPECT_EQ(deleted.mBody, R"({"saved":false})");
}

TEST_F(ProfileRoutesTest, OtherMethodsAndPathsAreRefused) {
    const HttpResponse putProfile =
        mRoutes.handle(HttpRequest{.mMethod = "PUT", .mTarget = "/api/profile"}, "/api/profile");
    const HttpResponse getDelete = get("/api/profile/delete");
    const HttpResponse unknown = get("/api/profile/other");

    EXPECT_EQ(putProfile.mStatus, 405U);
    EXPECT_EQ(getDelete.mStatus, 405U);
    EXPECT_EQ(unknown.mStatus, 404U);
    EXPECT_EQ(unknown.mBody, R"({"error":"not_found"})");
}

} // namespace
