#pragma once

namespace scanengine {
namespace hybrid {

int roundHalfToEven(double x);
double roundToDecimalDigits(double x, int digits);

// Official PP-DocLayoutV2: round(float(score.item()), 4).
// score.item() is float32 promoted to float64; CPython 3.12 then correctly
// rounds that exact value to 4 decimal places (half to even) and converts
// the decimal back to binary64. Not the 3-digit bbox path.
double pythonRoundFloat32ToDecimalDigits(float x, int digits);

}  // namespace hybrid
}  // namespace scanengine
