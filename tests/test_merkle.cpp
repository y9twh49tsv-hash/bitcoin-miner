// Merkle root construction, both from a full leaf list and from the branch a
// Stratum pool sends.
//
// Byte order is the whole game here: txids are DISPLAYED reversed, but hashed
// in internal order. Mixing the two produces a valid-looking root that no pool
// will ever accept.

#include <string>
#include <vector>

#include "bitcoin/hash.hpp"
#include "bitcoin/merkle.hpp"
#include "test_framework.hpp"
#include "util/hex.hpp"

using azd::bitcoin::Hash256;
using azd::bitcoin::merkleBranchForCoinbase;
using azd::bitcoin::merkleRootFromBranch;
using azd::bitcoin::merkleRootFromLeaves;

namespace {

Hash256 txid(const std::string& displayHex) {
    Hash256 hash;
    REQUIRE(Hash256::fromDisplayHex(displayHex, hash));
    return hash;
}

/// Deterministic pseudo-leaves for structural tests.
Hash256 syntheticLeaf(uint32_t index) {
    std::vector<uint8_t> data(4);
    data[0] = static_cast<uint8_t>(index);
    data[1] = static_cast<uint8_t>(index >> 8);
    data[2] = static_cast<uint8_t>(index >> 16);
    data[3] = static_cast<uint8_t>(index >> 24);
    return azd::bitcoin::doubleSha256(data);
}

} // namespace

TEST("single leaf: the merkle root is the coinbase hash itself") {
    // Bitcoin block 1 contains only its coinbase transaction, so the block's
    // merkle root equals that transaction's id.
    const Hash256 coinbase =
        txid("0e3e2357e806b6cdb1f70b54c3a3a17b6714ee1f0e68bebb44a74b1efd512098");

    CHECK_EQ(merkleRootFromLeaves({coinbase}).toDisplayHex(),
             std::string("0e3e2357e806b6cdb1f70b54c3a3a17b6714ee1f0e68bebb44a74b1efd512098"));

    // With an empty branch the stratum path must agree.
    CHECK_EQ(merkleRootFromBranch(coinbase, {}).toDisplayHex(),
             std::string("0e3e2357e806b6cdb1f70b54c3a3a17b6714ee1f0e68bebb44a74b1efd512098"));
}

TEST("two leaves: Bitcoin block 170 reproduces its real merkle root") {
    // Block 170 -- the block containing the first ever Bitcoin payment
    // (Satoshi -> Hal Finney). Two transactions, so exactly one hashing level.
    const Hash256 coinbase =
        txid("b1fea52486ce0c62bb442b530a3f0132b826c74e473d1f2c220bfa78111c5082");
    const Hash256 payment =
        txid("f4184fc596403b9d638783cf57adfe4c75c605f6356fbc91338530e9831e9e16");

    const std::string expectedRoot =
        "7dac2c5666815c17a3b36427de37bb9d2e2c5ccec3f8633eb91a4205cb4c10ff";

    CHECK_EQ(merkleRootFromLeaves({coinbase, payment}).toDisplayHex(), expectedRoot);

    // The stratum form: coinbase plus a one-element branch.
    CHECK_EQ(merkleRootFromBranch(coinbase, {payment}).toDisplayHex(), expectedRoot);
}

TEST("the branch path and the full-tree path always agree") {
    // For every tree size, building the root from the coinbase + its branch
    // must equal building it from all the leaves. This covers the odd-node
    // duplication rule at several levels.
    for (uint32_t count = 1; count <= 17; ++count) {
        std::vector<Hash256> leaves;
        for (uint32_t i = 0; i < count; ++i) leaves.push_back(syntheticLeaf(i));

        const Hash256 fullRoot = merkleRootFromLeaves(leaves);
        const std::vector<Hash256> branch = merkleBranchForCoinbase(leaves);
        const Hash256 branchRoot = merkleRootFromBranch(leaves.front(), branch);

        CHECK_EQ(branchRoot.toDisplayHex(), fullRoot.toDisplayHex());
    }
}

TEST("branch length matches the tree depth") {
    struct Case {
        uint32_t leaves;
        size_t expectedBranchLength;
    };
    const Case cases[] = {{1, 0}, {2, 1}, {3, 2}, {4, 2}, {5, 3}, {8, 3}, {9, 4}, {16, 4}};

    for (const Case& testCase : cases) {
        std::vector<Hash256> leaves;
        for (uint32_t i = 0; i < testCase.leaves; ++i) leaves.push_back(syntheticLeaf(i));
        CHECK_EQ(merkleBranchForCoinbase(leaves).size(), testCase.expectedBranchLength);
    }
}

TEST("odd node counts duplicate the last hash") {
    // Three leaves: level 1 is [H(a,b), H(c,c)].
    const Hash256 a = syntheticLeaf(1);
    const Hash256 b = syntheticLeaf(2);
    const Hash256 c = syntheticLeaf(3);

    const Hash256 ab = azd::bitcoin::doubleSha256Concat(a, b);
    const Hash256 cc = azd::bitcoin::doubleSha256Concat(c, c);
    const Hash256 expected = azd::bitcoin::doubleSha256Concat(ab, cc);

    CHECK_EQ(merkleRootFromLeaves({a, b, c}).toDisplayHex(), expected.toDisplayHex());
}

TEST("branch elements are concatenated on the right of the accumulator") {
    // The coinbase is the leftmost leaf, so order matters: SHA256d(acc || branch)
    // and SHA256d(branch || acc) must differ.
    const Hash256 coinbase = syntheticLeaf(100);
    const Hash256 sibling = syntheticLeaf(200);

    const Hash256 correct = merkleRootFromBranch(coinbase, {sibling});
    const Hash256 swapped = azd::bitcoin::doubleSha256Concat(sibling, coinbase);

    CHECK(correct != swapped);
    CHECK_EQ(correct.toDisplayHex(),
             azd::bitcoin::doubleSha256Concat(coinbase, sibling).toDisplayHex());
}

TEST("multi-level branch folds in order") {
    const Hash256 coinbase = syntheticLeaf(0);
    const std::vector<Hash256> branch = {syntheticLeaf(1), syntheticLeaf(2), syntheticLeaf(3)};

    Hash256 expected = coinbase;
    for (const Hash256& element : branch) {
        expected = azd::bitcoin::doubleSha256Concat(expected, element);
    }

    CHECK_EQ(merkleRootFromBranch(coinbase, branch).toDisplayHex(), expected.toDisplayHex());

    // Reversing the branch must produce a different root.
    std::vector<Hash256> reversed(branch.rbegin(), branch.rend());
    CHECK(merkleRootFromBranch(coinbase, reversed) != merkleRootFromBranch(coinbase, branch));
}

TEST("empty leaf list yields a zero hash rather than garbage") {
    CHECK(merkleRootFromLeaves({}).isZero());
}

AZD_TEST_MAIN("merkle")
