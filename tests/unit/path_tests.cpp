#include "mint/runtime/path.hpp"

#include <filesystem>

#include <gtest/gtest.h>

namespace {

TEST(PathTest, DistinguishesDescendantsFromPrefixesAndParents) {
    const auto root = std::filesystem::temp_directory_path() / "mint-path-root";

    EXPECT_TRUE(mint::is_path_within(root, root));
    EXPECT_TRUE(mint::is_path_within(root, root / "src" / "main.cpp"));
    EXPECT_FALSE(mint::is_path_within(root, root.parent_path()));
    EXPECT_FALSE(mint::is_path_within(root, root.parent_path() / "mint-path-root-sibling"));
}

TEST(PathTest, NormalizesLexicalComponentsAndRejectsRelativeInputs) {
    const auto root = std::filesystem::temp_directory_path() / "mint-path-root";

    EXPECT_TRUE(mint::is_path_within(root, root / "src" / ".." / "include"));
    EXPECT_FALSE(mint::is_path_within(root, root / ".." / "outside"));
    EXPECT_FALSE(mint::is_path_within("relative", "relative/child"));
    EXPECT_FALSE(mint::is_path_within({}, root));
}

TEST(PathTest, UsesConservativePlatformCaseComparison) {
    const auto parent = std::filesystem::temp_directory_path();
    const auto root = parent / "Mint-Path-Root";
    const auto differently_cased_child = parent / "mint-path-root" / "src";

#if defined(_WIN32) || defined(__APPLE__)
    EXPECT_TRUE(mint::is_path_within(root, differently_cased_child));
#else
    EXPECT_FALSE(mint::is_path_within(root, differently_cased_child));
#endif
}

} // namespace
