// Test stand-in for boost/container/small_vector.hpp (the pass only needs vector-like behaviour).
#pragma once
#include <cstddef>
#include <vector>
namespace boost::container {
template<class T, std::size_t N>
using small_vector = std::vector<T>;
}
