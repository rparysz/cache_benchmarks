#ifndef SIZE_LITERALS_HPP
#define SIZE_LITERALS_HPP

#include <cstddef>

namespace bench {
inline namespace literals {

constexpr std::size_t operator""_KB(unsigned long long x)
{
  return x * 1024;
}
constexpr std::size_t operator""_MB(unsigned long long x)
{
  return x * 1024 * 1024;
}
constexpr std::size_t operator""_GB(unsigned long long x)
{
  return x * 1024 * 1024 * 1024;
}

} // namespace literals
} // namespace bench

#endif // SIZE_LITERALS_HPP