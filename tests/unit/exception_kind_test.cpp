#include "server/exception_kind.hpp"

#include <gtest/gtest.h>

#include <new>
#include <stdexcept>
#include <string>

namespace {
using namespace taxbroker;

TEST(ExceptionKindTest, OutOfMemoryHasItsOwnWord) {
    std::string kind;

    try
    { throw std::bad_alloc(); } catch (...)
    { kind = currentExceptionKind(); }

    EXPECT_EQ(kind, "out_of_memory");
}

TEST(ExceptionKindTest, AStandardExceptionGivesItsTypeNameAndNeverItsText) {
    std::string kind;

    try
    { throw std::runtime_error("secret detail of /home/someone"); } catch (...)
    { kind = currentExceptionKind(); }

    EXPECT_EQ(kind, "std::runtime_error");
}

TEST(ExceptionKindTest, ADerivedExceptionGivesItsOwnTypeName) {
    std::string kind;

    try
    { throw std::out_of_range("secret"); } catch (...)
    { kind = currentExceptionKind(); }

    EXPECT_EQ(kind, "std::out_of_range");
}

TEST(ExceptionKindTest, ANonStandardExceptionIsUnknown) {
    std::string kind;

    try
    { throw 42; } catch (...)
    { kind = currentExceptionKind(); }

    EXPECT_EQ(kind, "unknown");
}

} // namespace
