// Must not compile: the command line reaches the backend only through the public headers. This
// file is built with the include directories and libraries of the taxbroker_cli target, and the
// PublicApiBoundary.CliCannotIncludeInternal test expects the error for this header.
#include "processors/fifo_matcher.hpp"
