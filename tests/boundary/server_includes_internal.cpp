// Must not compile: the local server reaches the backend only through the public headers. This
// file is built with the include directories and libraries of the taxbroker_server target, and the
// PublicApiBoundary.ServerCannotIncludeInternal test expects the error for this header.
#include "parsers/traderepublic_parser.hpp"
