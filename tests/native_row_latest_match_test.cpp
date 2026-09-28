#include "src/native_row_latest_match.hpp"
using namespace NativeRowLatestMatch;
static_assert(Precedes(100,100));
static_assert(Precedes(100,101));
static_assert(!Precedes(0,101));
static_assert(!Precedes(102,101));
static_assert(FenceAllows(2,2));
static_assert(FenceAllows(2,3));
static_assert(!FenceAllows(0,3));
static_assert(!FenceAllows(4,3));
static_assert(Later(6,100,5,999));
static_assert(Later(6,101,6,100));
static_assert(!Later(5,101,6,100));
static_assert(!Later(6,100,6,100));
int main() {}
