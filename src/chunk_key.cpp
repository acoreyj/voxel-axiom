#include "voxel/chunk_key.hpp"

namespace voxel {

// Static assertions ensuring Euclidean division and ChunkKey packing invariants
static_assert(ChunkKey::floor_div(-33, 32) == -2, "floor_div(-33, 32) must be -2");
static_assert(ChunkKey::floor_mod(-33, 32) == 31, "floor_mod(-33, 32) must be 31");
static_assert(ChunkKey::floor_div(-32, 32) == -1, "floor_div(-32, 32) must be -1");
static_assert(ChunkKey::floor_mod(-32, 32) == 0,  "floor_mod(-32, 32) must be 0");
static_assert(ChunkKey::floor_div(-31, 32) == -1, "floor_div(-31, 32) must be -1");
static_assert(ChunkKey::floor_mod(-31, 32) == 1,  "floor_mod(-31, 32) must be 1");
static_assert(ChunkKey::floor_div(-1, 32)  == -1, "floor_div(-1, 32) must be -1");
static_assert(ChunkKey::floor_mod(-1, 32)  == 31, "floor_mod(-1, 32) must be 31");
static_assert(ChunkKey::floor_div(0, 32)   == 0,  "floor_div(0, 32) must be 0");
static_assert(ChunkKey::floor_mod(0, 32)   == 0,  "floor_mod(0, 32) must be 0");
static_assert(ChunkKey::floor_div(1, 32)   == 0,  "floor_div(1, 32) must be 0");
static_assert(ChunkKey::floor_mod(1, 32)   == 1,  "floor_mod(1, 32) must be 1");
static_assert(ChunkKey::floor_div(31, 32)  == 0,  "floor_div(31, 32) must be 0");
static_assert(ChunkKey::floor_mod(31, 32)  == 31, "floor_mod(31, 32) must be 31");
static_assert(ChunkKey::floor_div(32, 32)  == 1,  "floor_div(32, 32) must be 1");
static_assert(ChunkKey::floor_mod(32, 32)  == 0,  "floor_mod(32, 32) must be 0");
static_assert(ChunkKey::floor_div(33, 32)  == 1,  "floor_div(33, 32) must be 1");
static_assert(ChunkKey::floor_mod(33, 32)  == 1,  "floor_mod(33, 32) must be 1");

static_assert(ChunkKey::from_coords(-1, -1, -1).x() == -1);
static_assert(ChunkKey::from_coords(-1, -1, -1).y() == -1);
static_assert(ChunkKey::from_coords(-1, -1, -1).z() == -1);

} // namespace voxel
