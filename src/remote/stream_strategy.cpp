// Placeholder bodies so the payload tests build before the implementation.
#include "sub0pipeline/remote/strategy.hpp"

namespace sub0pipeline::remote
{
PayloadStrategyFactory streamStrategy(StreamOptions) { return {}; }
} // namespace sub0pipeline::remote
