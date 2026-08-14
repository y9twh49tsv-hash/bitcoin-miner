#include "bitcoin/merkle.hpp"

namespace azd::bitcoin {

Hash256 merkleRootFromLeaves(const std::vector<Hash256>& leaves) {
    if (leaves.empty()) return Hash256::zero();

    std::vector<Hash256> level = leaves;
    while (level.size() > 1) {
        if (level.size() % 2 != 0) level.push_back(level.back());

        std::vector<Hash256> next;
        next.reserve(level.size() / 2);
        for (size_t i = 0; i < level.size(); i += 2) {
            next.push_back(doubleSha256Concat(level[i], level[i + 1]));
        }
        level.swap(next);
    }
    return level.front();
}

Hash256 merkleRootFromBranch(const Hash256& coinbaseHash, const std::vector<Hash256>& branches) {
    Hash256 acc = coinbaseHash;
    for (const Hash256& branch : branches) {
        acc = doubleSha256Concat(acc, branch);
    }
    return acc;
}

std::vector<Hash256> merkleBranchForCoinbase(const std::vector<Hash256>& leaves) {
    std::vector<Hash256> branch;
    if (leaves.size() <= 1) return branch;

    std::vector<Hash256> level = leaves;
    while (level.size() > 1) {
        if (level.size() % 2 != 0) level.push_back(level.back());

        // The coinbase is index 0, so its sibling is always index 1.
        branch.push_back(level[1]);

        std::vector<Hash256> next;
        next.reserve(level.size() / 2);
        for (size_t i = 0; i < level.size(); i += 2) {
            next.push_back(doubleSha256Concat(level[i], level[i + 1]));
        }
        level.swap(next);
    }
    return branch;
}

} // namespace azd::bitcoin
