#include "scanengine/hybrid/rounding.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace scanengine {
namespace hybrid {

namespace {

double roundHalfEvenDouble(double x) {
    double integral = 0.0;
    const double fraction = std::modf(x, &integral);
    const double magnitude = std::fabs(fraction);
    if (magnitude > 0.5
        || (magnitude == 0.5 && std::fmod(std::fabs(integral), 2.0) == 1.0))
        integral += std::copysign(1.0, x);
    return integral;
}

double thousandthsToDouble(std::uint64_t thousandths, bool negative) {
    if (thousandths == 0)
        return negative ? -0.0 : 0.0;

    int topBit = 0;
    for (std::uint64_t bits = thousandths; bits >>= 1;)
        ++topBit;

    // floor(log2(thousandths / 1000)); the estimate can be low by one.
    int exponent = topBit - 10;
    const int nextExponent = exponent + 1;
    bool reachesNextPower = false;
    if (nextExponent >= 0) {
        // roundToDecimalDigits returns early for binaryShift >= 0, so the
        // largest value reaching here is below 2^52. The comparison therefore
        // fits in uint64_t even at nextExponent == 52.
        assert(nextExponent <= 52);
        reachesNextPower = thousandths >= (std::uint64_t(1000) << nextExponent);
    } else {
        reachesNextPower = (thousandths << -nextExponent) >= 1000;
    }
    if (reachesNextPower)
        ++exponent;

    // Correctly round the exact rational thousandths / 1000 to binary64.
    const int shift = 52 - exponent;
#ifdef _MSC_VER
    // Windows x64 MSVC has no __int128 type. _udiv128 performs the same
    // 128-by-64 division while keeping the quotient (a binary64 significand)
    // and remainder exact.
    assert(shift > 0 && shift < 64);
    const std::uint64_t numeratorHigh = thousandths >> (64 - shift);
    const std::uint64_t numeratorLow = thousandths << shift;
    std::uint64_t remainder64 = 0;
    std::uint64_t significand = _udiv128(numeratorHigh, numeratorLow, 1000, &remainder64);
    const unsigned remainder = static_cast<unsigned>(remainder64);
#else
    const unsigned __int128 numerator =
        static_cast<unsigned __int128>(thousandths) << shift;
    std::uint64_t significand = static_cast<std::uint64_t>(numerator / 1000);
    const unsigned remainder = static_cast<unsigned>(numerator % 1000);
#endif
    if (remainder > 500 || (remainder == 500 && (significand & 1U)))
        ++significand;

    if (significand == (std::uint64_t(1) << 53)) {
        significand >>= 1;
        ++exponent;
    }

    const std::uint64_t sign = negative ? (std::uint64_t(1) << 63) : 0;
    const std::uint64_t biasedExponent = std::uint64_t(exponent + 1023) << 52;
    const std::uint64_t fraction = significand - (std::uint64_t(1) << 52);
    const std::uint64_t resultBits = sign | biasedExponent | fraction;
    double result = 0.0;
    std::memcpy(&result, &resultBits, sizeof(result));
    return result;
}

}  // namespace

int roundHalfToEven(double x) {
    return static_cast<int>(roundHalfEvenDouble(x));
}

double roundToDecimalDigits(double x, int digits) {
    // Product code currently requires banker's rounding to three decimal places.
    if (digits != 3) {
        assert(digits == 3 && "roundToDecimalDigits only supports digits == 3");
        return x;
    }

    if (!std::isfinite(x) || x == 0.0)
        return x;

    std::uint64_t bits = 0;
    std::memcpy(&bits, &x, sizeof(bits));
    const bool negative = (bits >> 63) != 0;
    const std::uint64_t exponentBits = (bits >> 52) & 0x7ffU;
    const std::uint64_t fraction = bits & ((std::uint64_t(1) << 52) - 1);

    std::uint64_t significand = fraction;
    int binaryShift = -1074;
    if (exponentBits != 0) {
        significand |= std::uint64_t(1) << 52;
        binaryShift = int(exponentBits) - 1023 - 52;
    }

    // This multiplication is exact: a binary64 significand has at most 53 bits,
    // and (2^53 - 1) * 1000 still fits in uint64_t.
    const std::uint64_t scaledSignificand = significand * 1000U;
    if (binaryShift >= 0)
        return x;

    const int denominatorShift = -binaryShift;
    if (denominatorShift >= 64)
        return negative ? -0.0 : 0.0;

    const std::uint64_t integerPart = scaledSignificand >> denominatorShift;
    const std::uint64_t remainderMask =
        (std::uint64_t(1) << denominatorShift) - 1;
    const std::uint64_t remainder = scaledSignificand & remainderMask;
    const std::uint64_t halfway = std::uint64_t(1) << (denominatorShift - 1);
    std::uint64_t rounded = integerPart;
    if (remainder > halfway || (remainder == halfway && (integerPart & 1U)))
        ++rounded;

    return thousandthsToDouble(rounded, negative);
}

double pythonRoundFloat32ToDecimalDigits(float x, int digits) {
    // Correctly round the exact float32 value to `digits` decimals.
    // float32 significand * 10^4 fits in 38 bits, so the 4-digit layout
    // score contract stays in uint64. Official only uses digits == 4.
    if (digits != 4) {
        assert(digits == 4 && "pythonRoundFloat32ToDecimalDigits only supports digits == 4");
        return double(x);
    }
    if (!std::isfinite(x) || x == 0.0f)
        return double(x);

    std::uint32_t bits = 0;
    std::memcpy(&bits, &x, sizeof(bits));
    const bool negative = (bits >> 31) != 0;
    const int exponentBits = int((bits >> 23) & 0xffU);
    const std::uint32_t fraction = bits & 0x7fffffU;

    std::uint64_t significand = 0;
    int binaryShift = 0;
    if (exponentBits == 0) {
        significand = fraction;
        binaryShift = -149;
    } else if (exponentBits == 255) {
        return double(x);
    } else {
        significand = std::uint64_t(fraction) | (std::uint64_t(1) << 23);
        binaryShift = exponentBits - 127 - 23;
    }

    const std::uint64_t scaled = significand * 10000U;
    if (binaryShift >= 0) {
        // |x| >= 2^23; layout scores are in (0, 1] and never reach here.
        return double(x);
    }

    const int right = -binaryShift;
    if (right >= 64)
        return negative ? -0.0 : 0.0;

    const std::uint64_t integerPart = scaled >> right;
    const std::uint64_t remainderMask = (std::uint64_t(1) << right) - 1;
    const std::uint64_t remainder = scaled & remainderMask;
    const std::uint64_t halfway = std::uint64_t(1) << (right - 1);
    std::uint64_t rounded = integerPart;
    if (remainder > halfway || (remainder == halfway && (integerPart & 1U)))
        ++rounded;

    const double value = double(rounded) / 10000.0;
    return negative ? -value : value;
}

}  // namespace hybrid
}  // namespace scanengine
