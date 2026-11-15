#include "exception_kind.hpp"

#include <cstdlib>
#include <exception>
#include <memory>
#include <new>
#include <typeinfo>

#if defined(__GNUG__)
#include <cxxabi.h>
#endif

namespace taxbroker {

namespace {

/// The readable name of a type; the raw name when it cannot be demangled.
std::string typeName(const std::type_info& aType) {
#if defined(__GNUG__)
    int status = 0;
    const std::unique_ptr<char, decltype(&std::free)> demangled{
        abi::__cxa_demangle(aType.name(), nullptr, nullptr, &status),
        &std::free};

    if (status == 0 && demangled != nullptr)
    {
        return demangled.get();
    }
#endif

    return aType.name();
}

} // namespace

std::string currentExceptionKind() {
    try
    { throw; } catch (const std::bad_alloc&)
    { return "out_of_memory"; } catch (const std::exception& error)
    { return typeName(typeid(error)); } catch (...)
    { return "unknown"; }
}

} // namespace taxbroker
