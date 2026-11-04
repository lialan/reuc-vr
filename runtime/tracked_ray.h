#pragma once
#include <array>
namespace uc {
// Guest camera space, in game units. This is the laser's muzzle-to-hit ray.
struct TrackedRay { std::array<float,3> origin{}, direction{}; };
}
