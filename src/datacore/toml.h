#pragma once
// The one include of vendored toml++ (third_party/tomlplusplus/README.md): no exceptions, its own
// warnings off, so sco-core's /W4 /WX and -Wall -Wextra -Werror stay on for our code. Only pack.cpp
// includes this.
#ifndef TOML_EXCEPTIONS
#define TOML_EXCEPTIONS 0
#endif
#define TOML_ENABLE_FORMATTERS 0
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#include <toml.hpp>
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
