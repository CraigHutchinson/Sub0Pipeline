// Placeholder bodies so the payload tests build before the implementation.
#include "sub0pipeline/remote/strategy.hpp"

namespace sub0pipeline::remote
{
PayloadStrategyFactory storeStrategy(IPayloadStore&) { return {}; }
} // namespace sub0pipeline::remote
