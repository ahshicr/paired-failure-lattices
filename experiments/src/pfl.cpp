#include "pfl/pfl.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace pfl {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

std::size_t floor_log2(std::size_t value) {
    if (value == 0) {
        throw std::invalid_argument("logarithm argument must be positive");
    }
    std::size_t result = 0;
    while (value > 1U) {
        value >>= 1U;
        ++result;
    }
    return result;
}

int population_count(std::uint64_t value) noexcept {
    int result = 0;
    while (value != 0U) {
        value &= value - 1U;
        ++result;
    }
    return result;
}

std::uint64_t checked_u64(Score value, const char* message) {
    if (value > std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error(message);
    }
    return static_cast<std::uint64_t>(value);
}

Mass checked_add_mass(Mass left, Mass right, const char* message) {
    if (right > std::numeric_limits<Mass>::max() - left) {
        throw std::overflow_error(message);
    }
    return left + right;
}

std::vector<NodeId> nodes_by_depth(const ReducedTree& tree, bool descending) {
    std::vector<NodeId> nodes(tree.size());
    std::iota(nodes.begin(), nodes.end(), NodeId{0});
    std::sort(nodes.begin(), nodes.end(), [&](NodeId left, NodeId right) {
        if (tree.depth()[static_cast<std::size_t>(left)] !=
            tree.depth()[static_cast<std::size_t>(right)]) {
            if (descending) {
                return tree.depth()[static_cast<std::size_t>(left)] >
                       tree.depth()[static_cast<std::size_t>(right)];
            }
            return tree.depth()[static_cast<std::size_t>(left)] <
                   tree.depth()[static_cast<std::size_t>(right)];
        }
        return left < right;
    });
    return nodes;
}

std::vector<NodeId> validate_recovery_tree(const RecoveryInstance& instance,
                                           std::vector<std::vector<NodeId>>& children,
                                           NodeId& root) {
    const std::size_t n = instance.parent.size();
    if (n == 0 || instance.benefit.size() != n || instance.menus.size() != n) {
        throw std::invalid_argument("recovery arrays must be nonempty and have equal length");
    }
    if (n > static_cast<std::size_t>(std::numeric_limits<NodeId>::max())) {
        throw std::invalid_argument("recovery tree has too many nodes");
    }
    children.assign(n, {});
    root = -1;
    std::size_t roots = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const NodeId parent = instance.parent[i];
        if (parent == -1) {
            root = static_cast<NodeId>(i);
            ++roots;
            continue;
        }
        if (parent < 0 || static_cast<std::size_t>(parent) >= n ||
            parent == static_cast<NodeId>(i)) {
            throw std::invalid_argument("invalid recovery parent array");
        }
        children[static_cast<std::size_t>(parent)].push_back(static_cast<NodeId>(i));
    }
    if (roots != 1) {
        throw std::invalid_argument("recovery tree must have exactly one root");
    }
    std::vector<NodeId> order;
    order.reserve(n);
    order.push_back(root);
    for (std::size_t cursor = 0; cursor < order.size(); ++cursor) {
        const NodeId node = order[cursor];
        for (const NodeId child : children[static_cast<std::size_t>(node)]) {
            order.push_back(child);
        }
    }
    if (order.size() != n) {
        throw std::invalid_argument("recovery parent array is disconnected or cyclic");
    }
    return order;
}

void require_recovery_score_bound(const RecoveryInstance& instance,
                                  Mass total_mass,
                                  const char* message) {
    const Score maximum = ~Score{0U};
    Score total_benefit = 0U;
    for (const Benefit benefit : instance.benefit) {
        if (total_benefit > maximum - benefit) {
            throw std::overflow_error(message);
        }
        total_benefit += benefit;
    }
    if (total_mass != 0U && total_benefit > maximum / total_mass) {
        throw std::overflow_error(message);
    }
}

}  // namespace

class PlusMinusOneRMQ {
public:
    explicit PlusMinusOneRMQ(std::vector<std::int32_t> values)
        : values_(std::move(values)), n_(values_.size()) {
        if (values_.empty()) {
            throw std::invalid_argument("RMQ requires a nonempty sequence");
        }
        for (std::size_t i = 1; i < values_.size(); ++i) {
            const std::int64_t delta = static_cast<std::int64_t>(values_[i]) - values_[i - 1];
            if (delta != 1 && delta != -1) {
                throw std::invalid_argument("RMQ depth sequence is not plus or minus one");
            }
        }
        block_size_ = std::max<std::size_t>(1, floor_log2(std::max<std::size_t>(2, n_)) / 2);
        block_size_ = std::min<std::size_t>(block_size_, 31);
        block_count_ = (n_ + block_size_ - 1) / block_size_;
        block_key_.reserve(block_count_);
        block_min_index_.reserve(block_count_);

        for (std::size_t block = 0; block < block_count_; ++block) {
            const std::size_t start = block * block_size_;
            const std::size_t length = std::min(block_size_, n_ - start);
            std::uint64_t code = 0;
            for (std::size_t offset = 1; offset < length; ++offset) {
                if (values_[start + offset] > values_[start + offset - 1]) {
                    code |= std::uint64_t{1} << (offset - 1);
                }
            }
            const std::uint64_t key = (static_cast<std::uint64_t>(length) << 56U) | code;
            block_key_.push_back(key);
            if (micro_tables_.find(key) == micro_tables_.end()) {
                std::vector<std::int32_t> relative(length, 0);
                for (std::size_t offset = 1; offset < length; ++offset) {
                    relative[offset] = relative[offset - 1] +
                        ((code & (std::uint64_t{1} << (offset - 1))) != 0 ? 1 : -1);
                }
                std::vector<std::uint8_t> table(length * length, 0);
                for (std::size_t left = 0; left < length; ++left) {
                    std::size_t best = left;
                    for (std::size_t right = left; right < length; ++right) {
                        if (relative[right] < relative[best]) {
                            best = right;
                        }
                        table[left * length + right] = static_cast<std::uint8_t>(best);
                    }
                }
                micro_tables_.emplace(key, std::move(table));
            }
            block_min_index_.push_back(within_block(block, 0, length - 1));
        }

        sparse_.push_back(std::vector<std::size_t>(block_count_));
        std::iota(sparse_.front().begin(), sparse_.front().end(), std::size_t{0});
        for (std::size_t level = 1; (std::size_t{1} << level) <= block_count_; ++level) {
            const std::size_t width = std::size_t{1} << level;
            const std::size_t half = width / 2;
            std::vector<std::size_t> row(block_count_ - width + 1);
            for (std::size_t left = 0; left + width <= block_count_; ++left) {
                row[left] = better_block(sparse_[level - 1][left],
                                         sparse_[level - 1][left + half]);
            }
            sparse_.push_back(std::move(row));
        }
    }

    [[nodiscard]] std::size_t query_index(std::size_t left, std::size_t right) const {
        if (left >= n_ || right >= n_) {
            throw std::out_of_range("RMQ query index is out of range");
        }
        if (left > right) {
            std::swap(left, right);
        }
        const std::size_t block_left = left / block_size_;
        const std::size_t block_right = right / block_size_;
        if (block_left == block_right) {
            return within_block(block_left, left % block_size_, right % block_size_);
        }
        std::size_t best = within_block(
            block_left,
            left % block_size_,
            block_length(block_left) - 1
        );
        best = better_index(best, within_block(block_right, 0, right % block_size_));
        if (block_left + 1 <= block_right - 1) {
            best = better_index(best, whole_blocks(block_left + 1, block_right - 1));
        }
        return best;
    }

    [[nodiscard]] std::size_t storage_bytes() const noexcept {
        std::size_t bytes = values_.capacity() * sizeof(values_[0]);
        bytes += block_key_.capacity() * sizeof(block_key_[0]);
        bytes += block_min_index_.capacity() * sizeof(block_min_index_[0]);
        for (const auto& [key, table] : micro_tables_) {
            (void)key;
            bytes += sizeof(key) + table.capacity() * sizeof(table[0]);
        }
        for (const auto& row : sparse_) {
            bytes += row.capacity() * sizeof(row[0]);
        }
        return bytes;
    }

private:
    [[nodiscard]] std::size_t block_length(std::size_t block) const noexcept {
        return std::min(block_size_, n_ - block * block_size_);
    }

    [[nodiscard]] std::size_t better_index(std::size_t left, std::size_t right) const noexcept {
        if (values_[left] != values_[right]) {
            return values_[left] < values_[right] ? left : right;
        }
        return std::min(left, right);
    }

    [[nodiscard]] std::size_t better_block(std::size_t left, std::size_t right) const noexcept {
        const std::size_t left_index = block_min_index_[left];
        const std::size_t right_index = block_min_index_[right];
        return better_index(left_index, right_index) == left_index ? left : right;
    }

    [[nodiscard]] std::size_t within_block(std::size_t block,
                                           std::size_t left,
                                           std::size_t right) const {
        const std::uint64_t key = block_key_[block];
        const std::size_t length = static_cast<std::size_t>(key >> 56U);
        const auto iterator = micro_tables_.find(key);
        if (iterator == micro_tables_.end() || left > right || right >= length) {
            throw std::logic_error("invalid micro RMQ query");
        }
        return block * block_size_ + iterator->second[left * length + right];
    }

    [[nodiscard]] std::size_t whole_blocks(std::size_t left, std::size_t right) const {
        const std::size_t count = right - left + 1;
        const std::size_t level = floor_log2(count);
        const std::size_t width = std::size_t{1} << level;
        const std::size_t first = sparse_[level][left];
        const std::size_t second = sparse_[level][right - width + 1];
        return block_min_index_[better_block(first, second)];
    }

    std::vector<std::int32_t> values_;
    std::size_t n_ = 0;
    std::size_t block_size_ = 1;
    std::size_t block_count_ = 0;
    std::vector<std::uint64_t> block_key_;
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> micro_tables_;
    std::vector<std::size_t> block_min_index_;
    std::vector<std::vector<std::size_t>> sparse_;
};

std::string score_to_string(Score value) {
    if (value == 0) {
        return "0";
    }
    std::string result;
    while (value != 0) {
        result.push_back(static_cast<char>('0' + value % 10));
        value /= 10;
    }
    std::reverse(result.begin(), result.end());
    return result;
}

ReducedTree::ReducedTree(std::vector<NodeId> parent) : parent_(std::move(parent)) {
    if (parent_.empty()) {
        throw std::invalid_argument("domain tree must be nonempty");
    }
    if (parent_.size() > static_cast<std::size_t>(std::numeric_limits<NodeId>::max())) {
        throw std::invalid_argument("domain tree has too many nodes");
    }
    children_.assign(parent_.size(), {});
    std::size_t roots = 0;
    for (std::size_t index = 0; index < parent_.size(); ++index) {
        const NodeId parent_node = parent_[index];
        if (parent_node == -1) {
            root_ = static_cast<NodeId>(index);
            ++roots;
            continue;
        }
        if (parent_node < 0 || static_cast<std::size_t>(parent_node) >= parent_.size() ||
            parent_node == static_cast<NodeId>(index)) {
            throw std::invalid_argument("invalid domain parent array");
        }
        children_[static_cast<std::size_t>(parent_node)].push_back(static_cast<NodeId>(index));
    }
    if (roots != 1) {
        throw std::invalid_argument("domain tree must have exactly one root");
    }
    for (const auto& row : children_) {
        if (row.size() == 1) {
            throw std::invalid_argument("domain tree is not reduced because it has a unary node");
        }
    }

    depth_.assign(parent_.size(), 0);
    tin_.assign(parent_.size(), 0);
    tout_.assign(parent_.size(), 0);
    first_.assign(parent_.size(), std::numeric_limits<std::size_t>::max());
    euler_.reserve(2 * parent_.size() - 1);
    euler_depth_.reserve(2 * parent_.size() - 1);

    struct Frame {
        NodeId node;
        std::size_t next_child;
    };
    std::vector<Frame> stack;
    stack.push_back({root_, 0});
    std::uint64_t clock = 0;
    tin_[static_cast<std::size_t>(root_)] = clock++;
    first_[static_cast<std::size_t>(root_)] = 0;
    euler_.push_back(root_);
    euler_depth_.push_back(0);
    std::size_t visited = 1;

    while (!stack.empty()) {
        Frame& frame = stack.back();
        const auto& row = children_[static_cast<std::size_t>(frame.node)];
        if (frame.next_child < row.size()) {
            const NodeId child = row[frame.next_child++];
            const std::uint64_t next_depth =
                static_cast<std::uint64_t>(depth_[static_cast<std::size_t>(frame.node)]) + 1;
            if (next_depth > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
                throw std::invalid_argument("domain tree depth exceeds the supported range");
            }
            depth_[static_cast<std::size_t>(child)] = static_cast<std::uint32_t>(next_depth);
            tin_[static_cast<std::size_t>(child)] = clock++;
            first_[static_cast<std::size_t>(child)] = euler_.size();
            euler_.push_back(child);
            euler_depth_.push_back(static_cast<std::int32_t>(next_depth));
            stack.push_back({child, 0});
            ++visited;
        } else {
            tout_[static_cast<std::size_t>(frame.node)] = clock;
            stack.pop_back();
            if (!stack.empty()) {
                const NodeId parent_node = stack.back().node;
                euler_.push_back(parent_node);
                euler_depth_.push_back(
                    static_cast<std::int32_t>(depth_[static_cast<std::size_t>(parent_node)])
                );
            }
        }
    }
    if (visited != parent_.size()) {
        throw std::invalid_argument("domain parent array is disconnected or cyclic");
    }
    for (std::size_t node = 0; node < children_.size(); ++node) {
        if (children_[node].empty()) {
            leaves_.push_back(static_cast<NodeId>(node));
        }
    }
    rmq_ = std::make_unique<PlusMinusOneRMQ>(euler_depth_);
}

ReducedTree::~ReducedTree() = default;
ReducedTree::ReducedTree(ReducedTree&&) noexcept = default;
ReducedTree& ReducedTree::operator=(ReducedTree&&) noexcept = default;

std::size_t ReducedTree::size() const noexcept { return parent_.size(); }
NodeId ReducedTree::root() const noexcept { return root_; }
const std::vector<NodeId>& ReducedTree::parent() const noexcept { return parent_; }
const std::vector<std::vector<NodeId>>& ReducedTree::children() const noexcept { return children_; }
const std::vector<std::uint32_t>& ReducedTree::depth() const noexcept { return depth_; }
const std::vector<NodeId>& ReducedTree::leaves() const noexcept { return leaves_; }

void ReducedTree::require_node(NodeId node) const {
    if (node < 0 || static_cast<std::size_t>(node) >= size()) {
        throw std::out_of_range("domain node is out of range");
    }
}

bool ReducedTree::is_leaf(NodeId node) const {
    require_node(node);
    return children_[static_cast<std::size_t>(node)].empty();
}

bool ReducedTree::is_ancestor(NodeId ancestor, NodeId node) const {
    require_node(ancestor);
    require_node(node);
    return tin_[static_cast<std::size_t>(ancestor)] <= tin_[static_cast<std::size_t>(node)] &&
           tin_[static_cast<std::size_t>(node)] < tout_[static_cast<std::size_t>(ancestor)];
}

NodeId ReducedTree::lca(NodeId left, NodeId right) const {
    require_node(left);
    require_node(right);
    if (left == right) {
        return left;
    }
    const std::size_t index = rmq_->query_index(first_[static_cast<std::size_t>(left)],
                                                 first_[static_cast<std::size_t>(right)]);
    return euler_[index];
}

NodeId ReducedTree::lca_many(const std::vector<NodeId>& nodes) const {
    if (nodes.empty()) {
        throw std::invalid_argument("LCA of an empty collection is undefined");
    }
    NodeId result = nodes.front();
    require_node(result);
    for (std::size_t index = 1; index < nodes.size(); ++index) {
        result = lca(result, nodes[index]);
    }
    return result;
}

std::size_t ReducedTree::structural_bytes() const noexcept {
    std::size_t bytes = parent_.capacity() * sizeof(parent_[0]);
    bytes += depth_.capacity() * sizeof(depth_[0]);
    bytes += tin_.capacity() * sizeof(tin_[0]);
    bytes += tout_.capacity() * sizeof(tout_[0]);
    bytes += first_.capacity() * sizeof(first_[0]);
    bytes += euler_.capacity() * sizeof(euler_[0]);
    bytes += euler_depth_.capacity() * sizeof(euler_depth_[0]);
    bytes += leaves_.capacity() * sizeof(leaves_[0]);
    for (const auto& row : children_) {
        bytes += row.capacity() * sizeof(row[0]);
    }
    if (rmq_) {
        bytes += rmq_->storage_bytes();
    }
    return bytes;
}

Label Label::bottom() noexcept { return {LabelTag::bottom, -1, -1}; }
Label Label::single(NodeId node) noexcept { return {LabelTag::single, node, -1}; }
Label Label::pair(NodeId first_node, NodeId second_node) noexcept {
    return {LabelTag::pair, first_node, second_node};
}
Label Label::invalid() noexcept { return {LabelTag::invalid, -1, -1}; }

bool operator<(const Label& left, const Label& right) noexcept {
    return std::tie(left.tag, left.first, left.second) <
           std::tie(right.tag, right.first, right.second);
}

std::size_t LabelHash::operator()(const Label& label) const noexcept {
    std::uint64_t hash = kFnvOffset;
    hash = fnv1a_update(hash, static_cast<std::uint8_t>(label.tag));
    hash = fnv1a_update(hash, static_cast<std::uint32_t>(label.first));
    hash = fnv1a_update(hash, static_cast<std::uint32_t>(label.second));
    return static_cast<std::size_t>(hash);
}

LatticeOracle::LatticeOracle(const ReducedTree& tree) noexcept : tree_(&tree) {}
const ReducedTree& LatticeOracle::tree() const noexcept { return *tree_; }

bool LatticeOracle::valid(const Label& label) const noexcept {
    const auto in_range = [&](NodeId node) {
        return node >= 0 && static_cast<std::size_t>(node) < tree_->size();
    };
    try {
        if (label.tag == LabelTag::bottom) {
            return label.first == -1 && label.second == -1;
        }
        if (label.tag == LabelTag::single) {
            return in_range(label.first) && label.second == -1;
        }
        if (label.tag != LabelTag::pair || !in_range(label.first) ||
            !in_range(label.second) || label.first >= label.second) {
            return false;
        }
        if (tree_->is_ancestor(label.first, label.second) ||
            tree_->is_ancestor(label.second, label.first)) {
            return false;
        }
        const NodeId parent = tree_->parent()[static_cast<std::size_t>(label.first)];
        if (parent >= 0 &&
            tree_->parent()[static_cast<std::size_t>(label.second)] == parent &&
            tree_->children()[static_cast<std::size_t>(parent)].size() == 2) {
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

std::optional<bool> LatticeOracle::equal(const Label& left, const Label& right) const noexcept {
    if (!valid(left) || !valid(right)) {
        return std::nullopt;
    }
    return left == right;
}

std::vector<NodeId> LatticeOracle::components(const Label& label) const {
    if (!valid(label)) {
        throw std::invalid_argument("label is not normalized");
    }
    if (label.tag == LabelTag::bottom) {
        return {};
    }
    if (label.tag == LabelTag::single) {
        return {label.first};
    }
    return {label.first, label.second};
}

std::vector<NodeId> LatticeOracle::reduce_components(const std::vector<NodeId>& nodes) const {
    std::vector<NodeId> unique;
    unique.reserve(nodes.size());
    for (const NodeId node : nodes) {
        if (std::find(unique.begin(), unique.end(), node) == unique.end()) {
            unique.push_back(node);
        }
    }
    std::vector<NodeId> reduced;
    reduced.reserve(unique.size());
    for (const NodeId node : unique) {
        bool dominated = false;
        for (const NodeId other : unique) {
            if (other != node && tree_->is_ancestor(other, node)) {
                dominated = true;
                break;
            }
        }
        if (!dominated) {
            reduced.push_back(node);
        }
    }
    return reduced;
}

Label LatticeOracle::normalize(const std::vector<NodeId>& nodes) const noexcept {
    if (nodes.size() > 2) {
        return Label::invalid();
    }
    for (const NodeId node : nodes) {
        if (node < 0 || static_cast<std::size_t>(node) >= tree_->size()) {
            return Label::invalid();
        }
    }
    try {
        std::vector<NodeId> reduced = reduce_components(nodes);
        if (reduced.empty()) {
            return Label::bottom();
        }
        if (reduced.size() == 1) {
            return Label::single(reduced.front());
        }
        if (reduced.size() != 2) {
            return Label::invalid();
        }
        NodeId left = std::min(reduced[0], reduced[1]);
        NodeId right = std::max(reduced[0], reduced[1]);
        const NodeId parent = tree_->parent()[static_cast<std::size_t>(left)];
        if (parent >= 0 && tree_->parent()[static_cast<std::size_t>(right)] == parent &&
            tree_->children()[static_cast<std::size_t>(parent)].size() == 2) {
            return Label::single(parent);
        }
        return Label::pair(left, right);
    } catch (...) {
        return Label::invalid();
    }
}

bool LatticeOracle::component_subset(const Label& left, const Label& right) const {
    const std::vector<NodeId> left_nodes = components(left);
    const std::vector<NodeId> right_nodes = components(right);
    for (const NodeId component : left_nodes) {
        bool covered = false;
        for (const NodeId container : right_nodes) {
            if (tree_->is_ancestor(container, component)) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            return false;
        }
    }
    return true;
}

std::optional<bool> LatticeOracle::order(const Label& left, const Label& right) const noexcept {
    if (!valid(left) || !valid(right)) {
        return std::nullopt;
    }
    try {
        return component_subset(left, right);
    } catch (...) {
        return std::nullopt;
    }
}

Label LatticeOracle::meet(const Label& left, const Label& right) const {
    if (!valid(left) || !valid(right)) {
        return Label::invalid();
    }
    std::vector<NodeId> intersections;
    for (const NodeId a : components(left)) {
        for (const NodeId b : components(right)) {
            if (tree_->is_ancestor(a, b)) {
                intersections.push_back(b);
            } else if (tree_->is_ancestor(b, a)) {
                intersections.push_back(a);
            }
        }
    }
    std::vector<NodeId> reduced = reduce_components(intersections);
    if (reduced.size() > 2) {
        throw std::logic_error("intersection closure was violated");
    }
    return normalize(reduced);
}

Label LatticeOracle::join(const Label& left, const Label& right) const {
    if (!valid(left) || !valid(right)) {
        return Label::invalid();
    }
    std::vector<NodeId> nodes = components(left);
    const std::vector<NodeId> right_nodes = components(right);
    nodes.insert(nodes.end(), right_nodes.begin(), right_nodes.end());
    std::vector<NodeId> reduced = reduce_components(nodes);
    if (reduced.empty()) {
        return Label::bottom();
    }
    if (reduced.size() > 4) {
        throw std::logic_error("binary join exposed more than four components");
    }

    std::vector<Label> candidates;
    const auto add_candidate = [&](Label candidate, std::vector<Label>& output) {
        if (!valid(candidate)) {
            throw std::logic_error("join generated an invalid candidate");
        }
        if (std::find(output.begin(), output.end(), candidate) == output.end()) {
            output.push_back(candidate);
        }
    };
    add_candidate(normalize({tree_->lca_many(reduced)}), candidates);

    const std::size_t tail_size = reduced.size() - 1;
    const std::size_t partition_count = std::size_t{1} << tail_size;
    for (std::size_t mask = 0; mask < partition_count; ++mask) {
        std::vector<NodeId> first_block{reduced.front()};
        std::vector<NodeId> second_block;
        for (std::size_t index = 0; index < tail_size; ++index) {
            if ((mask & (std::size_t{1} << index)) != 0) {
                first_block.push_back(reduced[index + 1]);
            } else {
                second_block.push_back(reduced[index + 1]);
            }
        }
        if (second_block.empty()) {
            continue;
        }
        add_candidate(
            normalize({tree_->lca_many(first_block), tree_->lca_many(second_block)}),
            candidates
        );
    }

    std::optional<Label> least;
    for (const Label& candidate : candidates) {
        bool below_all = true;
        for (const Label& other : candidates) {
            const auto comparison = order(candidate, other);
            if (!comparison.has_value() || !comparison.value()) {
                below_all = false;
                break;
            }
        }
        if (below_all) {
            if (least.has_value() && least.value() != candidate) {
                throw std::logic_error("partition join has multiple least candidates");
            }
            least = candidate;
        }
    }
    if (!least.has_value()) {
        throw std::logic_error("partition join has no least candidate");
    }
    return least.value();
}

Label LatticeOracle::hull_of_leaves(const std::vector<NodeId>& leaves) const {
    Label result = Label::bottom();
    for (const NodeId leaf : leaves) {
        if (leaf < 0 || static_cast<std::size_t>(leaf) >= tree_->size() || !tree_->is_leaf(leaf)) {
            return Label::invalid();
        }
        result = join(result, Label::single(leaf));
        if (!valid(result)) {
            throw std::logic_error("leaf hull accumulation produced an invalid label");
        }
    }
    return result;
}

StateCount LatticeOracle::exact_state_count() const {
    StateCount result;
    result.nodes = static_cast<std::uint64_t>(tree_->size());
    result.leaves = static_cast<std::uint64_t>(tree_->leaves().size());
    result.internal_nodes = result.nodes - result.leaves;
    Score comparable_pairs = 0;
    for (std::size_t node = 0; node < tree_->size(); ++node) {
        comparable_pairs += tree_->depth()[node];
        const std::size_t degree = tree_->children()[node].size();
        result.binary_internal_nodes += degree == 2 ? 1U : 0U;
        result.high_degree_internal_nodes += degree >= 3 ? 1U : 0U;
    }
    const Score all_pairs = Score{result.nodes} * (result.nodes - 1) / 2;
    if (comparable_pairs > all_pairs) {
        throw std::logic_error("comparable pair count exceeds all pairs");
    }
    result.incomparable_pairs = checked_u64(all_pairs - comparable_pairs,
                                             "incomparable pair count overflow");
    const Score state_count = Score{result.nodes} + result.incomparable_pairs -
                              result.binary_internal_nodes;
    result.nonempty_states = checked_u64(state_count, "state count overflow");
    result.states_with_bottom = checked_u64(state_count + 1, "state count overflow");
    result.lower_envelope = checked_u64(
        Score{result.leaves} * (result.leaves + 1) / 2,
        "lower envelope overflow"
    );
    result.upper_envelope = checked_u64(
        Score{result.nodes} * (result.nodes - 1) / 2 + 1,
        "upper envelope overflow"
    );
    if (result.nonempty_states != result.leaves + result.high_degree_internal_nodes +
                                      result.incomparable_pairs) {
        throw std::logic_error("the two exact state count identities disagree");
    }
    return result;
}

ExplicitOracle::ExplicitOracle(const ReducedTree& tree, const LatticeOracle& lattice)
    : tree_(&tree), lattice_(&lattice), node_masks_(tree.size(), 0) {
    if (tree.leaves().size() > 63) {
        throw std::invalid_argument("explicit oracle supports at most 63 leaves");
    }
    std::unordered_map<NodeId, std::size_t> leaf_position;
    for (std::size_t index = 0; index < tree.leaves().size(); ++index) {
        leaf_position.emplace(tree.leaves()[index], index);
    }
    for (const NodeId node : nodes_by_depth(tree, true)) {
        if (tree.children()[static_cast<std::size_t>(node)].empty()) {
            node_masks_[static_cast<std::size_t>(node)] =
                std::uint64_t{1} << leaf_position.at(node);
        } else {
            for (const NodeId child : tree.children()[static_cast<std::size_t>(node)]) {
                node_masks_[static_cast<std::size_t>(node)] |=
                    node_masks_[static_cast<std::size_t>(child)];
            }
        }
    }

    label_by_mask_.emplace(0, Label::bottom());
    for (std::size_t node = 0; node < tree.size(); ++node) {
        const Label label = Label::single(static_cast<NodeId>(node));
        label_by_mask_.emplace(node_masks_[node], label);
    }
    for (std::size_t left = 0; left < tree.size(); ++left) {
        for (std::size_t right = left + 1; right < tree.size(); ++right) {
            const NodeId a = static_cast<NodeId>(left);
            const NodeId b = static_cast<NodeId>(right);
            if (tree.is_ancestor(a, b) || tree.is_ancestor(b, a)) {
                continue;
            }
            const Label label = lattice.normalize({a, b});
            const std::uint64_t mask = node_masks_[left] | node_masks_[right];
            const auto [iterator, inserted] = label_by_mask_.emplace(mask, label);
            if (!inserted && iterator->second != label) {
                throw std::logic_error("explicit state collision violates canonical normalization");
            }
        }
    }
    std::vector<std::pair<std::uint64_t, Label>> rows;
    rows.reserve(label_by_mask_.size());
    for (const auto& [mask, label] : label_by_mask_) {
        rows.emplace_back(mask, label);
    }
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
        const int left_bits = population_count(left.first);
        const int right_bits = population_count(right.first);
        return left_bits != right_bits ? left_bits < right_bits : left.first < right.first;
    });
    for (const auto& [mask, label] : rows) {
        masks_.push_back(mask);
        labels_.push_back(label);
    }
}

std::uint64_t ExplicitOracle::node_mask(NodeId node) const {
    if (node < 0 || static_cast<std::size_t>(node) >= node_masks_.size()) {
        throw std::out_of_range("explicit node mask query is out of range");
    }
    return node_masks_[static_cast<std::size_t>(node)];
}

std::uint64_t ExplicitOracle::label_mask(const Label& label) const {
    if (!lattice_->valid(label)) {
        throw std::invalid_argument("explicit mask requires a valid label");
    }
    std::uint64_t mask = 0;
    for (const NodeId node : lattice_->components(label)) {
        mask |= node_mask(node);
    }
    return mask;
}

Label ExplicitOracle::mask_label(std::uint64_t mask) const {
    const auto iterator = label_by_mask_.find(mask);
    if (iterator == label_by_mask_.end()) {
        return Label::invalid();
    }
    return iterator->second;
}

Label ExplicitOracle::brute_hull(std::uint64_t placement_mask) const {
    const std::uint64_t universe = node_mask(tree_->root());
    if ((placement_mask & ~universe) != 0) {
        return Label::invalid();
    }
    std::uint64_t best = universe;
    for (const std::uint64_t candidate : masks_) {
        if ((placement_mask & ~candidate) == 0 && (candidate & ~best) == 0) {
            best = candidate;
        }
    }
    return mask_label(best);
}

Label ExplicitOracle::brute_join(const Label& left, const Label& right) const {
    return brute_hull(label_mask(left) | label_mask(right));
}

Label ExplicitOracle::brute_meet(const Label& left, const Label& right) const {
    return mask_label(label_mask(left) & label_mask(right));
}

const std::vector<Label>& ExplicitOracle::labels() const noexcept { return labels_; }
const std::vector<std::uint64_t>& ExplicitOracle::masks() const noexcept { return masks_; }
std::size_t ExplicitOracle::storage_bytes() const noexcept {
    std::size_t bytes = node_masks_.capacity() * sizeof(node_masks_[0]);
    bytes += labels_.capacity() * sizeof(labels_[0]);
    bytes += masks_.capacity() * sizeof(masks_[0]);
    bytes += label_by_mask_.size() *
             (sizeof(std::pair<const std::uint64_t, Label>) + 2 * sizeof(void*));
    return bytes;
}

AggregatedMasses aggregate_raw_events(const LatticeOracle& lattice,
                                      const std::vector<RawEvent>& events) {
    AggregatedMasses result;
    result.raw_identity_count = static_cast<std::uint64_t>(events.size());
    for (const RawEvent& event : events) {
        if (event.domains.size() > 2) {
            throw std::invalid_argument("raw event contains more than two domain identities");
        }
        const Label region = lattice.normalize(event.domains);
        if (!lattice.valid(region)) {
            throw std::invalid_argument("raw event names an invalid domain");
        }
        result.total_mass = checked_add_mass(result.total_mass, event.mass,
                                             "total raw event mass overflow");
        Mass& destination = result.by_region[region];
        destination = checked_add_mass(destination, event.mass,
                                       "aggregated region mass overflow");
    }
    if (result.total_mass == 0) {
        throw std::invalid_argument("raw event distribution has zero total mass");
    }
    return result;
}

AggregatedMasses single_domain_envelope_masses(
    const LatticeOracle& lattice,
    const AggregatedMasses& masses
) {
    AggregatedMasses result;
    result.raw_identity_count = masses.raw_identity_count;
    for (const auto& entry : masses.by_region) {
        if (!lattice.valid(entry.first)) {
            throw std::invalid_argument("single-domain envelope received an invalid region");
        }
        Label destination = entry.first;
        if (entry.first.tag == LabelTag::pair) {
            destination = Label::single(lattice.tree().lca(
                entry.first.first, entry.first.second
            ));
            const auto contained = lattice.order(entry.first, destination);
            if (!contained.has_value() || !contained.value()) {
                throw std::logic_error("single-domain envelope does not contain its pair");
            }
        }
        Mass& value = result.by_region[destination];
        value = checked_add_mass(value, entry.second, "single-domain envelope mass overflow");
        result.total_mass = checked_add_mass(
            result.total_mass, entry.second, "single-domain envelope total overflow"
        );
    }
    if (result.total_mass != masses.total_mass) {
        throw std::logic_error("single-domain envelope changed total mass");
    }
    return result;
}

MaximumEntropyPairedResult fit_maximum_entropy_paired_events(
    std::size_t nodes,
    const std::vector<RawEvent>& observed,
    Mass quantized_total_mass,
    long double tolerance,
    std::uint64_t maximum_iterations
) {
    if (nodes == 0U || quantized_total_mass == 0U || tolerance <= 0.0L ||
        maximum_iterations == 0U) {
        throw std::invalid_argument("maximum-entropy paired fit parameters are invalid");
    }
    MaximumEntropyPairedResult result;
    result.source_occurrence_mass.assign(nodes, 0U);
    result.realized_occurrence_mass.assign(nodes, 0U);
    result.quantized_total_mass = quantized_total_mass;
    for (const RawEvent& event : observed) {
        if (event.domains.size() > 2U) {
            throw std::invalid_argument("observed event exceeds the paired support");
        }
        result.source_total_mass = checked_add_mass(
            result.source_total_mass, event.mass, "observed total mass overflow"
        );
        std::vector<NodeId> unique = event.domains;
        std::sort(unique.begin(), unique.end());
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        for (const NodeId node : unique) {
            if (node < 0 || static_cast<std::size_t>(node) >= nodes) {
                throw std::invalid_argument("observed marginal identity is out of range");
            }
            Mass& occurrence = result.source_occurrence_mass[static_cast<std::size_t>(node)];
            occurrence = checked_add_mass(
                occurrence, event.mass, "observed marginal mass overflow"
            );
        }
    }
    if (result.source_total_mass == 0U) {
        throw std::invalid_argument("observed marginal distribution has zero mass");
    }
    std::vector<std::size_t> active;
    Score occurrence_sum = 0;
    for (std::size_t node = 0; node < nodes; ++node) {
        if (result.source_occurrence_mass[node] > result.source_total_mass) {
            throw std::logic_error("observed marginal exceeds total mass");
        }
        occurrence_sum += result.source_occurrence_mass[node];
        if (result.source_occurrence_mass[node] != 0U) {
            active.push_back(node);
        }
    }
    if (occurrence_sum > Score{2U} * result.source_total_mass) {
        throw std::invalid_argument("observed marginals are infeasible on paired support");
    }

    struct Candidate {
        std::vector<NodeId> domains;
        long double probability = 0.0L;
        long double remainder = 0.0L;
        std::uint64_t remainder_key = 0U;
        Mass mass = 0U;
    };
    std::vector<Candidate> candidates;
    const auto add_full_support = [&]() {
        candidates.push_back({{}, 0.0L, 0.0L, 0U, 0U});
        for (const std::size_t node : active) {
            candidates.push_back({{static_cast<NodeId>(node)}, 0.0L, 0.0L, 0U, 0U});
        }
        for (std::size_t left = 0; left < active.size(); ++left) {
            for (std::size_t right = left + 1U; right < active.size(); ++right) {
                candidates.push_back({{
                    static_cast<NodeId>(active[left]), static_cast<NodeId>(active[right])
                }, 0.0L, 0.0L, 0U, 0U});
            }
        }
    };
    add_full_support();
    std::unordered_map<NodeId, std::size_t> singleton_index;
    std::map<std::pair<NodeId, NodeId>, std::size_t> pair_index;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (candidates[index].domains.size() == 1U) {
            singleton_index[candidates[index].domains[0]] = index;
        } else if (candidates[index].domains.size() == 2U) {
            pair_index[{candidates[index].domains[0], candidates[index].domains[1]}] = index;
        }
    }

    std::vector<long double> probability(active.size(), 0.0L);
    std::vector<std::size_t> certain;
    for (std::size_t index = 0; index < active.size(); ++index) {
        const Mass occurrence = result.source_occurrence_mass[active[index]];
        probability[index] = static_cast<long double>(occurrence) /
                             result.source_total_mass;
        if (occurrence == result.source_total_mass) {
            certain.push_back(index);
        }
    }
    if (active.empty()) {
        candidates.front().probability = 1.0L;
    } else if (active.size() == 1U) {
        candidates.front().probability = 1.0L - probability.front();
        candidates[singleton_index.at(static_cast<NodeId>(active.front()))].probability =
            probability.front();
    } else if (!certain.empty()) {
        if (certain.size() > 2U) {
            throw std::invalid_argument("more than two identities have unit marginal");
        }
        if (certain.size() == 2U) {
            if (active.size() != 2U) {
                throw std::invalid_argument("two unit marginals leave no mass for another identity");
            }
            NodeId left = static_cast<NodeId>(active[certain[0]]);
            NodeId right = static_cast<NodeId>(active[certain[1]]);
            if (right < left) std::swap(left, right);
            candidates[pair_index.at({left, right})].probability = 1.0L;
        } else {
            const std::size_t fixed_index = certain.front();
            const NodeId fixed = static_cast<NodeId>(active[fixed_index]);
            long double other_sum = 0.0L;
            for (std::size_t index = 0; index < active.size(); ++index) {
                if (index == fixed_index) continue;
                other_sum += probability[index];
                NodeId other = static_cast<NodeId>(active[index]);
                NodeId left = std::min(fixed, other);
                NodeId right = std::max(fixed, other);
                candidates[pair_index.at({left, right})].probability = probability[index];
            }
            if (other_sum > 1.0L + tolerance) {
                throw std::invalid_argument("unit marginal boundary is infeasible");
            }
            candidates[singleton_index.at(fixed)].probability =
                std::max(0.0L, 1.0L - other_sum);
        }
    } else {
        const bool pair_only = occurrence_sum == Score{2U} * result.source_total_mass;
        std::vector<long double> weight = probability;
        if (!pair_only) {
            for (std::size_t index = 0; index < weight.size(); ++index) {
                weight[index] = probability[index] / (1.0L - probability[index]);
            }
        }
        bool converged = false;
        for (std::uint64_t iteration = 1U; iteration <= maximum_iterations; ++iteration) {
            const long double sum = std::accumulate(weight.begin(), weight.end(), 0.0L);
            const long double square_sum = std::inner_product(
                weight.begin(), weight.end(), weight.begin(), 0.0L
            );
            const long double pair_normalizer = (sum * sum - square_sum) / 2.0L;
            const long double normalizer = pair_only ? pair_normalizer
                                                      : 1.0L + sum + pair_normalizer;
            if (!std::isfinite(normalizer) || normalizer <= 0.0L) {
                throw std::overflow_error("maximum-entropy normalizer is invalid");
            }
            result.maximum_fit_error = 0.0L;
            std::vector<long double> fitted(weight.size(), 0.0L);
            for (std::size_t index = 0; index < weight.size(); ++index) {
                const long double factor = pair_only ? sum - weight[index]
                                                      : 1.0L + sum - weight[index];
                fitted[index] = weight[index] * factor / normalizer;
                result.maximum_fit_error = std::max(
                    result.maximum_fit_error,
                    std::fabs(fitted[index] - probability[index])
                );
            }
            result.iterations = iteration;
            if (result.maximum_fit_error <= tolerance) {
                converged = true;
                break;
            }
            for (std::size_t index = 0; index < weight.size(); ++index) {
                if (fitted[index] <= 0.0L) {
                    throw std::logic_error("maximum-entropy fitted marginal became zero");
                }
                weight[index] *= std::sqrt(probability[index] / fitted[index]);
            }
            if (pair_only) {
                const long double maximum = *std::max_element(weight.begin(), weight.end());
                for (long double& value : weight) value /= maximum;
            }
        }
        if (!converged) {
            throw std::runtime_error("maximum-entropy paired fit did not converge");
        }
        const long double sum = std::accumulate(weight.begin(), weight.end(), 0.0L);
        const long double square_sum = std::inner_product(
            weight.begin(), weight.end(), weight.begin(), 0.0L
        );
        const long double pair_normalizer = (sum * sum - square_sum) / 2.0L;
        const long double normalizer = pair_only ? pair_normalizer
                                                  : 1.0L + sum + pair_normalizer;
        if (!pair_only) {
            candidates.front().probability = 1.0L / normalizer;
            for (std::size_t index = 0; index < active.size(); ++index) {
                candidates[singleton_index.at(static_cast<NodeId>(active[index]))].probability =
                    weight[index] / normalizer;
            }
        }
        for (std::size_t left = 0; left < active.size(); ++left) {
            for (std::size_t right = left + 1U; right < active.size(); ++right) {
                candidates[pair_index.at({
                    static_cast<NodeId>(active[left]), static_cast<NodeId>(active[right])
                })].probability = weight[left] * weight[right] / normalizer;
            }
        }
    }

    long double probability_sum = 0.0L;
    for (const Candidate& candidate : candidates) probability_sum += candidate.probability;
    if (!std::isfinite(probability_sum) || probability_sum <= 0.0L) {
        throw std::logic_error("maximum-entropy probabilities have invalid total");
    }
    Mass assigned = 0U;
    const long double remainder_scale = std::ldexp(1.0L, 60);
    for (Candidate& candidate : candidates) {
        candidate.probability /= probability_sum;
    }
    // The certificate must describe the probability vector that is actually
    // quantized.  Recompute its marginals after the final normalization rather
    // than retaining the stopping residual of the unnormalized GIS iterate.
    result.maximum_fit_error = 0.0L;
    for (std::size_t index = 0; index < active.size(); ++index) {
        const NodeId node = static_cast<NodeId>(active[index]);
        long double fitted = 0.0L;
        for (const Candidate& candidate : candidates) {
            if (std::find(candidate.domains.begin(), candidate.domains.end(), node) !=
                candidate.domains.end()) {
                fitted += candidate.probability;
            }
        }
        result.maximum_fit_error = std::max(
            result.maximum_fit_error, std::fabs(fitted - probability[index])
        );
    }
    for (Candidate& candidate : candidates) {
        const long double exact = static_cast<long double>(quantized_total_mass) *
                                  candidate.probability;
        candidate.mass = static_cast<Mass>(std::floor(exact));
        candidate.remainder = exact - static_cast<long double>(candidate.mass);
        candidate.remainder_key = static_cast<std::uint64_t>(std::floor(
            candidate.remainder * remainder_scale + 0.5L
        ));
        assigned = checked_add_mass(assigned, candidate.mass, "quantized mass overflow");
    }
    if (assigned > quantized_total_mass) {
        throw std::logic_error("Hamilton floors exceed the quantized total");
    }
    std::vector<std::size_t> remainder_order(candidates.size());
    std::iota(remainder_order.begin(), remainder_order.end(), std::size_t{0});
    std::sort(remainder_order.begin(), remainder_order.end(), [&](std::size_t left,
                                                                  std::size_t right) {
        if (candidates[left].remainder_key != candidates[right].remainder_key) {
            return candidates[left].remainder_key > candidates[right].remainder_key;
        }
        return left < right;
    });
    const Mass remaining = quantized_total_mass - assigned;
    if (remaining > remainder_order.size()) {
        throw std::logic_error("Hamilton remainder count exceeds the support size");
    }
    for (Mass index = 0; index < remaining; ++index) {
        ++candidates[remainder_order[static_cast<std::size_t>(index)]].mass;
    }
    result.events.reserve(candidates.size());
    Mass quantized_check = 0U;
    for (const Candidate& candidate : candidates) {
        result.events.push_back({candidate.domains, candidate.mass});
        quantized_check = checked_add_mass(
            quantized_check, candidate.mass, "Hamilton total overflow"
        );
        for (const NodeId node : candidate.domains) {
            Mass& occurrence = result.realized_occurrence_mass[static_cast<std::size_t>(node)];
            occurrence = checked_add_mass(
                occurrence, candidate.mass, "Hamilton marginal overflow"
            );
        }
    }
    if (quantized_check != quantized_total_mass) {
        throw std::logic_error("Hamilton rounding did not preserve total mass");
    }
    result.maximum_quantized_marginal_error = 0.0L;
    for (const std::size_t node : active) {
        const long double source = static_cast<long double>(result.source_occurrence_mass[node]) /
                                   result.source_total_mass;
        const long double realized = static_cast<long double>(result.realized_occurrence_mass[node]) /
                                     quantized_total_mass;
        result.maximum_quantized_marginal_error = std::max(
            result.maximum_quantized_marginal_error, std::fabs(realized - source)
        );
    }
    result.certified_marginal_error_bound = result.maximum_fit_error +
        static_cast<long double>(active.size()) / quantized_total_mass;
    if (result.maximum_quantized_marginal_error >
        result.certified_marginal_error_bound + 4.0L * std::numeric_limits<long double>::epsilon()) {
        throw std::logic_error("Hamilton marginal error exceeds its certified bound");
    }
    return result;
}

std::uint64_t four_cycle_path_hash(const FourCycleCorrelationPath& path) noexcept {
    std::uint64_t hash = kFnvOffset;
    const auto add_score = [&](Score value, std::uint64_t current) {
        current = fnv1a_update(current, static_cast<std::uint64_t>(value));
        return fnv1a_update(current, static_cast<std::uint64_t>(value >> 64U));
    };
    hash = fnv1a_update(hash, path.rewiring_stream);
    hash = fnv1a_update(hash, path.maximum_depth);
    hash = fnv1a_update(hash, path.total_mass);
    hash = fnv1a_update(hash, path.baseline_pair_mass);
    hash = fnv1a_update(hash, path.positive_switches);
    hash = fnv1a_update(hash, path.zero_capacity_switches);
    hash = fnv1a_update(hash, path.degree_hash);
    hash = fnv1a_update(hash, path.topology_hash);
    hash = fnv1a_update(hash, path.leaf_hash);
    hash = fnv1a_update(hash, path.semantic_pair_edge_count);
    hash = fnv1a_update(hash, path.semantic_collapsed_edge_count);
    hash = add_score(path.total_switch_mass, hash);
    hash = add_score(path.endpoint_pair_l1_divergence, hash);
    hash = add_score(path.baseline_locality_depth_mass, hash);
    hash = add_score(path.endpoint_locality_depth_mass, hash);
    for (const NodeId parent : path.topology_parent) {
        hash = fnv1a_update(hash, static_cast<std::uint32_t>(parent));
    }
    for (const NodeId leaf : path.topology_leaves) {
        hash = fnv1a_update(hash, static_cast<std::uint32_t>(leaf));
    }
    for (std::size_t index = 0; index < path.baseline_events.size(); ++index) {
        const RawEvent& base = path.baseline_events[index];
        hash = fnv1a_update(hash, base.domains.size());
        for (const NodeId node : base.domains) {
            hash = fnv1a_update(hash, static_cast<std::uint32_t>(node));
        }
        hash = fnv1a_update(hash, base.mass);
        if (index < path.endpoint_events.size()) {
            hash = fnv1a_update(hash, path.endpoint_events[index].mass);
        }
    }
    for (const FourCycleDecompositionAudit& decomposition : path.decompositions) {
        hash = fnv1a_update(hash, static_cast<std::uint32_t>(decomposition.pivot));
        hash = fnv1a_update(hash, static_cast<std::uint32_t>(decomposition.left_child));
        hash = fnv1a_update(hash, static_cast<std::uint32_t>(decomposition.right_child));
        hash = fnv1a_update(hash, static_cast<std::uint32_t>(decomposition.short_child));
        hash = fnv1a_update(hash, static_cast<std::uint32_t>(decomposition.long_child));
        hash = fnv1a_update(hash, decomposition.padded_size);
        hash = fnv1a_update(hash, decomposition.real_edges_per_shift);
        hash = fnv1a_update(hash, decomposition.candidates_per_shift);
        hash = fnv1a_update(hash, decomposition.odd_edges_per_shift);
        for (const NodeId node : decomposition.permuted_short) {
            hash = fnv1a_update(hash, static_cast<std::uint32_t>(node));
        }
        for (const NodeId node : decomposition.permuted_long) {
            hash = fnv1a_update(hash, static_cast<std::uint32_t>(node));
        }
    }
    for (const FourCycleSwitchAudit& change : path.switches) {
        hash = fnv1a_update(hash, change.order);
        hash = fnv1a_update(hash, change.decomposition_index);
        hash = fnv1a_update(hash, change.cyclic_shift);
        hash = fnv1a_update(hash, change.matching_position);
        hash = fnv1a_update(hash, change.stream_tie_key);
        for (const NodeId node : change.quartet) {
            hash = fnv1a_update(hash, static_cast<std::uint32_t>(node));
        }
        hash = fnv1a_update(hash, change.source_locality_depth_sum);
        hash = fnv1a_update(hash, change.target_locality_depth_sum);
        hash = fnv1a_update(hash, change.pre_source_first);
        hash = fnv1a_update(hash, change.pre_source_second);
        hash = fnv1a_update(hash, change.moved_mass);
    }
    for (const FourCycleEdgeDifference& difference : path.edge_differences) {
        hash = fnv1a_update(hash, static_cast<std::uint32_t>(difference.left));
        hash = fnv1a_update(hash, static_cast<std::uint32_t>(difference.right));
        hash = fnv1a_update(hash, difference.baseline_mass);
        hash = fnv1a_update(hash, difference.endpoint_mass);
        hash = fnv1a_update(hash, difference.absolute_difference);
        hash = fnv1a_update(hash, difference.increased ? 1U : 0U);
    }
    for (const Mass mass : path.baseline_occurrence_mass) hash = fnv1a_update(hash, mass);
    for (const Mass mass : path.endpoint_occurrence_mass) hash = fnv1a_update(hash, mass);
    return hash;
}

FourCycleCorrelationPath make_four_cycle_correlation_path(
    const LatticeOracle& lattice,
    const std::vector<RawEvent>& baseline,
    std::uint64_t rewiring_stream
) {
    const std::size_t nodes = lattice.tree().size();
    FourCycleCorrelationPath result;
    result.rewiring_stream = rewiring_stream;
    result.topology_parent = lattice.tree().parent();
    result.topology_leaves = lattice.tree().leaves();
    result.topology_hash = kFnvOffset;
    for (const NodeId parent : result.topology_parent) {
        result.topology_hash = fnv1a_update(
            result.topology_hash, static_cast<std::uint32_t>(parent)
        );
    }
    result.leaf_hash = kFnvOffset;
    for (const NodeId leaf : result.topology_leaves) {
        result.leaf_hash = fnv1a_update(
            result.leaf_hash, static_cast<std::uint32_t>(leaf)
        );
    }
    std::map<std::vector<NodeId>, Mass> canonical;
    for (const RawEvent& raw : baseline) {
        if (raw.domains.size() > 2U) {
            throw std::invalid_argument("four-cycle baseline exceeds paired support");
        }
        std::vector<NodeId> domains = raw.domains;
        std::sort(domains.begin(), domains.end());
        domains.erase(std::unique(domains.begin(), domains.end()), domains.end());
        for (const NodeId node : domains) {
            if (node < 0 || static_cast<std::size_t>(node) >= nodes) {
                throw std::invalid_argument("four-cycle baseline identity is out of range");
            }
            if (!lattice.tree().is_leaf(node)) {
                throw std::invalid_argument(
                    "four-cycle controlled baseline contains a nonleaf identity"
                );
            }
        }
        Mass& mass = canonical[domains];
        mass = checked_add_mass(
            mass, raw.mass, "four-cycle duplicate event mass overflow"
        );
    }
    std::vector<NodeId> leaves = lattice.tree().leaves();
    std::sort(leaves.begin(), leaves.end());
    result.baseline_events.push_back({{}, canonical[{}]});
    for (const NodeId leaf : leaves) {
        const std::vector<NodeId> singleton{leaf};
        result.baseline_events.push_back({singleton, canonical[singleton]});
    }
    for (std::size_t left = 0; left < leaves.size(); ++left) {
        for (std::size_t right = left + 1U; right < leaves.size(); ++right) {
            const std::vector<NodeId> pair{leaves[left], leaves[right]};
            result.baseline_events.push_back({pair, canonical[pair]});
        }
    }
    for (const RawEvent& event : result.baseline_events) {
        result.total_mass = checked_add_mass(
            result.total_mass, event.mass, "four-cycle total mass overflow"
        );
        if (event.domains.size() == 2U) {
            result.baseline_pair_mass = checked_add_mass(
                result.baseline_pair_mass, event.mass,
                "four-cycle pair mass overflow"
            );
        }
    }
    const auto ordered_pair = [](NodeId left, NodeId right) {
        if (right < left) std::swap(left, right);
        return std::vector<NodeId>{left, right};
    };
    const ReducedTree& tree = lattice.tree();
    result.maximum_depth = *std::max_element(tree.depth().begin(), tree.depth().end());
    if (result.maximum_depth == 0U || tree.leaves().size() < 4U) {
        throw std::invalid_argument("four-cycle path requires at least four nontrivial leaves");
    }

    std::vector<std::vector<NodeId>> descendant_leaves(nodes);
    for (const NodeId node : nodes_by_depth(tree, true)) {
        const std::size_t index = static_cast<std::size_t>(node);
        if (tree.is_leaf(node)) {
            descendant_leaves[index].push_back(node);
        } else {
            for (const NodeId child : tree.children()[index]) {
                const auto& child_leaves = descendant_leaves[static_cast<std::size_t>(child)];
                descendant_leaves[index].insert(
                    descendant_leaves[index].end(), child_leaves.begin(), child_leaves.end()
                );
            }
            std::sort(descendant_leaves[index].begin(), descendant_leaves[index].end());
        }
    }
    DeterministicRng generator(rewiring_stream);
    const auto permute = [&](std::vector<NodeId>& values) {
        for (std::size_t remaining = values.size(); remaining > 1U; --remaining) {
            const std::size_t selected = static_cast<std::size_t>(
                generator.bounded(remaining)
            );
            std::swap(values[remaining - 1U], values[selected]);
        }
    };
    struct Candidate {
        std::size_t decomposition_index;
        std::size_t cyclic_shift;
        std::size_t matching_position;
        std::uint64_t stream_tie_key;
        std::array<NodeId, 4> quartet;
    };
    std::vector<Candidate> candidates;
    for (const NodeId pivot : nodes_by_depth(tree, false)) {
        const auto& children = tree.children()[static_cast<std::size_t>(pivot)];
        for (std::size_t left_index = 0; left_index < children.size(); ++left_index) {
            for (std::size_t right_index = left_index + 1U;
                 right_index < children.size(); ++right_index) {
                FourCycleDecompositionAudit decomposition;
                decomposition.pivot = pivot;
                decomposition.left_child = children[left_index];
                decomposition.right_child = children[right_index];
                const auto child_key = [&](NodeId child) {
                    return std::make_pair(
                        descendant_leaves[static_cast<std::size_t>(child)].size(), child
                    );
                };
                decomposition.short_child = child_key(decomposition.left_child) <=
                        child_key(decomposition.right_child)
                    ? decomposition.left_child : decomposition.right_child;
                decomposition.long_child = decomposition.short_child == decomposition.left_child
                    ? decomposition.right_child : decomposition.left_child;
                decomposition.permuted_short = descendant_leaves[
                    static_cast<std::size_t>(decomposition.short_child)
                ];
                decomposition.permuted_long = descendant_leaves[
                    static_cast<std::size_t>(decomposition.long_child)
                ];
                permute(decomposition.permuted_short);
                permute(decomposition.permuted_long);
                decomposition.real_edges_per_shift = decomposition.permuted_short.size();
                decomposition.candidates_per_shift =
                    decomposition.real_edges_per_shift / 2U;
                decomposition.odd_edges_per_shift =
                    decomposition.real_edges_per_shift % 2U;
                decomposition.padded_size = decomposition.permuted_long.size();
                decomposition.permuted_short.resize(
                    decomposition.padded_size, NodeId{-1}
                );
                const std::size_t decomposition_index = result.decompositions.size();
                result.decompositions.push_back(decomposition);
                const std::size_t size = decomposition.padded_size;
                for (std::size_t shift = 0; shift < size; ++shift) {
                    std::vector<std::pair<NodeId, NodeId>> matching;
                    matching.reserve(decomposition.real_edges_per_shift);
                    for (std::size_t position = 0;
                         position < decomposition.real_edges_per_shift; ++position) {
                        matching.emplace_back(
                            decomposition.permuted_short[position],
                            decomposition.permuted_long[(position + shift) % size]
                        );
                    }
                    for (std::size_t position = 0; position + 1U < matching.size();
                         position += 2U) {
                        const auto& first = matching[position];
                        const auto& second = matching[position + 1U];
                        candidates.push_back({
                            decomposition_index,
                            shift,
                            position,
                            generator.next(),
                            {{first.first, second.first, first.second, second.second}},
                        });
                    }
                }
            }
        }
    }
    std::sort(candidates.begin(), candidates.end(), [&](const Candidate& left,
                                                         const Candidate& right) {
        const auto& left_decomposition = result.decompositions[left.decomposition_index];
        const auto& right_decomposition = result.decompositions[right.decomposition_index];
        return std::tie(
            tree.depth()[static_cast<std::size_t>(left_decomposition.pivot)],
            left_decomposition.pivot,
            left_decomposition.left_child,
            left_decomposition.right_child,
            left.cyclic_shift,
            left.matching_position,
            left.stream_tie_key,
            left.quartet
        ) < std::tie(
            tree.depth()[static_cast<std::size_t>(right_decomposition.pivot)],
            right_decomposition.pivot,
            right_decomposition.left_child,
            right_decomposition.right_child,
            right.cyclic_shift,
            right.matching_position,
            right.stream_tie_key,
            right.quartet
        );
    });

    result.endpoint_events = result.baseline_events;
    std::map<std::vector<NodeId>, std::size_t> endpoint_index;
    for (std::size_t index = 0; index < result.endpoint_events.size(); ++index) {
        endpoint_index.emplace(result.endpoint_events[index].domains, index);
    }
    const auto ensure_pair_event = [&](const std::vector<NodeId>& pair) {
        const auto iterator = endpoint_index.find(pair);
        if (iterator != endpoint_index.end()) return iterator->second;
        const std::size_t index = result.endpoint_events.size();
        result.baseline_events.push_back({pair, 0U});
        result.endpoint_events.push_back({pair, 0U});
        endpoint_index.emplace(pair, index);
        return index;
    };
    result.switches.reserve(candidates.size());
    for (std::size_t order_index = 0; order_index < candidates.size(); ++order_index) {
        const Candidate& candidate = candidates[order_index];
        const auto& quartet = candidate.quartet;
        std::set<NodeId> distinct(quartet.begin(), quartet.end());
        if (distinct.size() != 4U) {
            throw std::logic_error("four-cycle construction repeated an identity");
        }
        const std::vector<NodeId> target_first = ordered_pair(quartet[0], quartet[1]);
        const std::vector<NodeId> target_second = ordered_pair(quartet[2], quartet[3]);
        const std::vector<NodeId> source_first = ordered_pair(quartet[0], quartet[2]);
        const std::vector<NodeId> source_second = ordered_pair(quartet[1], quartet[3]);
        const std::uint64_t source_depth =
            tree.depth()[static_cast<std::size_t>(tree.lca(source_first[0], source_first[1]))] +
            tree.depth()[static_cast<std::size_t>(tree.lca(source_second[0], source_second[1]))];
        const std::uint64_t target_depth =
            tree.depth()[static_cast<std::size_t>(tree.lca(target_first[0], target_first[1]))] +
            tree.depth()[static_cast<std::size_t>(tree.lca(target_second[0], target_second[1]))];
        if (target_depth <= source_depth) {
            throw std::logic_error("four-cycle candidate is not strictly locality increasing");
        }
        const std::size_t target_first_index = ensure_pair_event(target_first);
        const std::size_t target_second_index = ensure_pair_event(target_second);
        const std::size_t source_first_index = ensure_pair_event(source_first);
        const std::size_t source_second_index = ensure_pair_event(source_second);
        const Mass pre_first = result.endpoint_events[source_first_index].mass;
        const Mass pre_second = result.endpoint_events[source_second_index].mass;
        const Mass moved = 4U * (std::min(pre_first, pre_second) / 4U);
        result.switches.push_back({
            static_cast<std::uint64_t>(order_index),
            candidate.decomposition_index,
            candidate.cyclic_shift,
            candidate.matching_position,
            candidate.stream_tie_key,
            quartet,
            source_depth,
            target_depth,
            pre_first,
            pre_second,
            moved,
        });
        if (moved == 0U) {
            ++result.zero_capacity_switches;
            continue;
        }
        ++result.positive_switches;
        result.total_switch_mass += moved;
        result.endpoint_events[source_first_index].mass -= moved;
        result.endpoint_events[source_second_index].mass -= moved;
        result.endpoint_events[target_first_index].mass = checked_add_mass(
            result.endpoint_events[target_first_index].mass, moved,
            "four-cycle target mass overflow"
        );
        result.endpoint_events[target_second_index].mass = checked_add_mass(
            result.endpoint_events[target_second_index].mass, moved,
            "four-cycle target mass overflow"
        );
    }
    for (std::size_t index = 0; index < result.baseline_events.size(); ++index) {
        const RawEvent& base = result.baseline_events[index];
        const RawEvent& endpoint = result.endpoint_events[index];
        if (base.domains != endpoint.domains) {
            throw std::logic_error("four-cycle endpoint support is misaligned");
        }
        if (base.domains.size() != 2U) {
            if (base.mass != endpoint.mass) {
                throw std::logic_error("four-cycle modified a nonpair category");
            }
            continue;
        }
        const bool increased = endpoint.mass >= base.mass;
        const Mass difference = increased ? endpoint.mass - base.mass
                                          : base.mass - endpoint.mass;
        if (difference % 4U != 0U) {
            throw std::logic_error("four-cycle endpoint difference is not divisible by four");
        }
        if (difference != 0U) {
            result.edge_differences.push_back({
                base.domains[0], base.domains[1], base.mass, endpoint.mass,
                difference, increased,
            });
            const Label semantic = lattice.normalize(base.domains);
            if (semantic.tag == LabelTag::pair) {
                ++result.semantic_pair_edge_count;
            } else {
                ++result.semantic_collapsed_edge_count;
            }
        }
    }

    const auto summarize = [&](const std::vector<RawEvent>& events,
                               std::vector<Mass>& occurrence,
                               Mass& total,
                               Mass& pair_total,
                               Score& locality_depth_mass) {
        occurrence.assign(nodes, 0U);
        total = 0U;
        pair_total = 0U;
        locality_depth_mass = 0U;
        for (const RawEvent& event : events) {
            total = checked_add_mass(total, event.mass, "four-cycle verification overflow");
            if (event.domains.size() == 2U) {
                pair_total = checked_add_mass(
                    pair_total, event.mass, "four-cycle pair verification overflow"
                );
                locality_depth_mass += Score{event.mass} * tree.depth()[
                    static_cast<std::size_t>(tree.lca(event.domains[0], event.domains[1]))
                ];
            }
            for (const NodeId node : event.domains) {
                Mass& value = occurrence[static_cast<std::size_t>(node)];
                value = checked_add_mass(
                    value, event.mass, "four-cycle marginal verification overflow"
                );
            }
        }
    };
    Mass baseline_total_check = 0U;
    Mass baseline_pair_check = 0U;
    summarize(
        result.baseline_events, result.baseline_occurrence_mass,
        baseline_total_check, baseline_pair_check, result.baseline_locality_depth_mass
    );
    Mass endpoint_total = 0U;
    Mass endpoint_pair = 0U;
    summarize(
        result.endpoint_events, result.endpoint_occurrence_mass, endpoint_total, endpoint_pair,
        result.endpoint_locality_depth_mass
    );
    if (result.total_mass == 0U || baseline_total_check != result.total_mass ||
        endpoint_total != result.total_mass || endpoint_pair != result.baseline_pair_mass ||
        baseline_pair_check != result.baseline_pair_mass ||
        result.endpoint_occurrence_mass != result.baseline_occurrence_mass) {
        throw std::logic_error("four-cycle rewiring violated an exact invariant");
    }
    result.endpoint_pair_l1_divergence = 0U;
    for (const auto& difference : result.edge_differences) {
        result.endpoint_pair_l1_divergence += difference.absolute_difference;
    }
    result.degree_hash = kFnvOffset;
    for (std::size_t node = 0; node < result.baseline_occurrence_mass.size(); ++node) {
        result.degree_hash = fnv1a_update(result.degree_hash, node);
        result.degree_hash = fnv1a_update(
            result.degree_hash, result.baseline_occurrence_mass[node]
        );
    }
    Score replayed_locality_increase = 0U;
    for (const auto& change : result.switches) {
        replayed_locality_increase += Score{change.moved_mass} *
            (change.target_locality_depth_sum - change.source_locality_depth_sum);
    }
    if (result.endpoint_locality_depth_mass < result.baseline_locality_depth_mass ||
        result.endpoint_locality_depth_mass - result.baseline_locality_depth_mass !=
        replayed_locality_increase) {
        throw std::logic_error("four-cycle locality audit does not replay the endpoint");
    }
    if (result.positive_switches > 0U &&
        result.endpoint_locality_depth_mass <= result.baseline_locality_depth_mass) {
        throw std::logic_error("four-cycle path did not increase locality");
    }
    result.path_hash = four_cycle_path_hash(result);
    return result;
}

FourCycleCorrelationLevel materialize_four_cycle_correlation_level(
    const LatticeOracle& lattice,
    const FourCycleCorrelationPath& path,
    std::uint64_t rho_quarter
) {
    if (rho_quarter > 4U) {
        throw std::invalid_argument("four-cycle rho quarter must be in [0,4]");
    }
    if (path.topology_parent != lattice.tree().parent() ||
        path.topology_leaves != lattice.tree().leaves()) {
        throw std::invalid_argument("four-cycle path belongs to a different hierarchy");
    }
    if (four_cycle_path_hash(path) != path.path_hash) {
        throw std::invalid_argument("four-cycle path audit hash does not match its contents");
    }
    if (path.baseline_events.size() != path.endpoint_events.size()) {
        throw std::invalid_argument("four-cycle path support arrays differ");
    }
    FourCycleCorrelationLevel result;
    result.rho_quarter = rho_quarter;
    result.degree_hash = path.degree_hash;
    result.path_hash = path.path_hash;
    result.occurrence_mass.assign(lattice.tree().size(), 0U);
    result.events.reserve(path.baseline_events.size());
    for (std::size_t index = 0; index < path.baseline_events.size(); ++index) {
        const RawEvent& base = path.baseline_events[index];
        const RawEvent& endpoint = path.endpoint_events[index];
        if (base.domains != endpoint.domains) {
            throw std::invalid_argument("four-cycle path support is not aligned");
        }
        const Score numerator = Score{4U - rho_quarter} * base.mass +
                                Score{rho_quarter} * endpoint.mass;
        if (numerator % 4U != 0U) {
            throw std::logic_error("four-cycle quarter interpolation is not integral");
        }
        const Mass mass = checked_u64(numerator / 4U, "four-cycle level mass overflow");
        result.events.push_back({base.domains, mass});
        result.total_mass = checked_add_mass(
            result.total_mass, mass, "four-cycle level total overflow"
        );
        if (base.domains.size() != 2U && mass != base.mass) {
            throw std::logic_error("four-cycle level modified a nonpair category");
        }
        if (base.domains.size() == 2U) {
            result.pair_mass = checked_add_mass(
                result.pair_mass, mass, "four-cycle level pair total overflow"
            );
            result.locality_depth_mass += Score{mass} * lattice.tree().depth()[
                static_cast<std::size_t>(
                    lattice.tree().lca(base.domains[0], base.domains[1])
                )
            ];
            result.pair_l1_divergence += mass >= base.mass ? mass - base.mass
                                                           : base.mass - mass;
        }
        for (const NodeId node : base.domains) {
            if (node < 0 || static_cast<std::size_t>(node) >= result.occurrence_mass.size()) {
                throw std::invalid_argument("four-cycle level identity is out of range");
            }
            Mass& occurrence = result.occurrence_mass[static_cast<std::size_t>(node)];
            occurrence = checked_add_mass(
                occurrence, mass, "four-cycle level marginal overflow"
            );
        }
    }
    const Score locality_numerator =
        Score{4U - rho_quarter} * path.baseline_locality_depth_mass +
        Score{rho_quarter} * path.endpoint_locality_depth_mass;
    if (locality_numerator % 4U != 0U) {
        throw std::logic_error("four-cycle locality interpolation is not integral");
    }
    const Score expected_locality = locality_numerator / 4U;
    const Score expected_l1 = path.endpoint_pair_l1_divergence * rho_quarter / 4U;
    if (result.total_mass != path.total_mass || result.pair_mass != path.baseline_pair_mass ||
        result.occurrence_mass != path.baseline_occurrence_mass ||
        result.locality_depth_mass != expected_locality ||
        result.pair_l1_divergence != expected_l1) {
        throw std::logic_error("four-cycle level violated its exact path invariant");
    }
    std::uint64_t degree_hash = kFnvOffset;
    for (std::size_t node = 0; node < result.occurrence_mass.size(); ++node) {
        degree_hash = fnv1a_update(degree_hash, node);
        degree_hash = fnv1a_update(degree_hash, result.occurrence_mass[node]);
    }
    if (degree_hash != path.degree_hash) {
        throw std::logic_error("four-cycle level degree hash changed");
    }
    return result;
}

DenseLossMassCompiler::DenseLossMassCompiler(const LatticeOracle& lattice,
                                             const AggregatedMasses& masses)
    : lattice_(&lattice), n_(lattice.tree().size()), total_mass_(masses.total_mass) {
    if (n_ != 0 && n_ > std::numeric_limits<std::size_t>::max() / n_) {
        throw std::overflow_error("dense loss table dimension overflow");
    }
    const std::size_t cells = n_ * n_;
    chain_mass_.assign(n_, 0);
    cross_mass_.assign(cells, 0);
    std::vector<Mass> alpha(n_, 0);
    for (const auto& [label, mass] : masses.by_region) {
        if (!lattice.valid(label)) {
            throw std::invalid_argument("aggregated map contains an invalid label");
        }
        if (label.tag == LabelTag::single) {
            alpha[static_cast<std::size_t>(label.first)] = checked_add_mass(
                alpha[static_cast<std::size_t>(label.first)], mass,
                "single event mass overflow"
            );
        } else if (label.tag == LabelTag::pair) {
            const std::size_t left = static_cast<std::size_t>(label.first);
            const std::size_t right = static_cast<std::size_t>(label.second);
            cross_mass_[left * n_ + right] = checked_add_mass(
                cross_mass_[left * n_ + right], mass, "pair event mass overflow"
            );
            cross_mass_[right * n_ + left] = checked_add_mass(
                cross_mass_[right * n_ + left], mass, "pair event mass overflow"
            );
        }
    }

    std::vector<Mass> row = alpha;
    for (std::size_t left = 0; left < n_; ++left) {
        for (std::size_t right = 0; right < n_; ++right) {
            row[left] = checked_add_mass(row[left], cross_mass_[left * n_ + right],
                                         "row event mass overflow");
        }
    }
    const std::vector<NodeId> order = nodes_by_depth(lattice.tree(), false);
    for (const NodeId node : order) {
        Mass value = row[static_cast<std::size_t>(node)];
        const NodeId parent = lattice.tree().parent()[static_cast<std::size_t>(node)];
        if (parent >= 0) {
            value = checked_add_mass(value, chain_mass_[static_cast<std::size_t>(parent)],
                                     "chain event mass overflow");
        }
        if (value > total_mass_) {
            throw std::logic_error("compiled chain mass exceeds total mass");
        }
        chain_mass_[static_cast<std::size_t>(node)] = value;
    }

    const auto previous = [&](NodeId left, NodeId right) -> Mass {
        if (left < 0 || right < 0 || lattice.tree().is_ancestor(left, right) ||
            lattice.tree().is_ancestor(right, left)) {
            return 0;
        }
        return cross_mass_[static_cast<std::size_t>(left) * n_ +
                           static_cast<std::size_t>(right)];
    };
    for (const NodeId left : order) {
        for (const NodeId right : order) {
            if (lattice.tree().is_ancestor(left, right) ||
                lattice.tree().is_ancestor(right, left)) {
                cross_mass_[static_cast<std::size_t>(left) * n_ +
                            static_cast<std::size_t>(right)] = 0;
                continue;
            }
            const std::size_t index = static_cast<std::size_t>(left) * n_ +
                                      static_cast<std::size_t>(right);
            const Mass beta = cross_mass_[index];
            const NodeId parent_left = lattice.tree().parent()[static_cast<std::size_t>(left)];
            const NodeId parent_right = lattice.tree().parent()[static_cast<std::size_t>(right)];
            const Score positive = Score{beta} + previous(parent_left, right) +
                                   previous(left, parent_right);
            const Mass overlap = previous(parent_left, parent_right);
            if (positive < overlap) {
                throw std::logic_error("loss mass inclusion and exclusion became negative");
            }
            const Mass value = checked_u64(positive - overlap, "cross event mass overflow");
            if (value > total_mass_) {
                throw std::logic_error("compiled cross mass exceeds total mass");
            }
            cross_mass_[index] = value;
        }
    }
}

Mass DenseLossMassCompiler::loss_mass(const Label& state) const {
    if (!lattice_->valid(state)) {
        throw std::invalid_argument("loss query requires a valid normalized label");
    }
    if (state.tag == LabelTag::bottom) {
        return total_mass_;
    }
    if (state.tag == LabelTag::single) {
        return chain_mass_[static_cast<std::size_t>(state.first)];
    }
    const NodeId ancestor = lattice_->tree().lca(state.first, state.second);
    const Score value = Score{chain_mass_[static_cast<std::size_t>(ancestor)]} +
                        cross_mass_[static_cast<std::size_t>(state.first) * n_ +
                                    static_cast<std::size_t>(state.second)];
    const Mass result = checked_u64(value, "label loss mass overflow");
    if (result > total_mass_) {
        throw std::logic_error("label loss mass exceeds total mass");
    }
    return result;
}

Mass DenseLossMassCompiler::total_mass() const noexcept { return total_mass_; }
std::size_t DenseLossMassCompiler::storage_bytes() const noexcept {
    return chain_mass_.capacity() * sizeof(chain_mass_[0]) +
           cross_mass_.capacity() * sizeof(cross_mass_[0]);
}
const LatticeOracle& DenseLossMassCompiler::lattice() const noexcept { return *lattice_; }

namespace {

struct CertificateRecoveryOption {
    Cost cost = 0;
    Label hull = Label::bottom();
    std::int64_t original_index = -1;
    std::vector<NodeId> replica_leaves;
    bool original_nonnull = false;
};

struct CertificateRecoveryProblem {
    std::vector<std::vector<NodeId>> children;
    std::vector<NodeId> order;
    NodeId root = -1;
    std::vector<std::vector<CertificateRecoveryOption>> menus;
    std::vector<std::vector<Label>> inherited;
    std::vector<std::vector<Label>> produced;
    std::vector<std::unordered_map<Label, std::size_t, LabelHash>> inherited_index;
    std::vector<std::unordered_map<Label, std::size_t, LabelHash>> produced_index;
    RecoveryMetrics metrics;
};

CertificateRecoveryProblem prepare_certificate_recovery(
    const RecoveryInstance& instance,
    const LatticeOracle& lattice
) {
    CertificateRecoveryProblem problem;
    problem.order = validate_recovery_tree(instance, problem.children, problem.root);
    const std::size_t n = instance.parent.size();
    const std::size_t budget_count = static_cast<std::size_t>(instance.budget) + 1U;
    problem.menus.resize(n);
    for (std::size_t node = 0; node < n; ++node) {
        std::map<Label, CertificateRecoveryOption> best;
        best.emplace(
            Label::bottom(),
            CertificateRecoveryOption{0U, Label::bottom(), -1, {}, false}
        );
        for (std::size_t option_index = 0; option_index < instance.menus[node].size();
             ++option_index) {
            const RecoveryOption& option = instance.menus[node][option_index];
            for (const NodeId replica : option.replica_leaves) {
                if (replica < 0 ||
                    static_cast<std::size_t>(replica) >= lattice.tree().size() ||
                    !lattice.tree().is_leaf(replica)) {
                    throw std::invalid_argument(
                        "recovery option contains a nonleaf replica location"
                    );
                }
            }
            if (option.cost > instance.budget) {
                continue;
            }
            const Label hull = lattice.hull_of_leaves(option.replica_leaves);
            if (!lattice.valid(hull)) {
                throw std::logic_error("menu hull is invalid");
            }
            const auto iterator = best.find(hull);
            if (iterator == best.end() || option.cost < iterator->second.cost ||
                (option.cost == iterator->second.cost &&
                 iterator->second.original_index >= 0 &&
                 option_index < static_cast<std::size_t>(
                     iterator->second.original_index
                 ))) {
                best.insert_or_assign(hull, CertificateRecoveryOption{
                    option.cost,
                    hull,
                    static_cast<std::int64_t>(option_index),
                    option.replica_leaves,
                    !option.replica_leaves.empty(),
                });
            }
        }
        for (const auto& entry : best) {
            problem.menus[node].push_back(entry.second);
        }
        std::sort(
            problem.menus[node].begin(), problem.menus[node].end(),
            [](const CertificateRecoveryOption& left,
               const CertificateRecoveryOption& right) {
                return std::tie(left.cost, left.hull, left.original_index) <
                       std::tie(right.cost, right.hull, right.original_index);
            }
        );
        problem.metrics.compressed_option_count += problem.menus[node].size();
    }

    problem.inherited.resize(n);
    problem.produced.resize(n);
    problem.inherited[static_cast<std::size_t>(problem.root)] = {Label::bottom()};
    for (const NodeId node_id : problem.order) {
        const std::size_t node = static_cast<std::size_t>(node_id);
        std::unordered_set<Label, LabelHash> output;
        for (const Label& state : problem.inherited[node]) {
            for (const CertificateRecoveryOption& option : problem.menus[node]) {
                output.insert(lattice.join(state, option.hull));
                ++problem.metrics.state_option_transitions;
            }
        }
        problem.produced[node].assign(output.begin(), output.end());
        std::sort(problem.produced[node].begin(), problem.produced[node].end());
        if (problem.produced[node].empty()) {
            throw std::logic_error(
                "reachable produced set is empty despite the null option"
            );
        }
        for (const NodeId child : problem.children[node]) {
            problem.inherited[static_cast<std::size_t>(child)] =
                problem.produced[node];
        }
    }

    problem.metrics.inherited_state_count.resize(n);
    problem.metrics.produced_state_count.resize(n);
    problem.inherited_index.resize(n);
    problem.produced_index.resize(n);
    for (std::size_t node = 0; node < n; ++node) {
        problem.metrics.inherited_state_count[node] = problem.inherited[node].size();
        problem.metrics.produced_state_count[node] = problem.produced[node].size();
        problem.metrics.score_cells = checked_u64(
            Score{problem.metrics.score_cells} +
                Score{problem.inherited[node].size()} * budget_count,
            "certificate recovery score cell count overflow"
        );
        for (std::size_t index = 0; index < problem.inherited[node].size(); ++index) {
            problem.inherited_index[node].emplace(
                problem.inherited[node][index], index
            );
        }
        for (std::size_t index = 0; index < problem.produced[node].size(); ++index) {
            problem.produced_index[node].emplace(problem.produced[node][index], index);
        }
    }
    return problem;
}

struct TwoDistinctScores {
    std::array<Score, 2U> score{0U, 0U};
    std::size_t size = 0U;

    void insert(Score candidate) {
        if ((size >= 1U && score[0] == candidate) ||
            (size >= 2U && score[1] == candidate)) {
            return;
        }
        if (size == 0U) {
            score[0] = candidate;
            size = 1U;
            return;
        }
        if (candidate > score[0]) {
            if (size == 1U) {
                size = 2U;
            }
            score[1] = score[0];
            score[0] = candidate;
            return;
        }
        if (size == 1U || candidate > score[1]) {
            score[1] = candidate;
            size = 2U;
        }
    }

    [[nodiscard]] std::optional<std::size_t> rank_of(Score value) const {
        for (std::size_t rank = 0; rank < size; ++rank) {
            if (score[rank] == value) {
                return rank;
            }
        }
        return std::nullopt;
    }
};

}  // namespace

RecoveryOptimizer::RecoveryOptimizer(const LatticeOracle& lattice,
                                     const DenseLossMassCompiler& loss) noexcept
    : lattice_(&lattice), loss_(&loss) {}

RecoveryResult RecoveryOptimizer::solve(const RecoveryInstance& instance) const {
    std::vector<std::vector<NodeId>> children;
    NodeId root = -1;
    const std::vector<NodeId> order = validate_recovery_tree(instance, children, root);
    require_recovery_score_bound(
        instance, loss_->total_mass(), "recovery objective exceeds the exact score type"
    );
    const std::size_t n = instance.parent.size();
    const std::size_t budget_count = static_cast<std::size_t>(instance.budget) + 1;

    struct CompressedOption {
        Cost cost;
        Label hull;
        std::int64_t original_index;
        std::vector<NodeId> replica_leaves;
    };
    std::vector<std::vector<CompressedOption>> menus(n);
    RecoveryResult result;
    result.probability_denominator = loss_->total_mass();
    for (std::size_t node = 0; node < n; ++node) {
        std::map<Label, CompressedOption> best;
        best.emplace(
            Label::bottom(),
            CompressedOption{0U, Label::bottom(), -1, {}}
        );
        for (std::size_t option_index = 0; option_index < instance.menus[node].size();
             ++option_index) {
            const RecoveryOption& option = instance.menus[node][option_index];
            for (const NodeId replica : option.replica_leaves) {
                if (replica < 0 || static_cast<std::size_t>(replica) >= lattice_->tree().size() ||
                    !lattice_->tree().is_leaf(replica)) {
                    throw std::invalid_argument("recovery option contains a nonleaf replica location");
                }
            }
            if (option.cost > instance.budget) {
                continue;
            }
            const Label hull = lattice_->hull_of_leaves(option.replica_leaves);
            if (!lattice_->valid(hull)) {
                throw std::logic_error("menu hull is invalid");
            }
            const auto iterator = best.find(hull);
            if (iterator == best.end() || option.cost < iterator->second.cost ||
                (option.cost == iterator->second.cost &&
                 iterator->second.original_index >= 0 &&
                 option_index < static_cast<std::size_t>(iterator->second.original_index))) {
                best.insert_or_assign(hull, CompressedOption{
                    option.cost,
                    hull,
                    static_cast<std::int64_t>(option_index),
                    option.replica_leaves,
                });
            }
        }
        for (const auto& entry : best) {
            menus[node].push_back(entry.second);
        }
        std::sort(menus[node].begin(), menus[node].end(), [](const auto& left, const auto& right) {
            return std::tie(left.cost, left.hull, left.original_index) <
                   std::tie(right.cost, right.hull, right.original_index);
        });
        result.metrics.compressed_option_count += menus[node].size();
    }

    std::vector<std::vector<Label>> inherited(n);
    std::vector<std::vector<Label>> produced(n);
    inherited[static_cast<std::size_t>(root)] = {Label::bottom()};
    for (const NodeId node_id : order) {
        const std::size_t node = static_cast<std::size_t>(node_id);
        std::unordered_set<Label, LabelHash> output;
        for (const Label& state : inherited[node]) {
            for (const CompressedOption& option : menus[node]) {
                output.insert(lattice_->join(state, option.hull));
                ++result.metrics.state_option_transitions;
            }
        }
        produced[node].assign(output.begin(), output.end());
        std::sort(produced[node].begin(), produced[node].end());
        if (produced[node].empty()) {
            throw std::logic_error("reachable produced set is empty despite the null option");
        }
        for (const NodeId child : children[node]) {
            inherited[static_cast<std::size_t>(child)] = produced[node];
        }
    }
    result.metrics.inherited_state_count.resize(n);
    result.metrics.produced_state_count.resize(n);
    for (std::size_t node = 0; node < n; ++node) {
        result.metrics.inherited_state_count[node] = inherited[node].size();
        result.metrics.produced_state_count[node] = produced[node].size();
        const Score cells = Score{inherited[node].size()} * budget_count;
        result.metrics.score_cells = checked_u64(
            Score{result.metrics.score_cells} + cells,
            "recovery score cell count overflow"
        );
    }

    using ScoreRows = std::vector<std::vector<Score>>;
    std::vector<ScoreRows> tables(n);
    std::vector<std::unordered_map<Label, std::size_t, LabelHash>> inherited_index(n);
    std::vector<std::unordered_map<Label, std::size_t, LabelHash>> produced_index(n);
    for (std::size_t node = 0; node < n; ++node) {
        for (std::size_t index = 0; index < inherited[node].size(); ++index) {
            inherited_index[node].emplace(inherited[node][index], index);
        }
        for (std::size_t index = 0; index < produced[node].size(); ++index) {
            produced_index[node].emplace(produced[node][index], index);
        }
    }

    for (auto iterator = order.rbegin(); iterator != order.rend(); ++iterator) {
        const std::size_t node = static_cast<std::size_t>(*iterator);
        ScoreRows child_value(produced[node].size(), std::vector<Score>(budget_count, 0));
        for (const NodeId child_id : children[node]) {
            const std::size_t child = static_cast<std::size_t>(child_id);
            ScoreRows next_value(produced[node].size(), std::vector<Score>(budget_count, 0));
            for (std::size_t state_index = 0; state_index < produced[node].size(); ++state_index) {
                const Label state = produced[node][state_index];
                const auto child_state = inherited_index[child].find(state);
                if (child_state == inherited_index[child].end()) {
                    throw std::logic_error("child does not contain its inherited state");
                }
                const std::vector<Score>& child_row = tables[child][child_state->second];
                for (std::size_t budget = 0; budget < budget_count; ++budget) {
                    Score best = 0;
                    for (std::size_t child_budget = 0; child_budget <= budget; ++child_budget) {
                        const Score candidate = child_value[state_index][budget - child_budget] +
                                                child_row[child_budget];
                        best = std::max(best, candidate);
                        ++result.metrics.budget_convolution_operations;
                    }
                    next_value[state_index][budget] = best;
                }
            }
            child_value = std::move(next_value);
        }

        tables[node].assign(inherited[node].size(), std::vector<Score>(budget_count, 0));
        for (std::size_t state_index = 0; state_index < inherited[node].size(); ++state_index) {
            const Label inherited_state = inherited[node][state_index];
            for (std::size_t budget = 0; budget < budget_count; ++budget) {
                Score best = 0;
                for (const CompressedOption& option : menus[node]) {
                    if (option.cost > budget) {
                        continue;
                    }
                    const Label state = lattice_->join(inherited_state, option.hull);
                    const std::size_t produced_state = produced_index[node].at(state);
                    const Mass loss_mass = loss_->loss_mass(state);
                    const Score local = Score{instance.benefit[node]} *
                                        (loss_->total_mass() - loss_mass);
                    const Score candidate = local +
                        child_value[produced_state][budget - option.cost];
                    best = std::max(best, candidate);
                }
                tables[node][state_index][budget] = best;
            }
        }
    }
    const std::size_t root_state = inherited_index[static_cast<std::size_t>(root)].at(Label::bottom());
    result.objective_numerator = tables[static_cast<std::size_t>(root)][root_state][instance.budget];
    result.secondary_objective_numerator = result.objective_numerator;
    result.secondary_probability_denominator = result.probability_denominator;
    result.selected_option_hulls.assign(n, Label::bottom());
    result.selected_option_costs.assign(n, 0);
    result.selected_option_indices.assign(n, -1);
    result.selected_replica_leaves.assign(n, {});
    std::function<void(NodeId, std::size_t, Label)> reconstruct =
        [&](NodeId node_id, std::size_t budget, Label inherited_state) {
        const std::size_t node = static_cast<std::size_t>(node_id);
        const std::size_t state_index = inherited_index[node].at(inherited_state);
        const Score target = tables[node][state_index][budget];
        for (const CompressedOption& option : menus[node]) {
            if (option.cost > budget) {
                continue;
            }
            const Label produced_state = lattice_->join(inherited_state, option.hull);
            const std::size_t remaining_budget = budget - option.cost;
            std::vector<Score> current(remaining_budget + 1, 0);
            std::vector<std::vector<std::size_t>> chosen_split;
            chosen_split.reserve(children[node].size());
            for (const NodeId child_id : children[node]) {
                const std::size_t child = static_cast<std::size_t>(child_id);
                const std::size_t child_state = inherited_index[child].at(produced_state);
                const std::vector<Score>& child_row = tables[child][child_state];
                std::vector<Score> next(remaining_budget + 1, 0);
                std::vector<std::size_t> split(remaining_budget + 1, 0);
                for (std::size_t available = 0; available <= remaining_budget; ++available) {
                    Score best = 0;
                    std::size_t best_child_budget = 0;
                    for (std::size_t child_budget = 0; child_budget <= available; ++child_budget) {
                        const Score candidate = current[available - child_budget] +
                                                child_row[child_budget];
                        if (candidate > best) {
                            best = candidate;
                            best_child_budget = child_budget;
                        }
                    }
                    next[available] = best;
                    split[available] = best_child_budget;
                }
                current = std::move(next);
                chosen_split.push_back(std::move(split));
            }
            const Score local = Score{instance.benefit[node]} *
                (loss_->total_mass() - loss_->loss_mass(produced_state));
            if (local + current[remaining_budget] != target) {
                continue;
            }
            result.selected_option_hulls[node] = option.hull;
            result.selected_option_costs[node] = option.cost;
            result.selected_option_indices[node] = option.original_index;
            result.selected_replica_leaves[node] = option.replica_leaves;
            result.selected_total_cost += option.cost;
            std::vector<std::size_t> child_budgets(children[node].size(), 0);
            std::size_t available = remaining_budget;
            for (std::size_t reverse = children[node].size(); reverse > 0; --reverse) {
                const std::size_t child_budget = chosen_split[reverse - 1][available];
                child_budgets[reverse - 1] = child_budget;
                available -= child_budget;
            }
            for (std::size_t child_index = 0; child_index < children[node].size(); ++child_index) {
                reconstruct(children[node][child_index], child_budgets[child_index], produced_state);
            }
            return;
        }
        throw std::logic_error("recovery optimum cannot be reconstructed");
    };
    reconstruct(root, instance.budget, Label::bottom());
    if (result.selected_total_cost > instance.budget) {
        throw std::logic_error("reconstructed deployment exceeds its budget");
    }
    return result;
}

RecoveryResult RecoveryOptimizer::solve_lexicographic(
    const RecoveryInstance& instance,
    const DenseLossMassCompiler& secondary_loss,
    SecondaryTiePolicy policy
) const {
    std::vector<std::vector<NodeId>> children;
    NodeId root = -1;
    const std::vector<NodeId> order = validate_recovery_tree(instance, children, root);
    if (&secondary_loss.lattice() != lattice_) {
        throw std::invalid_argument(
            "lexicographic losses must share the optimizer lattice oracle"
        );
    }
    if (secondary_loss.total_mass() != loss_->total_mass()) {
        throw std::invalid_argument(
            "lexicographic primary and secondary distributions must have equal total mass"
        );
    }
    require_recovery_score_bound(
        instance, loss_->total_mass(),
        "lexicographic primary objective exceeds the exact score type"
    );
    require_recovery_score_bound(
        instance, secondary_loss.total_mass(),
        "lexicographic secondary objective exceeds the exact score type"
    );
    const std::size_t n = instance.parent.size();
    const std::size_t budget_count = static_cast<std::size_t>(instance.budget) + 1U;

    struct CompressedOption {
        Cost cost;
        Label hull;
        std::int64_t original_index;
        std::vector<NodeId> replica_leaves;
    };
    std::vector<std::vector<CompressedOption>> menus(n);
    RecoveryResult result;
    result.probability_denominator = loss_->total_mass();
    result.secondary_probability_denominator = secondary_loss.total_mass();
    for (std::size_t node = 0; node < n; ++node) {
        std::map<Label, CompressedOption> best;
        best.emplace(Label::bottom(), CompressedOption{0U, Label::bottom(), -1, {}});
        for (std::size_t option_index = 0; option_index < instance.menus[node].size();
             ++option_index) {
            const RecoveryOption& option = instance.menus[node][option_index];
            for (const NodeId replica : option.replica_leaves) {
                if (replica < 0 || static_cast<std::size_t>(replica) >= lattice_->tree().size() ||
                    !lattice_->tree().is_leaf(replica)) {
                    throw std::invalid_argument(
                        "recovery option contains a nonleaf replica location"
                    );
                }
            }
            if (option.cost > instance.budget) continue;
            const Label hull = lattice_->hull_of_leaves(option.replica_leaves);
            if (!lattice_->valid(hull)) {
                throw std::logic_error("menu hull is invalid");
            }
            const auto iterator = best.find(hull);
            if (iterator == best.end() || option.cost < iterator->second.cost ||
                (option.cost == iterator->second.cost &&
                 iterator->second.original_index >= 0 &&
                 option_index < static_cast<std::size_t>(iterator->second.original_index))) {
                best.insert_or_assign(hull, CompressedOption{
                    option.cost,
                    hull,
                    static_cast<std::int64_t>(option_index),
                    option.replica_leaves,
                });
            }
        }
        for (const auto& entry : best) menus[node].push_back(entry.second);
        std::sort(menus[node].begin(), menus[node].end(), [](const auto& left,
                                                              const auto& right) {
            return std::tie(left.cost, left.hull, left.original_index) <
                   std::tie(right.cost, right.hull, right.original_index);
        });
        result.metrics.compressed_option_count += menus[node].size();
    }

    std::vector<std::vector<Label>> inherited(n);
    std::vector<std::vector<Label>> produced(n);
    inherited[static_cast<std::size_t>(root)] = {Label::bottom()};
    for (const NodeId node_id : order) {
        const std::size_t node = static_cast<std::size_t>(node_id);
        std::unordered_set<Label, LabelHash> output;
        for (const Label& state : inherited[node]) {
            for (const CompressedOption& option : menus[node]) {
                output.insert(lattice_->join(state, option.hull));
                ++result.metrics.state_option_transitions;
            }
        }
        produced[node].assign(output.begin(), output.end());
        std::sort(produced[node].begin(), produced[node].end());
        if (produced[node].empty()) {
            throw std::logic_error("reachable produced set is empty despite the null option");
        }
        for (const NodeId child : children[node]) {
            inherited[static_cast<std::size_t>(child)] = produced[node];
        }
    }
    result.metrics.inherited_state_count.resize(n);
    result.metrics.produced_state_count.resize(n);
    for (std::size_t node = 0; node < n; ++node) {
        result.metrics.inherited_state_count[node] = inherited[node].size();
        result.metrics.produced_state_count[node] = produced[node].size();
        result.metrics.score_cells = checked_u64(
            Score{result.metrics.score_cells} +
                Score{inherited[node].size()} * budget_count,
            "lexicographic recovery score cell count overflow"
        );
    }

    struct LexScore {
        Score primary = 0;
        Score secondary = 0;
    };
    const auto better = [&](const LexScore& left, const LexScore& right) {
        if (left.primary != right.primary) return left.primary > right.primary;
        return policy == SecondaryTiePolicy::optimistic
            ? left.secondary > right.secondary
            : left.secondary < right.secondary;
    };
    const auto equal = [](const LexScore& left, const LexScore& right) {
        return left.primary == right.primary && left.secondary == right.secondary;
    };
    const auto add = [](const LexScore& left, const LexScore& right) {
        return LexScore{left.primary + right.primary, left.secondary + right.secondary};
    };
    using ScoreRows = std::vector<std::vector<LexScore>>;
    std::vector<ScoreRows> tables(n);
    std::vector<std::unordered_map<Label, std::size_t, LabelHash>> inherited_index(n);
    std::vector<std::unordered_map<Label, std::size_t, LabelHash>> produced_index(n);
    for (std::size_t node = 0; node < n; ++node) {
        for (std::size_t index = 0; index < inherited[node].size(); ++index) {
            inherited_index[node].emplace(inherited[node][index], index);
        }
        for (std::size_t index = 0; index < produced[node].size(); ++index) {
            produced_index[node].emplace(produced[node][index], index);
        }
    }

    for (auto iterator = order.rbegin(); iterator != order.rend(); ++iterator) {
        const std::size_t node = static_cast<std::size_t>(*iterator);
        ScoreRows child_value(
            produced[node].size(), std::vector<LexScore>(budget_count)
        );
        for (const NodeId child_id : children[node]) {
            const std::size_t child = static_cast<std::size_t>(child_id);
            ScoreRows next_value(
                produced[node].size(), std::vector<LexScore>(budget_count)
            );
            for (std::size_t state_index = 0; state_index < produced[node].size();
                 ++state_index) {
                const Label state = produced[node][state_index];
                const auto child_state = inherited_index[child].find(state);
                if (child_state == inherited_index[child].end()) {
                    throw std::logic_error("child does not contain its inherited state");
                }
                const std::vector<LexScore>& child_row =
                    tables[child][child_state->second];
                for (std::size_t budget = 0; budget < budget_count; ++budget) {
                    LexScore best{};
                    bool found = false;
                    for (std::size_t child_budget = 0; child_budget <= budget;
                         ++child_budget) {
                        const LexScore candidate = add(
                            child_value[state_index][budget - child_budget],
                            child_row[child_budget]
                        );
                        if (!found || better(candidate, best)) {
                            best = candidate;
                            found = true;
                        }
                        ++result.metrics.budget_convolution_operations;
                    }
                    if (!found) throw std::logic_error("lexicographic convolution is empty");
                    next_value[state_index][budget] = best;
                }
            }
            child_value = std::move(next_value);
        }

        tables[node].assign(
            inherited[node].size(), std::vector<LexScore>(budget_count)
        );
        for (std::size_t state_index = 0; state_index < inherited[node].size();
             ++state_index) {
            const Label inherited_state = inherited[node][state_index];
            for (std::size_t budget = 0; budget < budget_count; ++budget) {
                LexScore best{};
                bool found = false;
                for (const CompressedOption& option : menus[node]) {
                    if (option.cost > budget) continue;
                    const Label state = lattice_->join(inherited_state, option.hull);
                    const std::size_t produced_state = produced_index[node].at(state);
                    const LexScore local{
                        Score{instance.benefit[node]} *
                            (loss_->total_mass() - loss_->loss_mass(state)),
                        Score{instance.benefit[node]} *
                            (secondary_loss.total_mass() - secondary_loss.loss_mass(state)),
                    };
                    const LexScore candidate = add(
                        local, child_value[produced_state][budget - option.cost]
                    );
                    if (!found || better(candidate, best)) {
                        best = candidate;
                        found = true;
                    }
                }
                if (!found) throw std::logic_error("lexicographic menu has no feasible option");
                tables[node][state_index][budget] = best;
            }
        }
    }
    const std::size_t root_state = inherited_index[static_cast<std::size_t>(root)].at(
        Label::bottom()
    );
    const LexScore optimum =
        tables[static_cast<std::size_t>(root)][root_state][instance.budget];
    result.objective_numerator = optimum.primary;
    result.secondary_objective_numerator = optimum.secondary;
    result.selected_option_hulls.assign(n, Label::bottom());
    result.selected_option_costs.assign(n, 0U);
    result.selected_option_indices.assign(n, -1);
    result.selected_replica_leaves.assign(n, {});

    std::function<void(NodeId, std::size_t, Label)> reconstruct =
        [&](NodeId node_id, std::size_t budget, Label inherited_state) {
        const std::size_t node = static_cast<std::size_t>(node_id);
        const std::size_t state_index = inherited_index[node].at(inherited_state);
        const LexScore target = tables[node][state_index][budget];
        for (const CompressedOption& option : menus[node]) {
            if (option.cost > budget) continue;
            const Label produced_state = lattice_->join(inherited_state, option.hull);
            const std::size_t remaining_budget = budget - option.cost;
            std::vector<LexScore> current(remaining_budget + 1U);
            std::vector<std::vector<std::size_t>> chosen_split;
            chosen_split.reserve(children[node].size());
            for (const NodeId child_id : children[node]) {
                const std::size_t child = static_cast<std::size_t>(child_id);
                const std::size_t child_state = inherited_index[child].at(produced_state);
                const std::vector<LexScore>& child_row = tables[child][child_state];
                std::vector<LexScore> next(remaining_budget + 1U);
                std::vector<std::size_t> split(remaining_budget + 1U, 0U);
                for (std::size_t available = 0; available <= remaining_budget; ++available) {
                    LexScore best{};
                    std::size_t best_child_budget = 0U;
                    bool found = false;
                    for (std::size_t child_budget = 0; child_budget <= available;
                         ++child_budget) {
                        const LexScore candidate = add(
                            current[available - child_budget], child_row[child_budget]
                        );
                        if (!found || better(candidate, best)) {
                            best = candidate;
                            best_child_budget = child_budget;
                            found = true;
                        }
                    }
                    if (!found) {
                        throw std::logic_error("lexicographic reconstruction split is empty");
                    }
                    next[available] = best;
                    split[available] = best_child_budget;
                }
                current = std::move(next);
                chosen_split.push_back(std::move(split));
            }
            const LexScore local{
                Score{instance.benefit[node]} *
                    (loss_->total_mass() - loss_->loss_mass(produced_state)),
                Score{instance.benefit[node]} *
                    (secondary_loss.total_mass() - secondary_loss.loss_mass(produced_state)),
            };
            if (!equal(add(local, current[remaining_budget]), target)) continue;
            result.selected_option_hulls[node] = option.hull;
            result.selected_option_costs[node] = option.cost;
            result.selected_option_indices[node] = option.original_index;
            result.selected_replica_leaves[node] = option.replica_leaves;
            result.selected_total_cost += option.cost;
            std::vector<std::size_t> child_budgets(children[node].size(), 0U);
            std::size_t available = remaining_budget;
            for (std::size_t reverse = children[node].size(); reverse > 0U; --reverse) {
                const std::size_t child_budget = chosen_split[reverse - 1U][available];
                child_budgets[reverse - 1U] = child_budget;
                available -= child_budget;
            }
            for (std::size_t child_index = 0; child_index < children[node].size();
                 ++child_index) {
                reconstruct(
                    children[node][child_index], child_budgets[child_index], produced_state
                );
            }
            return;
        }
        throw std::logic_error("lexicographic recovery optimum cannot be reconstructed");
    };
    reconstruct(root, instance.budget, Label::bottom());
    if (result.selected_total_cost > instance.budget) {
        throw std::logic_error("lexicographic deployment exceeds its budget");
    }
    std::vector<Label> path(n, Label::bottom());
    LexScore reconstructed{};
    for (const NodeId node_id : order) {
        const std::size_t node = static_cast<std::size_t>(node_id);
        const NodeId parent = instance.parent[node];
        path[node] = parent < 0
            ? result.selected_option_hulls[node]
            : lattice_->join(
                path[static_cast<std::size_t>(parent)],
                result.selected_option_hulls[node]
            );
        reconstructed.primary += Score{instance.benefit[node]} *
            (loss_->total_mass() - loss_->loss_mass(path[node]));
        reconstructed.secondary += Score{instance.benefit[node]} *
            (secondary_loss.total_mass() - secondary_loss.loss_mass(path[node]));
    }
    if (!equal(reconstructed, optimum)) {
        throw std::logic_error("lexicographic reconstruction does not attain its optimum");
    }
    return result;
}

OptimalImageCountCertificate RecoveryOptimizer::certify_optimal_image_count(
    const RecoveryInstance& instance,
    const std::vector<bool>& artificial_nodes
) const {
    require_recovery_score_bound(
        instance, loss_->total_mass(),
        "image-count certificate objective exceeds the exact score type"
    );
    const std::size_t n = instance.parent.size();
    if (artificial_nodes.size() != n) {
        throw std::invalid_argument(
            "image-count certificate requires one artificial-node flag per recovery node"
        );
    }
    const CertificateRecoveryProblem problem =
        prepare_certificate_recovery(instance, *lattice_);
    std::size_t artificial_count = 0U;
    for (std::size_t node = 0; node < n; ++node) {
        if (!artificial_nodes[node]) {
            continue;
        }
        ++artificial_count;
        if (static_cast<NodeId>(node) != problem.root || instance.benefit[node] != 0U ||
            !instance.menus[node].empty()) {
            throw std::invalid_argument(
                "an artificial recovery node must be the zero-benefit root with an empty menu"
            );
        }
    }
    if (artificial_count > 1U) {
        throw std::invalid_argument("a recovery tree can have at most one artificial root");
    }

    enum class CountPolicy : std::uint8_t {
        minimum = 0,
        maximum = 1,
    };
    struct CountScore {
        Score primary = 0U;
        std::uint64_t image_count = 0U;
        bool valid = false;
    };
    struct CountRun {
        RecoveryResult deployment;
        std::uint64_t image_count = 0U;
    };
    const std::size_t budget_count = static_cast<std::size_t>(instance.budget) + 1U;

    const auto solve_policy = [&](CountPolicy policy) -> CountRun {
        const auto better = [&](const CountScore& left, const CountScore& right) {
            if (left.primary != right.primary) {
                return left.primary > right.primary;
            }
            return policy == CountPolicy::minimum
                ? left.image_count < right.image_count
                : left.image_count > right.image_count;
        };
        const auto equal = [](const CountScore& left, const CountScore& right) {
            return left.valid == right.valid && left.primary == right.primary &&
                   left.image_count == right.image_count;
        };
        const auto add = [](const CountScore& left, const CountScore& right) {
            if (!left.valid || !right.valid) {
                return CountScore{};
            }
            return CountScore{
                left.primary + right.primary,
                left.image_count + right.image_count,
                true,
            };
        };

        using CountRows = std::vector<std::vector<CountScore>>;
        std::vector<CountRows> tables(n);
        RecoveryResult result;
        result.probability_denominator = loss_->total_mass();
        result.secondary_probability_denominator = loss_->total_mass();
        result.metrics = problem.metrics;

        for (auto iterator = problem.order.rbegin(); iterator != problem.order.rend();
             ++iterator) {
            const std::size_t node = static_cast<std::size_t>(*iterator);
            CountRows child_value(
                problem.produced[node].size(),
                std::vector<CountScore>(budget_count, CountScore{0U, 0U, true})
            );
            for (const NodeId child_id : problem.children[node]) {
                const std::size_t child = static_cast<std::size_t>(child_id);
                CountRows next_value(
                    problem.produced[node].size(),
                    std::vector<CountScore>(budget_count)
                );
                for (std::size_t state_index = 0;
                     state_index < problem.produced[node].size(); ++state_index) {
                    const Label state = problem.produced[node][state_index];
                    const std::size_t child_state =
                        problem.inherited_index[child].at(state);
                    const std::vector<CountScore>& child_row =
                        tables[child][child_state];
                    for (std::size_t budget = 0; budget < budget_count; ++budget) {
                        CountScore best;
                        for (std::size_t child_budget = 0; child_budget <= budget;
                             ++child_budget) {
                            const CountScore candidate = add(
                                child_value[state_index][budget - child_budget],
                                child_row[child_budget]
                            );
                            if (candidate.valid &&
                                (!best.valid || better(candidate, best))) {
                                best = candidate;
                            }
                            ++result.metrics.budget_convolution_operations;
                        }
                        if (!best.valid) {
                            throw std::logic_error(
                                "image-count certificate convolution is empty"
                            );
                        }
                        next_value[state_index][budget] = best;
                    }
                }
                child_value = std::move(next_value);
            }

            tables[node].assign(
                problem.inherited[node].size(),
                std::vector<CountScore>(budget_count)
            );
            for (std::size_t state_index = 0;
                 state_index < problem.inherited[node].size(); ++state_index) {
                const Label inherited_state = problem.inherited[node][state_index];
                for (std::size_t budget = 0; budget < budget_count; ++budget) {
                    CountScore best;
                    for (const CertificateRecoveryOption& option :
                         problem.menus[node]) {
                        if (option.cost > budget) {
                            continue;
                        }
                        const Label state = lattice_->join(inherited_state, option.hull);
                        const std::size_t produced_state =
                            problem.produced_index[node].at(state);
                        const CountScore local{
                            Score{instance.benefit[node]} *
                                (loss_->total_mass() - loss_->loss_mass(state)),
                            (!artificial_nodes[node] && option.original_nonnull) ? 1U : 0U,
                            true,
                        };
                        const CountScore candidate = add(
                            local,
                            child_value[produced_state][budget - option.cost]
                        );
                        if (!best.valid || better(candidate, best)) {
                            best = candidate;
                        }
                    }
                    if (!best.valid) {
                        throw std::logic_error(
                            "image-count certificate menu has no feasible option"
                        );
                    }
                    tables[node][state_index][budget] = best;
                }
            }
        }

        const std::size_t root_state =
            problem.inherited_index[static_cast<std::size_t>(problem.root)].at(
                Label::bottom()
            );
        const CountScore optimum =
            tables[static_cast<std::size_t>(problem.root)][root_state][instance.budget];
        if (!optimum.valid) {
            throw std::logic_error("image-count certificate root is infeasible");
        }
        result.objective_numerator = optimum.primary;
        result.secondary_objective_numerator = optimum.primary;
        result.selected_option_hulls.assign(n, Label::bottom());
        result.selected_option_costs.assign(n, 0U);
        result.selected_option_indices.assign(n, -1);
        result.selected_replica_leaves.assign(n, {});

        std::function<void(NodeId, std::size_t, Label)> reconstruct =
            [&](NodeId node_id, std::size_t budget, Label inherited_state) {
            const std::size_t node = static_cast<std::size_t>(node_id);
            const std::size_t state_index =
                problem.inherited_index[node].at(inherited_state);
            const CountScore target = tables[node][state_index][budget];
            for (const CertificateRecoveryOption& option : problem.menus[node]) {
                if (option.cost > budget) {
                    continue;
                }
                const Label produced_state = lattice_->join(inherited_state, option.hull);
                const std::size_t remaining_budget = budget - option.cost;
                std::vector<CountScore> current(
                    remaining_budget + 1U, CountScore{0U, 0U, true}
                );
                std::vector<std::vector<std::size_t>> chosen_split;
                chosen_split.reserve(problem.children[node].size());
                for (const NodeId child_id : problem.children[node]) {
                    const std::size_t child = static_cast<std::size_t>(child_id);
                    const std::size_t child_state =
                        problem.inherited_index[child].at(produced_state);
                    const std::vector<CountScore>& child_row =
                        tables[child][child_state];
                    std::vector<CountScore> next(remaining_budget + 1U);
                    std::vector<std::size_t> split(remaining_budget + 1U, 0U);
                    for (std::size_t available = 0; available <= remaining_budget;
                         ++available) {
                        CountScore best;
                        std::size_t best_child_budget = 0U;
                        for (std::size_t child_budget = 0;
                             child_budget <= available; ++child_budget) {
                            const CountScore candidate = add(
                                current[available - child_budget],
                                child_row[child_budget]
                            );
                            if (candidate.valid &&
                                (!best.valid || better(candidate, best))) {
                                best = candidate;
                                best_child_budget = child_budget;
                            }
                        }
                        if (!best.valid) {
                            throw std::logic_error(
                                "image-count reconstruction convolution is empty"
                            );
                        }
                        next[available] = best;
                        split[available] = best_child_budget;
                    }
                    current = std::move(next);
                    chosen_split.push_back(std::move(split));
                }
                const CountScore local{
                    Score{instance.benefit[node]} *
                        (loss_->total_mass() - loss_->loss_mass(produced_state)),
                    (!artificial_nodes[node] && option.original_nonnull) ? 1U : 0U,
                    true,
                };
                if (!equal(add(local, current[remaining_budget]), target)) {
                    continue;
                }
                result.selected_option_hulls[node] = option.hull;
                result.selected_option_costs[node] = option.cost;
                result.selected_option_indices[node] = option.original_index;
                result.selected_replica_leaves[node] = option.replica_leaves;
                result.selected_total_cost += option.cost;
                std::vector<std::size_t> child_budgets(
                    problem.children[node].size(), 0U
                );
                std::size_t available = remaining_budget;
                for (std::size_t reverse = problem.children[node].size(); reverse > 0U;
                     --reverse) {
                    const std::size_t child_budget =
                        chosen_split[reverse - 1U][available];
                    child_budgets[reverse - 1U] = child_budget;
                    available -= child_budget;
                }
                for (std::size_t child_index = 0;
                     child_index < problem.children[node].size(); ++child_index) {
                    reconstruct(
                        problem.children[node][child_index],
                        child_budgets[child_index], produced_state
                    );
                }
                return;
            }
            throw std::logic_error(
                "image-count certificate optimum cannot be reconstructed"
            );
        };
        reconstruct(problem.root, instance.budget, Label::bottom());
        if (result.selected_total_cost > instance.budget) {
            throw std::logic_error(
                "image-count certificate deployment exceeds its budget"
            );
        }

        std::vector<Label> path(n, Label::bottom());
        Score reconstructed_score = 0U;
        std::uint64_t reconstructed_count = 0U;
        for (const NodeId node_id : problem.order) {
            const std::size_t node = static_cast<std::size_t>(node_id);
            const NodeId parent = instance.parent[node];
            path[node] = parent < 0
                ? result.selected_option_hulls[node]
                : lattice_->join(
                    path[static_cast<std::size_t>(parent)],
                    result.selected_option_hulls[node]
                );
            reconstructed_score += Score{instance.benefit[node]} *
                (loss_->total_mass() - loss_->loss_mass(path[node]));
            const std::int64_t original_index = result.selected_option_indices[node];
            if (original_index >= 0) {
                const RecoveryOption& original = instance.menus[node].at(
                    static_cast<std::size_t>(original_index)
                );
                if (original.cost != result.selected_option_costs[node] ||
                    original.replica_leaves != result.selected_replica_leaves[node]) {
                    throw std::logic_error(
                        "image-count reconstruction does not identify its original option"
                    );
                }
                if (!artificial_nodes[node] && !original.replica_leaves.empty()) {
                    ++reconstructed_count;
                }
            }
        }
        if (reconstructed_score != optimum.primary ||
            reconstructed_count != optimum.image_count) {
            throw std::logic_error(
                "image-count reconstruction does not attain its certified pair"
            );
        }
        return CountRun{std::move(result), reconstructed_count};
    };

    CountRun minimum = solve_policy(CountPolicy::minimum);
    CountRun maximum = solve_policy(CountPolicy::maximum);
    if (minimum.deployment.objective_numerator !=
            maximum.deployment.objective_numerator ||
        minimum.image_count > maximum.image_count) {
        throw std::logic_error("image-count certificate extrema are inconsistent");
    }
    OptimalImageCountCertificate certificate;
    certificate.best_raw_score = minimum.deployment.objective_numerator;
    certificate.minimum_real_nonnull_images = minimum.image_count;
    certificate.maximum_real_nonnull_images = maximum.image_count;

    for (std::size_t node = 0U; node < n; ++node) {
        if (artificial_nodes[node] && std::any_of(
                instance.menus[node].begin(), instance.menus[node].end(),
                [](const RecoveryOption& option) {
                    return !option.replica_leaves.empty();
                })) {
            throw std::invalid_argument(
                "exact-one certificate requires artificial nodes to have null-only menus"
            );
        }
    }

    // Exactly one real nonnull image has a complete polynomial certificate.
    // Such a deployment is determined by one original nonnull option and null
    // choices at every other real node, so enumerating every original option is
    // exhaustive and retains a direct witness.
    std::optional<RecoveryResult> exact_one_best;
    for (const NodeId selected_node_id : problem.order) {
        const std::size_t selected_node = static_cast<std::size_t>(selected_node_id);
        if (artificial_nodes[selected_node]) continue;
        for (std::size_t option_index = 0U;
             option_index < instance.menus[selected_node].size(); ++option_index) {
            const RecoveryOption& option = instance.menus[selected_node][option_index];
            if (option.replica_leaves.empty() || option.cost > instance.budget) continue;

            RecoveryResult witness;
            witness.probability_denominator = loss_->total_mass();
            witness.secondary_probability_denominator = loss_->total_mass();
            witness.selected_option_hulls.assign(n, Label::bottom());
            witness.selected_option_costs.assign(n, 0U);
            witness.selected_option_indices.assign(n, -1);
            witness.selected_replica_leaves.assign(n, {});
            witness.selected_option_hulls[selected_node] =
                lattice_->hull_of_leaves(option.replica_leaves);
            witness.selected_option_costs[selected_node] = option.cost;
            witness.selected_option_indices[selected_node] =
                static_cast<std::int64_t>(option_index);
            witness.selected_replica_leaves[selected_node] = option.replica_leaves;
            witness.selected_total_cost = option.cost;
            witness.metrics = problem.metrics;

            std::vector<Label> path(n, Label::bottom());
            Score score = 0U;
            std::uint64_t count = 0U;
            for (const NodeId node_id : problem.order) {
                const std::size_t node = static_cast<std::size_t>(node_id);
                const NodeId parent = instance.parent[node];
                path[node] = parent < 0
                    ? witness.selected_option_hulls[node]
                    : lattice_->join(
                        path[static_cast<std::size_t>(parent)],
                        witness.selected_option_hulls[node]
                    );
                score += Score{instance.benefit[node]} *
                    (loss_->total_mass() - loss_->loss_mass(path[node]));
                if (!artificial_nodes[node] &&
                    witness.selected_option_indices[node] >= 0) {
                    ++count;
                }
            }
            if (count != 1U || witness.selected_total_cost > instance.budget) {
                throw std::logic_error("exact-one witness construction is inconsistent");
            }
            witness.objective_numerator = score;
            witness.secondary_objective_numerator = score;
            if (!exact_one_best ||
                witness.objective_numerator > exact_one_best->objective_numerator) {
                exact_one_best = std::move(witness);
            }
        }
    }
    certificate.exact_one_feasible = exact_one_best.has_value();
    if (exact_one_best) {
        certificate.exact_one_best_raw_score = exact_one_best->objective_numerator;
        certificate.exists_optimum_with_exactly_one_real_nonnull_image =
            exact_one_best->objective_numerator == certificate.best_raw_score;
        certificate.exact_one_deployment = std::move(exact_one_best);
    }
    certificate.minimum_count_deployment = std::move(minimum.deployment);
    certificate.maximum_count_deployment = std::move(maximum.deployment);
    return certificate;
}

DistinctScoreCertificate RecoveryOptimizer::certify_distinct_score_gap(
    const RecoveryInstance& instance
) const {
    require_recovery_score_bound(
        instance, loss_->total_mass(),
        "distinct-score certificate objective exceeds the exact score type"
    );
    const CertificateRecoveryProblem problem =
        prepare_certificate_recovery(instance, *lattice_);
    const std::size_t n = instance.parent.size();
    const std::size_t budget_count = static_cast<std::size_t>(instance.budget) + 1U;
    using DistinctRows = std::vector<std::vector<TwoDistinctScores>>;
    std::vector<DistinctRows> tables(n);
    RecoveryMetrics metrics = problem.metrics;

    for (auto iterator = problem.order.rbegin(); iterator != problem.order.rend();
         ++iterator) {
        const std::size_t node = static_cast<std::size_t>(*iterator);
        DistinctRows child_value(
            problem.produced[node].size(),
            std::vector<TwoDistinctScores>(budget_count)
        );
        for (std::vector<TwoDistinctScores>& row : child_value) {
            for (TwoDistinctScores& cell : row) {
                cell.insert(0U);
            }
        }
        for (const NodeId child_id : problem.children[node]) {
            const std::size_t child = static_cast<std::size_t>(child_id);
            DistinctRows next_value(
                problem.produced[node].size(),
                std::vector<TwoDistinctScores>(budget_count)
            );
            for (std::size_t state_index = 0;
                 state_index < problem.produced[node].size(); ++state_index) {
                const Label state = problem.produced[node][state_index];
                const std::size_t child_state =
                    problem.inherited_index[child].at(state);
                const std::vector<TwoDistinctScores>& child_row =
                    tables[child][child_state];
                for (std::size_t budget = 0; budget < budget_count; ++budget) {
                    TwoDistinctScores best;
                    for (std::size_t child_budget = 0; child_budget <= budget;
                         ++child_budget) {
                        const TwoDistinctScores& left =
                            child_value[state_index][budget - child_budget];
                        const TwoDistinctScores& right = child_row[child_budget];
                        for (std::size_t left_rank = 0; left_rank < left.size;
                             ++left_rank) {
                            for (std::size_t right_rank = 0; right_rank < right.size;
                                 ++right_rank) {
                                best.insert(
                                    left.score[left_rank] + right.score[right_rank]
                                );
                            }
                        }
                        ++metrics.budget_convolution_operations;
                    }
                    if (best.size == 0U) {
                        throw std::logic_error(
                            "distinct-score certificate convolution is empty"
                        );
                    }
                    next_value[state_index][budget] = best;
                }
            }
            child_value = std::move(next_value);
        }

        tables[node].assign(
            problem.inherited[node].size(),
            std::vector<TwoDistinctScores>(budget_count)
        );
        for (std::size_t state_index = 0;
             state_index < problem.inherited[node].size(); ++state_index) {
            const Label inherited_state = problem.inherited[node][state_index];
            for (std::size_t budget = 0; budget < budget_count; ++budget) {
                TwoDistinctScores best;
                for (const CertificateRecoveryOption& option : problem.menus[node]) {
                    if (option.cost > budget) {
                        continue;
                    }
                    const Label state = lattice_->join(inherited_state, option.hull);
                    const std::size_t produced_state =
                        problem.produced_index[node].at(state);
                    const Score local = Score{instance.benefit[node]} *
                        (loss_->total_mass() - loss_->loss_mass(state));
                    const TwoDistinctScores& descendants =
                        child_value[produced_state][budget - option.cost];
                    for (std::size_t rank = 0; rank < descendants.size; ++rank) {
                        best.insert(local + descendants.score[rank]);
                    }
                }
                if (best.size == 0U) {
                    throw std::logic_error(
                        "distinct-score certificate menu has no feasible option"
                    );
                }
                tables[node][state_index][budget] = best;
            }
        }
    }

    const std::size_t root_state =
        problem.inherited_index[static_cast<std::size_t>(problem.root)].at(
            Label::bottom()
        );
    const TwoDistinctScores root_scores =
        tables[static_cast<std::size_t>(problem.root)][root_state][instance.budget];
    if (root_scores.size == 0U) {
        throw std::logic_error("distinct-score certificate root is infeasible");
    }

    struct WitnessEntry {
        Score score = 0U;
        std::size_t previous_budget = 0U;
        std::size_t previous_rank = 0U;
        std::size_t child_budget = 0U;
        std::size_t child_rank = 0U;
        bool valid = false;
    };
    struct WitnessCell {
        std::array<WitnessEntry, 2U> entry;
        std::size_t size = 0U;
    };
    const auto insert_witness = [](WitnessCell& cell, WitnessEntry candidate) {
        for (std::size_t rank = 0; rank < cell.size; ++rank) {
            if (cell.entry[rank].score == candidate.score) {
                return;
            }
        }
        if (cell.size == 0U) {
            cell.entry[0] = candidate;
            cell.size = 1U;
            return;
        }
        if (candidate.score > cell.entry[0].score) {
            if (cell.size == 1U) {
                cell.size = 2U;
            }
            cell.entry[1] = cell.entry[0];
            cell.entry[0] = candidate;
            return;
        }
        if (cell.size == 1U || candidate.score > cell.entry[1].score) {
            cell.entry[1] = candidate;
            cell.size = 2U;
        }
    };

    const auto reconstruct_deployment = [&](Score requested_score) {
        RecoveryResult result;
        result.objective_numerator = requested_score;
        result.probability_denominator = loss_->total_mass();
        result.secondary_objective_numerator = requested_score;
        result.secondary_probability_denominator = loss_->total_mass();
        result.metrics = metrics;
        result.selected_option_hulls.assign(n, Label::bottom());
        result.selected_option_costs.assign(n, 0U);
        result.selected_option_indices.assign(n, -1);
        result.selected_replica_leaves.assign(n, {});

        std::function<void(NodeId, std::size_t, Label, Score)> reconstruct =
            [&](NodeId node_id, std::size_t budget, Label inherited_state,
                Score target) {
            const std::size_t node = static_cast<std::size_t>(node_id);
            const std::size_t state_index =
                problem.inherited_index[node].at(inherited_state);
            if (!tables[node][state_index][budget].rank_of(target).has_value()) {
                throw std::logic_error(
                    "distinct-score reconstruction target is not certified at its cell"
                );
            }
            for (const CertificateRecoveryOption& option : problem.menus[node]) {
                if (option.cost > budget) {
                    continue;
                }
                const Label produced_state = lattice_->join(inherited_state, option.hull);
                const Score local = Score{instance.benefit[node]} *
                    (loss_->total_mass() - loss_->loss_mass(produced_state));
                if (local > target) {
                    continue;
                }
                const Score descendant_target = target - local;
                const std::size_t remaining_budget = budget - option.cost;
                const std::size_t child_count = problem.children[node].size();
                std::vector<std::vector<WitnessCell>> prefix(
                    child_count + 1U,
                    std::vector<WitnessCell>(remaining_budget + 1U)
                );
                for (std::size_t available = 0; available <= remaining_budget;
                     ++available) {
                    prefix[0][available].entry[0] = WitnessEntry{
                        0U, available, 0U, 0U, 0U, true
                    };
                    prefix[0][available].size = 1U;
                }
                for (std::size_t child_index = 0; child_index < child_count;
                     ++child_index) {
                    const std::size_t child = static_cast<std::size_t>(
                        problem.children[node][child_index]
                    );
                    const std::size_t child_state =
                        problem.inherited_index[child].at(produced_state);
                    const std::vector<TwoDistinctScores>& child_row =
                        tables[child][child_state];
                    for (std::size_t available = 0; available <= remaining_budget;
                         ++available) {
                        WitnessCell& output = prefix[child_index + 1U][available];
                        for (std::size_t child_budget = 0;
                             child_budget <= available; ++child_budget) {
                            const std::size_t previous_budget =
                                available - child_budget;
                            const WitnessCell& previous =
                                prefix[child_index][previous_budget];
                            const TwoDistinctScores& child_scores =
                                child_row[child_budget];
                            for (std::size_t previous_rank = 0;
                                 previous_rank < previous.size; ++previous_rank) {
                                for (std::size_t child_rank = 0;
                                     child_rank < child_scores.size; ++child_rank) {
                                    insert_witness(output, WitnessEntry{
                                        previous.entry[previous_rank].score +
                                            child_scores.score[child_rank],
                                        previous_budget,
                                        previous_rank,
                                        child_budget,
                                        child_rank,
                                        true,
                                    });
                                }
                            }
                        }
                    }
                }
                const WitnessCell& final_cell = prefix[child_count][remaining_budget];
                std::optional<std::size_t> final_rank;
                for (std::size_t rank = 0; rank < final_cell.size; ++rank) {
                    if (final_cell.entry[rank].score == descendant_target) {
                        final_rank = rank;
                        break;
                    }
                }
                if (!final_rank.has_value()) {
                    continue;
                }

                result.selected_option_hulls[node] = option.hull;
                result.selected_option_costs[node] = option.cost;
                result.selected_option_indices[node] = option.original_index;
                result.selected_replica_leaves[node] = option.replica_leaves;
                result.selected_total_cost += option.cost;
                std::vector<std::size_t> child_budgets(child_count, 0U);
                std::vector<Score> child_targets(child_count, 0U);
                std::size_t available = remaining_budget;
                std::size_t rank = *final_rank;
                for (std::size_t reverse = child_count; reverse > 0U; --reverse) {
                    const WitnessEntry& witness = prefix[reverse][available].entry[rank];
                    if (!witness.valid) {
                        throw std::logic_error(
                            "distinct-score reconstruction encountered an invalid witness"
                        );
                    }
                    const std::size_t child = static_cast<std::size_t>(
                        problem.children[node][reverse - 1U]
                    );
                    const std::size_t child_state =
                        problem.inherited_index[child].at(produced_state);
                    const TwoDistinctScores& child_scores =
                        tables[child][child_state][witness.child_budget];
                    if (witness.child_rank >= child_scores.size) {
                        throw std::logic_error(
                            "distinct-score witness has an invalid child rank"
                        );
                    }
                    child_budgets[reverse - 1U] = witness.child_budget;
                    child_targets[reverse - 1U] =
                        child_scores.score[witness.child_rank];
                    available = witness.previous_budget;
                    rank = witness.previous_rank;
                }
                if (prefix[0][available].entry[rank].score != 0U) {
                    throw std::logic_error(
                        "distinct-score witness did not reduce to the empty prefix"
                    );
                }
                for (std::size_t child_index = 0; child_index < child_count;
                     ++child_index) {
                    reconstruct(
                        problem.children[node][child_index],
                        child_budgets[child_index], produced_state,
                        child_targets[child_index]
                    );
                }
                return;
            }
            throw std::logic_error(
                "distinct-score certified deployment cannot be reconstructed"
            );
        };
        reconstruct(problem.root, instance.budget, Label::bottom(), requested_score);
        if (result.selected_total_cost > instance.budget) {
            throw std::logic_error(
                "distinct-score reconstructed deployment exceeds its budget"
            );
        }
        std::vector<Label> path(n, Label::bottom());
        Score reconstructed_score = 0U;
        for (const NodeId node_id : problem.order) {
            const std::size_t node = static_cast<std::size_t>(node_id);
            const NodeId parent = instance.parent[node];
            path[node] = parent < 0
                ? result.selected_option_hulls[node]
                : lattice_->join(
                    path[static_cast<std::size_t>(parent)],
                    result.selected_option_hulls[node]
                );
            reconstructed_score += Score{instance.benefit[node]} *
                (loss_->total_mass() - loss_->loss_mass(path[node]));
            const std::int64_t original_index = result.selected_option_indices[node];
            if (original_index >= 0) {
                const RecoveryOption& original = instance.menus[node].at(
                    static_cast<std::size_t>(original_index)
                );
                if (original.cost != result.selected_option_costs[node] ||
                    original.replica_leaves != result.selected_replica_leaves[node]) {
                    throw std::logic_error(
                        "distinct-score reconstruction does not identify its original option"
                    );
                }
            }
        }
        if (reconstructed_score != requested_score) {
            throw std::logic_error(
                "distinct-score reconstruction does not attain its certified score"
            );
        }
        return result;
    };

    DistinctScoreCertificate certificate;
    certificate.best_raw_score = root_scores.score[0];
    certificate.event_mass = loss_->total_mass();
    for (const Benefit benefit : instance.benefit) {
        certificate.total_benefit += benefit;
    }
    certificate.normalization_denominator =
        Score{certificate.event_mass} * certificate.total_benefit;
    certificate.best_deployment = reconstruct_deployment(root_scores.score[0]);
    if (root_scores.size == 1U) {
        certificate.second_score_status =
            StrictSecondScoreStatus::all_feasible_scores_equal;
        return certificate;
    }

    certificate.second_score_status = StrictSecondScoreStatus::available;
    certificate.second_raw_score = root_scores.score[1];
    certificate.raw_gap = root_scores.score[0] - root_scores.score[1];
    if (*certificate.raw_gap == 0U || certificate.normalization_denominator == 0U) {
        throw std::logic_error(
            "a strict second score requires a positive raw gap and normalization"
        );
    }
    certificate.gamma = static_cast<long double>(*certificate.raw_gap) /
                        static_cast<long double>(certificate.normalization_denominator);
    certificate.second_deployment = reconstruct_deployment(root_scores.score[1]);
    return certificate;
}

DeterministicRng::DeterministicRng(std::uint64_t stream) noexcept : state_(stream) {}
std::uint64_t DeterministicRng::next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t value = state_;
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
}

std::uint64_t DeterministicRng::bounded(std::uint64_t bound) {
    if (bound == 0) {
        throw std::invalid_argument("bounded random request has zero bound");
    }
    const std::uint64_t threshold = static_cast<std::uint64_t>(-bound) % bound;
    while (true) {
        const std::uint64_t value = next();
        if (value >= threshold) {
            return value % bound;
        }
    }
}

namespace {

NodeId append_balanced(std::vector<NodeId>& parent, NodeId parent_node, std::size_t leaves) {
    const NodeId node = static_cast<NodeId>(parent.size());
    parent.push_back(parent_node);
    if (leaves > 1) {
        const std::size_t left = leaves / 2;
        append_balanced(parent, node, left);
        append_balanced(parent, node, leaves - left);
    }
    return node;
}

NodeId append_random_reduced(std::vector<NodeId>& parent,
                             NodeId parent_node,
                             std::size_t leaves,
                             std::uint32_t maximum_degree,
                             DeterministicRng& generator) {
    const NodeId node = static_cast<NodeId>(parent.size());
    parent.push_back(parent_node);
    if (leaves == 1) {
        return node;
    }
    const std::size_t maximum = std::min<std::size_t>(maximum_degree, leaves);
    const std::size_t degree = 2 + static_cast<std::size_t>(generator.bounded(maximum - 1));
    std::vector<std::size_t> parts(degree, 1);
    for (std::size_t remaining = leaves - degree; remaining > 0; --remaining) {
        ++parts[static_cast<std::size_t>(generator.bounded(degree))];
    }
    std::sort(parts.begin(), parts.end(), std::greater<>());
    for (const std::size_t part : parts) {
        append_random_reduced(parent, node, part, maximum_degree, generator);
    }
    return node;
}

std::vector<std::pair<NodeId, NodeId>> make_shared_fate_pairs(
    const ReducedTree& domain,
    std::uint64_t stream
) {
    if (domain.leaves().size() < 4U || domain.leaves().size() % 2U != 0U) {
        throw std::invalid_argument(
            "shared_fate_pairs requires an even number of at least four leaves"
        );
    }
    std::vector<NodeId> leaves = domain.leaves();
    DeterministicRng generator(stream);
    for (std::size_t remaining = leaves.size(); remaining > 1U; --remaining) {
        const std::size_t selected = static_cast<std::size_t>(generator.bounded(remaining));
        std::swap(leaves[remaining - 1U], leaves[selected]);
    }
    std::vector<std::pair<NodeId, NodeId>> pairs;
    pairs.reserve(leaves.size() / 2U);
    for (std::size_t index = 0; index < leaves.size(); index += 2U) {
        pairs.emplace_back(leaves[index], leaves[index + 1U]);
    }
    const LatticeOracle lattice(domain);
    for (std::size_t index = 0; index < pairs.size(); ++index) {
        if (lattice.normalize({pairs[index].first, pairs[index].second}).tag ==
            LabelTag::pair) {
            continue;
        }
        bool repaired = false;
        for (std::size_t partner = 0; partner < pairs.size(); ++partner) {
            if (partner == index) {
                continue;
            }
            const NodeId first_right = pairs[partner].second;
            const NodeId second_right = pairs[index].second;
            if (lattice.normalize({pairs[index].first, first_right}).tag == LabelTag::pair &&
                lattice.normalize({pairs[partner].first, second_right}).tag == LabelTag::pair) {
                std::swap(pairs[index].second, pairs[partner].second);
                repaired = true;
                break;
            }
        }
        if (!repaired) {
            throw std::invalid_argument(
                "shared_fate_pairs has no deterministic noncollapsing perfect matching"
            );
        }
    }
    for (const auto& pair : pairs) {
        if (lattice.normalize({pair.first, pair.second}).tag != LabelTag::pair) {
            throw std::logic_error("shared fate matching repair did not produce pair labels");
        }
    }
    return pairs;
}

std::vector<std::pair<NodeId, NodeId>> make_sparse_leaf_pairs(
    const ReducedTree& domain,
    std::uint64_t stream
) {
    std::vector<NodeId> leaves = domain.leaves();
    if (leaves.size() < 2U) {
        return {};
    }
    DeterministicRng generator(stream ^ 0x94D049BB133111EBULL);
    for (std::size_t remaining = leaves.size(); remaining > 1U; --remaining) {
        const std::size_t selected = static_cast<std::size_t>(generator.bounded(remaining));
        std::swap(leaves[remaining - 1U], leaves[selected]);
    }
    std::vector<std::pair<NodeId, NodeId>> pairs;
    if (leaves.size() == 2U) {
        pairs.emplace_back(leaves[0], leaves[1]);
        return pairs;
    }
    pairs.reserve(leaves.size());
    for (std::size_t index = 0; index < leaves.size(); ++index) {
        NodeId left = leaves[index];
        NodeId right = leaves[(index + 1U) % leaves.size()];
        if (right < left) {
            std::swap(left, right);
        }
        pairs.emplace_back(left, right);
    }
    std::sort(pairs.begin(), pairs.end());
    pairs.erase(std::unique(pairs.begin(), pairs.end()), pairs.end());
    return pairs;
}

NodeId random_descendant_leaf(const ReducedTree& domain,
                              NodeId start,
                              DeterministicRng& generator) {
    NodeId current = start;
    while (!domain.is_leaf(current)) {
        const auto& children = domain.children()[static_cast<std::size_t>(current)];
        current = children[static_cast<std::size_t>(generator.bounded(children.size()))];
    }
    return current;
}

}  // namespace

std::vector<NodeId> make_balanced_binary_tree(std::size_t leaves) {
    if (leaves == 0) {
        throw std::invalid_argument("a domain tree requires at least one leaf");
    }
    std::vector<NodeId> parent;
    parent.reserve(2 * leaves - 1);
    append_balanced(parent, -1, leaves);
    return parent;
}

std::vector<NodeId> make_star_tree(std::size_t leaves) {
    if (leaves == 0) {
        throw std::invalid_argument("a domain tree requires at least one leaf");
    }
    if (leaves == 1) {
        return {-1};
    }
    std::vector<NodeId> parent(leaves + 1, 0);
    parent[0] = -1;
    return parent;
}

std::vector<NodeId> make_random_reduced_tree(std::size_t leaves,
                                             std::uint32_t maximum_degree,
                                             std::uint64_t stream) {
    if (leaves == 0 || maximum_degree < 2) {
        throw std::invalid_argument("random reduced tree parameters are invalid");
    }
    std::vector<NodeId> parent;
    parent.reserve(2 * leaves - 1);
    DeterministicRng generator(stream);
    append_random_reduced(parent, -1, leaves, maximum_degree, generator);
    return parent;
}

std::vector<NodeId> make_mixed_degree_tree(std::size_t leaves, std::uint64_t stream) {
    return make_random_reduced_tree(leaves, 6, stream ^ 0xD1B54A32D192ED03ULL);
}

ImageCostCalibration make_image_cost_calibration(
    const std::vector<NodeId>& recovery_parent,
    const std::vector<std::uint64_t>& atomic_image_bytes,
    Cost cost_scale,
    bool artificial_root
) {
    if (recovery_parent.empty() || atomic_image_bytes.size() != recovery_parent.size() ||
        cost_scale == 0U) {
        throw std::invalid_argument("image cost calibration arrays or scale are invalid");
    }
    RecoveryInstance structure;
    structure.parent = recovery_parent;
    structure.benefit.assign(recovery_parent.size(), 0U);
    structure.menus.resize(recovery_parent.size());
    structure.budget = 0U;
    std::vector<std::vector<NodeId>> children;
    NodeId root = -1;
    const std::vector<NodeId> order = validate_recovery_tree(structure, children, root);
    if (artificial_root && atomic_image_bytes[static_cast<std::size_t>(root)] != 0U) {
        throw std::invalid_argument("artificial recovery root has positive atomic image bytes");
    }
    ImageCostCalibration result;
    result.subtree_image_bytes = atomic_image_bytes;
    result.cost_scale = cost_scale;
    Score reference = 0U;
    for (std::size_t node = 0; node < atomic_image_bytes.size(); ++node) {
        if (artificial_root && node == static_cast<std::size_t>(root)) continue;
        reference += atomic_image_bytes[node];
    }
    if (reference > std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("reference image bytes overflow uint64");
    }
    result.reference_image_bytes = static_cast<std::uint64_t>(reference);
    for (auto iterator = order.rbegin(); iterator != order.rend(); ++iterator) {
        const std::size_t node = static_cast<std::size_t>(*iterator);
        Score subtree = result.subtree_image_bytes[node];
        for (const NodeId child : children[node]) {
            subtree += result.subtree_image_bytes[static_cast<std::size_t>(child)];
        }
        if (subtree > std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("recovery subtree image bytes overflow uint64");
        }
        result.subtree_image_bytes[node] = static_cast<std::uint64_t>(subtree);
    }
    result.cost_unit_bytes = result.reference_image_bytes == 0U
        ? 1U
        : checked_u64(
            (Score{result.reference_image_bytes} + cost_scale - 1U) / cost_scale,
            "image cost unit overflow"
        );
    return result;
}

Cost quantize_image_cost(std::uint64_t image_bytes,
                         std::size_t replica_count,
                         std::uint64_t cost_unit_bytes) {
    if (cost_unit_bytes == 0U) {
        throw std::invalid_argument("image cost unit is zero");
    }
    if (image_bytes == 0U || replica_count == 0U) return 0U;
    const Score product = Score{image_bytes} * replica_count;
    const Score quantized = (product + cost_unit_bytes - 1U) / cost_unit_bytes;
    if (quantized == 0U || quantized > std::numeric_limits<Cost>::max()) {
        throw std::overflow_error("quantized image cost exceeds uint32");
    }
    return static_cast<Cost>(quantized);
}

RecoveryInstance make_recovery_instance(const ReducedTree& domain,
                                        std::size_t recovery_nodes,
                                        std::size_t menu_options,
                                        Cost budget,
                                        std::uint64_t stream) {
    return make_recovery_instance(
        domain, recovery_nodes, menu_options, budget, stream, MenuFamily::random, 64U
    );
}

RecoveryGenerationStreams derive_recovery_generation_streams(
    std::uint64_t base_stream
) noexcept {
    return {
        base_stream ^ 0x243F6A8885A308D3ULL,
        base_stream ^ 0x13198A2E03707344ULL,
        base_stream ^ 0xA4093822299F31D0ULL,
        base_stream ^ 0x082EFA98EC4E6C89ULL,
    };
}

RecoveryInstance make_recovery_instance(const ReducedTree& domain,
                                        std::size_t recovery_nodes,
                                        std::size_t menu_options,
                                        Cost budget,
                                        std::uint64_t stream,
                                        MenuFamily family,
                                        Cost menu_cost_scale) {
    return make_recovery_instance(
        domain, recovery_nodes, menu_options, budget,
        derive_recovery_generation_streams(stream), family, menu_cost_scale
    );
}

RecoveryInstance make_recovery_instance(const ReducedTree& domain,
                                        std::size_t recovery_nodes,
                                        std::size_t menu_options,
                                        Cost budget,
                                        const RecoveryGenerationStreams& streams,
                                        MenuFamily family,
                                        Cost menu_cost_scale) {
    if (recovery_nodes == 0) {
        throw std::invalid_argument("recovery tree must be nonempty");
    }
    if (menu_options == 0) {
        throw std::invalid_argument("recovery menus must be nonempty");
    }
    if (menu_cost_scale == 0U) {
        throw std::invalid_argument("recovery menu cost scale must be positive");
    }
    if (family == MenuFamily::shared_fate_pairs && menu_options < 2U) {
        throw std::invalid_argument(
            "shared_fate_pairs menus require at least two options"
        );
    }
    RecoveryInstance instance;
    instance.parent.resize(recovery_nodes, -1);
    instance.benefit.resize(recovery_nodes, 0);
    instance.menus.resize(recovery_nodes);
    instance.budget = budget;
    DeterministicRng topology_generator(streams.topology);
    for (std::size_t node = 1; node < recovery_nodes; ++node) {
        instance.parent[node] = static_cast<NodeId>(topology_generator.bounded(node));
    }
    DeterministicRng benefit_generator(streams.benefit);
    for (std::size_t node = 0; node < recovery_nodes; ++node) {
        instance.benefit[node] = 1U + benefit_generator.bounded(100U);
    }
    const ImageCostCalibration unit_cost = make_image_cost_calibration(
        instance.parent, std::vector<std::uint64_t>(recovery_nodes, 1U),
        menu_cost_scale, false
    );
    std::vector<std::pair<NodeId, NodeId>> fixed_pairs;
    if (family == MenuFamily::fixed_two_replica) {
        const LatticeOracle lattice(domain);
        for (std::size_t left = 0; left < domain.leaves().size(); ++left) {
            for (std::size_t right = left + 1U; right < domain.leaves().size(); ++right) {
                const NodeId first = domain.leaves()[left];
                const NodeId second = domain.leaves()[right];
                if (lattice.normalize({first, second}).tag == LabelTag::pair) {
                    fixed_pairs.emplace_back(first, second);
                }
            }
        }
        if (menu_options > fixed_pairs.size()) {
            throw std::invalid_argument(
                "fixed_two_replica requires enough proper normalized leaf pairs"
            );
        }
    }
    for (std::size_t node = 0; node < recovery_nodes; ++node) {
        const std::uint64_t node_stream = fnv1a_update(
            streams.menu, node
        );
        DeterministicRng menu_generator(node_stream);
        if (family == MenuFamily::random) {
            for (std::size_t option_index = 0; option_index < menu_options; ++option_index) {
                RecoveryOption option;
                const std::size_t replica_count = 1U + static_cast<std::size_t>(
                    menu_generator.bounded(
                        std::min<std::size_t>(4U, domain.leaves().size())
                    )
                );
                while (option.replica_leaves.size() < replica_count) {
                    const NodeId leaf = domain.leaves()[static_cast<std::size_t>(
                        menu_generator.bounded(domain.leaves().size())
                    )];
                    if (std::find(option.replica_leaves.begin(),
                                  option.replica_leaves.end(),
                                  leaf) == option.replica_leaves.end()) {
                        option.replica_leaves.push_back(leaf);
                    }
                }
                std::sort(option.replica_leaves.begin(), option.replica_leaves.end());
                option.cost = quantize_image_cost(
                    unit_cost.subtree_image_bytes[node], replica_count,
                    unit_cost.cost_unit_bytes
                );
                instance.menus[node].push_back(std::move(option));
            }
        } else if (family == MenuFamily::fixed_two_replica) {
            const Cost cost = quantize_image_cost(
                unit_cost.subtree_image_bytes[node], 2U, unit_cost.cost_unit_bytes
            );
            std::set<std::size_t> selected;
            while (selected.size() < menu_options) {
                const std::size_t pair_index = static_cast<std::size_t>(
                    menu_generator.bounded(fixed_pairs.size())
                );
                if (selected.insert(pair_index).second) {
                    const auto& pair = fixed_pairs[pair_index];
                    instance.menus[node].push_back(
                        RecoveryOption{cost, {pair.first, pair.second}}
                    );
                }
            }
        }
    }
    if (family == MenuFamily::shared_fate_pairs) {
        const auto pairs = make_shared_fate_pairs(domain, streams.menu);
        for (std::size_t node = 0; node < recovery_nodes; ++node) {
            DeterministicRng menu_generator(fnv1a_update(
                streams.menu ^ 0x452821E638D01377ULL, node
            ));
            const std::size_t own = static_cast<std::size_t>(
                menu_generator.bounded(pairs.size())
            );
            const std::size_t other = (own + 1U + static_cast<std::size_t>(
                menu_generator.bounded(pairs.size() - 1U)
            )) % pairs.size();
            instance.menus[node].push_back(
                RecoveryOption{0U, {pairs[own].first, pairs[own].second}}
            );
            instance.menus[node].push_back(
                RecoveryOption{1U, {pairs[own].first, pairs[other].second}}
            );
            for (std::size_t option_index = 2U; option_index < menu_options; ++option_index) {
                const std::size_t alternative = (own + option_index) % pairs.size();
                const std::uint64_t raw_cost = option_index;
                const Cost cost = raw_cost > std::numeric_limits<Cost>::max()
                    ? std::numeric_limits<Cost>::max()
                    : static_cast<Cost>(raw_cost);
                instance.menus[node].push_back(
                    RecoveryOption{cost, {pairs[own].first, pairs[alternative].second}}
                );
            }
        }
    }
    return instance;
}

ImageSizeMenuResult make_image_size_calibrated_menus(
    const ReducedTree& domain,
    const std::vector<NodeId>& recovery_parent,
    const std::vector<std::uint64_t>& fragment_output_bytes,
    std::size_t menu_options,
    Cost cost_scale,
    std::uint64_t stream,
    bool null_only_root
) {
    if (recovery_parent.empty() || fragment_output_bytes.size() != recovery_parent.size()) {
        throw std::invalid_argument("image-size menu arrays must be nonempty and have equal length");
    }
    if (menu_options == 0U || cost_scale == 0U) {
        throw std::invalid_argument("image-size menus require options and a positive cost scale");
    }
    const ImageCostCalibration calibration = make_image_cost_calibration(
        recovery_parent, fragment_output_bytes, cost_scale, null_only_root
    );
    const auto root_iterator = std::find(recovery_parent.begin(), recovery_parent.end(), -1);
    if (root_iterator == recovery_parent.end()) {
        throw std::logic_error("validated recovery tree has no root");
    }
    const std::size_t root = static_cast<std::size_t>(
        root_iterator - recovery_parent.begin()
    );

    ImageSizeMenuResult result;
    result.menus.resize(recovery_parent.size());
    result.subtree_image_bytes = calibration.subtree_image_bytes;
    result.reference_image_bytes = calibration.reference_image_bytes;
    result.cost_unit_bytes = calibration.cost_unit_bytes;
    result.cost_scale = calibration.cost_scale;

    const std::size_t maximum_replicas = std::min<std::size_t>(4U, domain.leaves().size());
    for (std::size_t node = 0; node < recovery_parent.size(); ++node) {
        if (null_only_root && node == root) {
            continue;
        }
        if (result.subtree_image_bytes[node] == 0U) {
            continue;
        }
        DeterministicRng generator(fnv1a_update(
            stream ^ 0x4F1BBCDCBFA54001ULL, node
        ));
        for (std::size_t option_index = 0; option_index < menu_options; ++option_index) {
            RecoveryOption option;
            const std::size_t replica_count = 1U + static_cast<std::size_t>(
                generator.bounded(maximum_replicas)
            );
            while (option.replica_leaves.size() < replica_count) {
                const NodeId leaf = domain.leaves()[static_cast<std::size_t>(
                    generator.bounded(domain.leaves().size())
                )];
                if (std::find(option.replica_leaves.begin(), option.replica_leaves.end(), leaf) ==
                    option.replica_leaves.end()) {
                    option.replica_leaves.push_back(leaf);
                }
            }
            std::sort(option.replica_leaves.begin(), option.replica_leaves.end());
            option.cost = quantize_image_cost(
                result.subtree_image_bytes[node], replica_count, result.cost_unit_bytes
            );
            result.menus[node].push_back(std::move(option));
        }
    }
    return result;
}

std::vector<RawEvent> make_raw_events(const ReducedTree& domain,
                                      std::size_t random_pair_count,
                                      std::uint64_t stream) {
    return make_raw_events(
        domain, random_pair_count, stream, MassFamily::mixed_reference
    );
}

std::vector<RawEvent> make_raw_events(const ReducedTree& domain,
                                      std::size_t random_pair_count,
                                      std::uint64_t stream,
                                      MassFamily family) {
    DeterministicRng generator(stream);
    std::vector<RawEvent> events;
    if (family == MassFamily::shared_fate_pairs) {
        const auto pairs = make_shared_fate_pairs(domain, stream);
        events.push_back({{}, 1U});
        for (const NodeId leaf : domain.leaves()) {
            events.push_back({{leaf}, 1U});
        }
        for (const auto& pair : pairs) {
            events.push_back({{pair.first, pair.second}, 128U});
        }
        return events;
    }

    if (family == MassFamily::sparse_pairs || family == MassFamily::dense_pairs) {
        events.push_back({{}, 1U + generator.bounded(17U)});
        for (std::size_t node = 0; node < domain.size(); ++node) {
            events.push_back({
                {static_cast<NodeId>(node)},
                1U + generator.bounded(31U),
            });
        }
        if (family == MassFamily::sparse_pairs) {
            for (const auto& pair : make_sparse_leaf_pairs(domain, stream)) {
                events.push_back({
                    {pair.first, pair.second},
                    1U + generator.bounded(37U),
                });
            }
        } else {
            const auto& leaves = domain.leaves();
            for (std::size_t left = 0; left < leaves.size(); ++left) {
                for (std::size_t right = left + 1U; right < leaves.size(); ++right) {
                    events.push_back({
                        {leaves[left], leaves[right]},
                        1U + generator.bounded(37U),
                    });
                }
            }
        }
        return events;
    }

    const bool level_decay = family == MassFamily::level_decay;
    events.push_back({{}, level_decay ? 256U : 1U + generator.bounded(17U)});
    for (std::size_t node = 0; node < domain.size(); ++node) {
        const std::uint64_t mass = level_decay
            ? 1U + 256U / (1U + domain.depth()[node])
            : 1U + generator.bounded(31U);
        events.push_back({{static_cast<NodeId>(node)}, mass});
    }
    for (std::size_t node = 0; node < domain.size(); ++node) {
        const NodeId parent = domain.parent()[node];
        if (parent >= 0) {
            const std::uint64_t mass = level_decay
                ? 1U + 256U / (1U + domain.depth()[node])
                : 1U + generator.bounded(23U);
            events.push_back({{parent, static_cast<NodeId>(node)}, mass});
        }
        const auto& children = domain.children()[node];
        if (children.size() == 2) {
            const std::uint64_t mass = level_decay
                ? 1U + 256U / (2U + domain.depth()[node])
                : 1U + generator.bounded(29U);
            events.push_back({{children[0], children[1]}, mass});
        }
    }

    if (family == MassFamily::same_level_locality) {
        const std::uint32_t maximum_depth = *std::max_element(
            domain.depth().begin(), domain.depth().end()
        );
        std::vector<std::vector<NodeId>> levels(
            static_cast<std::size_t>(maximum_depth) + 1U
        );
        for (std::size_t node = 0; node < domain.size(); ++node) {
            levels[domain.depth()[node]].push_back(static_cast<NodeId>(node));
        }
        std::vector<std::size_t> eligible;
        for (std::size_t depth = 0; depth < levels.size(); ++depth) {
            if (levels[depth].size() >= 2U) {
                eligible.push_back(depth);
            }
        }
        if (eligible.empty()) {
            throw std::invalid_argument("same_level_locality requires a nontrivial tree");
        }
        for (std::size_t index = 0; index < random_pair_count; ++index) {
            const auto& nodes = levels[eligible[static_cast<std::size_t>(
                generator.bounded(eligible.size())
            )]];
            const std::size_t left_index = static_cast<std::size_t>(
                generator.bounded(nodes.size())
            );
            std::size_t right_index = static_cast<std::size_t>(
                generator.bounded(nodes.size() - 1U)
            );
            if (right_index >= left_index) {
                ++right_index;
            }
            events.push_back({
                {nodes[left_index], nodes[right_index]},
                1U + generator.bounded(37U),
            });
        }
    } else if (family == MassFamily::cross_branch_locality) {
        std::vector<NodeId> branching;
        for (std::size_t node = 0; node < domain.size(); ++node) {
            if (domain.children()[node].size() >= 2U) {
                branching.push_back(static_cast<NodeId>(node));
            }
        }
        if (branching.empty()) {
            throw std::invalid_argument("cross_branch_locality requires a branching tree");
        }
        for (std::size_t index = 0; index < random_pair_count; ++index) {
            const NodeId pivot = branching[static_cast<std::size_t>(
                generator.bounded(branching.size())
            )];
            const auto& children = domain.children()[static_cast<std::size_t>(pivot)];
            const std::size_t left_branch = static_cast<std::size_t>(
                generator.bounded(children.size())
            );
            std::size_t right_branch = static_cast<std::size_t>(
                generator.bounded(children.size() - 1U)
            );
            if (right_branch >= left_branch) {
                ++right_branch;
            }
            events.push_back({
                {
                    random_descendant_leaf(domain, children[left_branch], generator),
                    random_descendant_leaf(domain, children[right_branch], generator),
                },
                1U + generator.bounded(37U),
            });
        }
    } else if (domain.size() >= 2U) {
        for (std::size_t index = 0; index < random_pair_count; ++index) {
            NodeId left = static_cast<NodeId>(generator.bounded(domain.size()));
            NodeId right = static_cast<NodeId>(generator.bounded(domain.size() - 1U));
            if (right >= left) {
                ++right;
            }
            const std::uint64_t mass = level_decay
                ? 1U + 256U / (1U + std::max(
                    domain.depth()[static_cast<std::size_t>(left)],
                    domain.depth()[static_cast<std::size_t>(right)]
                ))
                : 1U + generator.bounded(37U);
            events.push_back({{left, right}, mass});
        }
    }
    return events;
}

std::uint64_t fnv1a_update(std::uint64_t hash, std::uint64_t value) noexcept {
    for (unsigned byte = 0; byte < 8; ++byte) {
        hash ^= (value >> (byte * 8U)) & 0xFFU;
        hash *= kFnvPrime;
    }
    return hash;
}

std::string json_escape(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (character < 0x20U) {
                    output << "\\u00";
                    constexpr char digits[] = "0123456789abcdef";
                    output << digits[(character >> 4U) & 0x0FU] << digits[character & 0x0FU];
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

void atomic_write_text(const std::filesystem::path& path, const std::string& text) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot open temporary output file");
        }
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error("cannot finish temporary output file");
        }
    }
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("cannot atomically replace output file");
    }
#else
    std::filesystem::rename(temporary, path);
#endif
}

}  // namespace pfl
