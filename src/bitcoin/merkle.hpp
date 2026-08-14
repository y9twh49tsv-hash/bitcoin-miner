#pragma once

#include <vector>

#include "bitcoin/hash.hpp"

namespace azd::bitcoin {

/// Merkle root from a full list of transaction hashes (internal byte order,
/// coinbase first) -- the definition used when validating a whole block.
///
/// Bitcoin duplicates the last hash when a level has an odd number of nodes.
/// An empty list yields a zero hash.
Hash256 merkleRootFromLeaves(const std::vector<Hash256>& leaves);

/// Merkle root from a coinbase hash plus the branch a Stratum pool sends.
///
///   root = fold(branches, coinbaseHash, [](acc, b) { SHA256d(acc || b); })
///
/// The coinbase is always the leftmost leaf, so every branch element is
/// concatenated on the RIGHT of the accumulator -- never the other way round.
/// All hashes are in internal byte order, which is exactly how the hex strings
/// in `mining.notify` arrive, so no reversal happens anywhere in here.
Hash256 merkleRootFromBranch(const Hash256& coinbaseHash, const std::vector<Hash256>& branches);

/// Build the Merkle branch for the coinbase (leftmost leaf) from a full leaf
/// list. This is what a pool computes and sends in `mining.notify`; having it
/// locally lets the tests check the branch path against the full-tree path.
std::vector<Hash256> merkleBranchForCoinbase(const std::vector<Hash256>& leaves);

} // namespace azd::bitcoin
