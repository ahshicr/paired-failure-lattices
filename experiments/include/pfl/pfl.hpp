#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pfl {

using NodeId = std::int32_t;
using Cost = std::uint32_t;
using Mass = std::uint64_t;
using Benefit = std::uint64_t;
#if defined(__SIZEOF_INT128__)
__extension__ typedef unsigned __int128 Score;
#else
#error "The exact recovery score type requires compiler support for unsigned 128-bit integers."
#endif

std::string score_to_string(Score value);

class PlusMinusOneRMQ;

class ReducedTree {
public:
    explicit ReducedTree(std::vector<NodeId> parent);
    ~ReducedTree();
    ReducedTree(ReducedTree&&) noexcept;
    ReducedTree& operator=(ReducedTree&&) noexcept;
    ReducedTree(const ReducedTree&) = delete;
    ReducedTree& operator=(const ReducedTree&) = delete;

    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] NodeId root() const noexcept;
    [[nodiscard]] const std::vector<NodeId>& parent() const noexcept;
    [[nodiscard]] const std::vector<std::vector<NodeId>>& children() const noexcept;
    [[nodiscard]] const std::vector<std::uint32_t>& depth() const noexcept;
    [[nodiscard]] const std::vector<NodeId>& leaves() const noexcept;
    [[nodiscard]] bool is_leaf(NodeId node) const;
    [[nodiscard]] bool is_ancestor(NodeId ancestor, NodeId node) const;
    [[nodiscard]] NodeId lca(NodeId left, NodeId right) const;
    [[nodiscard]] NodeId lca_many(const std::vector<NodeId>& nodes) const;
    [[nodiscard]] std::size_t structural_bytes() const noexcept;

private:
    void require_node(NodeId node) const;

    std::vector<NodeId> parent_;
    std::vector<std::vector<NodeId>> children_;
    std::vector<std::uint32_t> depth_;
    std::vector<std::uint64_t> tin_;
    std::vector<std::uint64_t> tout_;
    std::vector<std::size_t> first_;
    std::vector<NodeId> euler_;
    std::vector<std::int32_t> euler_depth_;
    std::vector<NodeId> leaves_;
    NodeId root_ = -1;
    std::unique_ptr<PlusMinusOneRMQ> rmq_;
};

enum class LabelTag : std::uint8_t {
    bottom = 0,
    single = 1,
    pair = 2,
    invalid = 255,
};

struct Label {
    LabelTag tag = LabelTag::invalid;
    NodeId first = -1;
    NodeId second = -1;

    [[nodiscard]] static Label bottom() noexcept;
    [[nodiscard]] static Label single(NodeId node) noexcept;
    [[nodiscard]] static Label pair(NodeId first, NodeId second) noexcept;
    [[nodiscard]] static Label invalid() noexcept;

    friend bool operator==(const Label& left, const Label& right) noexcept {
        return left.tag == right.tag && left.first == right.first &&
               left.second == right.second;
    }
    friend bool operator!=(const Label& left, const Label& right) noexcept {
        return !(left == right);
    }
    friend bool operator<(const Label& left, const Label& right) noexcept;
};

struct LabelHash {
    [[nodiscard]] std::size_t operator()(const Label& label) const noexcept;
};

struct StateCount {
    std::uint64_t nodes = 0;
    std::uint64_t leaves = 0;
    std::uint64_t internal_nodes = 0;
    std::uint64_t binary_internal_nodes = 0;
    std::uint64_t high_degree_internal_nodes = 0;
    std::uint64_t incomparable_pairs = 0;
    std::uint64_t nonempty_states = 0;
    std::uint64_t states_with_bottom = 0;
    std::uint64_t lower_envelope = 0;
    std::uint64_t upper_envelope = 0;
};

class LatticeOracle {
public:
    explicit LatticeOracle(const ReducedTree& tree) noexcept;

    [[nodiscard]] const ReducedTree& tree() const noexcept;
    [[nodiscard]] bool valid(const Label& label) const noexcept;
    [[nodiscard]] std::optional<bool> equal(const Label& left, const Label& right) const noexcept;
    [[nodiscard]] std::optional<bool> order(const Label& left, const Label& right) const noexcept;
    [[nodiscard]] Label normalize(const std::vector<NodeId>& nodes) const noexcept;
    [[nodiscard]] Label meet(const Label& left, const Label& right) const;
    [[nodiscard]] Label join(const Label& left, const Label& right) const;
    [[nodiscard]] Label hull_of_leaves(const std::vector<NodeId>& leaves) const;
    [[nodiscard]] std::vector<NodeId> components(const Label& label) const;
    [[nodiscard]] StateCount exact_state_count() const;

private:
    [[nodiscard]] std::vector<NodeId> reduce_components(const std::vector<NodeId>& nodes) const;
    [[nodiscard]] bool component_subset(const Label& left, const Label& right) const;

    const ReducedTree* tree_;
};

class ExplicitOracle {
public:
    ExplicitOracle(const ReducedTree& tree, const LatticeOracle& lattice);

    [[nodiscard]] std::uint64_t node_mask(NodeId node) const;
    [[nodiscard]] std::uint64_t label_mask(const Label& label) const;
    [[nodiscard]] Label mask_label(std::uint64_t mask) const;
    [[nodiscard]] Label brute_hull(std::uint64_t placement_mask) const;
    [[nodiscard]] Label brute_join(const Label& left, const Label& right) const;
    [[nodiscard]] Label brute_meet(const Label& left, const Label& right) const;
    [[nodiscard]] const std::vector<Label>& labels() const noexcept;
    [[nodiscard]] const std::vector<std::uint64_t>& masks() const noexcept;
    [[nodiscard]] std::size_t storage_bytes() const noexcept;

private:
    const ReducedTree* tree_;
    const LatticeOracle* lattice_;
    std::vector<std::uint64_t> node_masks_;
    std::vector<Label> labels_;
    std::vector<std::uint64_t> masks_;
    std::unordered_map<std::uint64_t, Label> label_by_mask_;
};

struct RawEvent {
    std::vector<NodeId> domains;
    Mass mass = 0;
};

struct AggregatedMasses {
    std::unordered_map<Label, Mass, LabelHash> by_region;
    Mass total_mass = 0;
    std::uint64_t raw_identity_count = 0;
};

struct MaximumEntropyPairedResult {
    std::vector<RawEvent> events;
    std::vector<Mass> source_occurrence_mass;
    std::vector<Mass> realized_occurrence_mass;
    Mass source_total_mass = 0;
    Mass quantized_total_mass = 0;
    std::uint64_t iterations = 0;
    long double maximum_fit_error = 0;
    long double maximum_quantized_marginal_error = 0;
    long double certified_marginal_error_bound = 0;
};

struct FourCycleDecompositionAudit {
    NodeId pivot = -1;
    NodeId left_child = -1;
    NodeId right_child = -1;
    NodeId short_child = -1;
    NodeId long_child = -1;
    std::size_t padded_size = 0;
    std::size_t real_edges_per_shift = 0;
    std::size_t candidates_per_shift = 0;
    std::size_t odd_edges_per_shift = 0;
    std::vector<NodeId> permuted_short;
    std::vector<NodeId> permuted_long;
};

struct FourCycleSwitchAudit {
    std::uint64_t order = 0;
    std::size_t decomposition_index = 0;
    std::size_t cyclic_shift = 0;
    std::size_t matching_position = 0;
    std::uint64_t stream_tie_key = 0;
    std::array<NodeId, 4> quartet{{-1, -1, -1, -1}};
    std::uint64_t source_locality_depth_sum = 0;
    std::uint64_t target_locality_depth_sum = 0;
    Mass pre_source_first = 0;
    Mass pre_source_second = 0;
    Mass moved_mass = 0;
};

struct FourCycleEdgeDifference {
    NodeId left = -1;
    NodeId right = -1;
    Mass baseline_mass = 0;
    Mass endpoint_mass = 0;
    Mass absolute_difference = 0;
    bool increased = false;
};

struct FourCycleCorrelationPath {
    std::vector<NodeId> topology_parent;
    std::vector<NodeId> topology_leaves;
    std::vector<RawEvent> baseline_events;
    std::vector<RawEvent> endpoint_events;
    std::vector<FourCycleDecompositionAudit> decompositions;
    std::vector<FourCycleSwitchAudit> switches;
    std::vector<FourCycleEdgeDifference> edge_differences;
    std::vector<Mass> baseline_occurrence_mass;
    std::vector<Mass> endpoint_occurrence_mass;
    Mass total_mass = 0;
    Mass baseline_pair_mass = 0;
    Score total_switch_mass = 0;
    std::uint64_t positive_switches = 0;
    std::uint64_t zero_capacity_switches = 0;
    Score endpoint_pair_l1_divergence = 0;
    Score baseline_locality_depth_mass = 0;
    Score endpoint_locality_depth_mass = 0;
    std::uint64_t maximum_depth = 0;
    std::uint64_t rewiring_stream = 0;
    std::uint64_t degree_hash = 0;
    std::uint64_t topology_hash = 0;
    std::uint64_t leaf_hash = 0;
    std::uint64_t path_hash = 0;
    std::uint64_t semantic_pair_edge_count = 0;
    std::uint64_t semantic_collapsed_edge_count = 0;
};

[[nodiscard]] std::uint64_t four_cycle_path_hash(
    const FourCycleCorrelationPath& path
) noexcept;

struct FourCycleCorrelationLevel {
    std::vector<RawEvent> events;
    std::vector<Mass> occurrence_mass;
    Mass total_mass = 0;
    Mass pair_mass = 0;
    Score locality_depth_mass = 0;
    Score pair_l1_divergence = 0;
    std::uint64_t rho_quarter = 0;
    std::uint64_t degree_hash = 0;
    std::uint64_t path_hash = 0;
};

[[nodiscard]] MaximumEntropyPairedResult fit_maximum_entropy_paired_events(
    std::size_t nodes,
    const std::vector<RawEvent>& observed,
    Mass quantized_total_mass,
    long double tolerance = 1e-14L,
    std::uint64_t maximum_iterations = 100000U
);
[[nodiscard]] FourCycleCorrelationPath make_four_cycle_correlation_path(
    const LatticeOracle& lattice,
    const std::vector<RawEvent>& baseline,
    std::uint64_t rewiring_stream
);
[[nodiscard]] FourCycleCorrelationLevel materialize_four_cycle_correlation_level(
    const LatticeOracle& lattice,
    const FourCycleCorrelationPath& path,
    std::uint64_t rho_quarter
);

[[nodiscard]] AggregatedMasses aggregate_raw_events(
    const LatticeOracle& lattice,
    const std::vector<RawEvent>& events
);
[[nodiscard]] AggregatedMasses single_domain_envelope_masses(
    const LatticeOracle& lattice,
    const AggregatedMasses& masses
);

class DenseLossMassCompiler {
public:
    DenseLossMassCompiler(const LatticeOracle& lattice, const AggregatedMasses& masses);

    [[nodiscard]] Mass loss_mass(const Label& state) const;
    [[nodiscard]] Mass total_mass() const noexcept;
    [[nodiscard]] std::size_t storage_bytes() const noexcept;
    [[nodiscard]] const LatticeOracle& lattice() const noexcept;

private:
    const LatticeOracle* lattice_;
    std::size_t n_ = 0;
    Mass total_mass_ = 0;
    std::vector<Mass> chain_mass_;
    std::vector<Mass> cross_mass_;
};

struct RecoveryOption {
    Cost cost = 0;
    std::vector<NodeId> replica_leaves;
};

struct RecoveryInstance {
    std::vector<NodeId> parent;
    std::vector<Benefit> benefit;
    std::vector<std::vector<RecoveryOption>> menus;
    Cost budget = 0;
};

struct RecoveryMetrics {
    std::vector<std::uint64_t> inherited_state_count;
    std::vector<std::uint64_t> produced_state_count;
    std::uint64_t compressed_option_count = 0;
    std::uint64_t state_option_transitions = 0;
    std::uint64_t score_cells = 0;
    std::uint64_t budget_convolution_operations = 0;
};

struct RecoveryResult {
    Score objective_numerator = 0;
    Mass probability_denominator = 0;
    Score secondary_objective_numerator = 0;
    Mass secondary_probability_denominator = 0;
    RecoveryMetrics metrics;
    std::vector<Label> selected_option_hulls;
    std::vector<Cost> selected_option_costs;
    std::vector<std::int64_t> selected_option_indices;
    std::vector<std::vector<NodeId>> selected_replica_leaves;
    std::uint64_t selected_total_cost = 0;
};

enum class SecondaryTiePolicy : std::uint8_t {
    optimistic = 0,
    pessimistic = 1,
};

struct OptimalImageCountCertificate {
    Score best_raw_score = 0;
    std::uint64_t minimum_real_nonnull_images = 0;
    std::uint64_t maximum_real_nonnull_images = 0;
    bool exact_one_feasible = false;
    bool exists_optimum_with_exactly_one_real_nonnull_image = false;
    std::optional<Score> exact_one_best_raw_score;
    std::optional<RecoveryResult> exact_one_deployment;
    RecoveryResult minimum_count_deployment;
    RecoveryResult maximum_count_deployment;
};

enum class StrictSecondScoreStatus : std::uint8_t {
    available = 0,
    all_feasible_scores_equal = 1,
};

struct DistinctScoreCertificate {
    StrictSecondScoreStatus second_score_status =
        StrictSecondScoreStatus::all_feasible_scores_equal;
    Score best_raw_score = 0;
    std::optional<Score> second_raw_score;
    std::optional<Score> raw_gap;
    Mass event_mass = 0;
    Score total_benefit = 0;
    Score normalization_denominator = 0;
    std::optional<long double> gamma;
    RecoveryResult best_deployment;
    std::optional<RecoveryResult> second_deployment;
};

class RecoveryOptimizer {
public:
    RecoveryOptimizer(const LatticeOracle& lattice, const DenseLossMassCompiler& loss) noexcept;
    [[nodiscard]] RecoveryResult solve(const RecoveryInstance& instance) const;
    [[nodiscard]] RecoveryResult solve_lexicographic(
        const RecoveryInstance& instance,
        const DenseLossMassCompiler& secondary_loss,
        SecondaryTiePolicy policy
    ) const;
    [[nodiscard]] OptimalImageCountCertificate certify_optimal_image_count(
        const RecoveryInstance& instance,
        const std::vector<bool>& artificial_nodes
    ) const;
    [[nodiscard]] DistinctScoreCertificate certify_distinct_score_gap(
        const RecoveryInstance& instance
    ) const;

private:
    const LatticeOracle* lattice_;
    const DenseLossMassCompiler* loss_;
};

class DeterministicRng {
public:
    explicit DeterministicRng(std::uint64_t stream) noexcept;
    [[nodiscard]] std::uint64_t next() noexcept;
    [[nodiscard]] std::uint64_t bounded(std::uint64_t bound);

private:
    std::uint64_t state_;
};

enum class MassFamily : std::uint8_t {
    mixed_reference = 0,
    sparse_pairs = 1,
    dense_pairs = 2,
    level_decay = 3,
    same_level_locality = 4,
    cross_branch_locality = 5,
    shared_fate_pairs = 6,
    four_cycle_correlation = 7,
};

enum class MenuFamily : std::uint8_t {
    random = 0,
    shared_fate_pairs = 1,
    image_size_calibrated = 2,
    fixed_two_replica = 3,
};

struct ImageSizeMenuResult {
    std::vector<std::vector<RecoveryOption>> menus;
    std::vector<std::uint64_t> subtree_image_bytes;
    std::uint64_t reference_image_bytes = 0;
    std::uint64_t cost_unit_bytes = 0;
    Cost cost_scale = 0;
};

struct ImageCostCalibration {
    std::vector<std::uint64_t> subtree_image_bytes;
    std::uint64_t reference_image_bytes = 0;
    std::uint64_t cost_unit_bytes = 0;
    Cost cost_scale = 0;
};

struct RecoveryGenerationStreams {
    std::uint64_t topology = 0;
    std::uint64_t benefit = 0;
    std::uint64_t state_size = 0;
    std::uint64_t menu = 0;
};

[[nodiscard]] RecoveryGenerationStreams derive_recovery_generation_streams(
    std::uint64_t base_stream
) noexcept;

[[nodiscard]] std::vector<NodeId> make_balanced_binary_tree(std::size_t leaves);
[[nodiscard]] std::vector<NodeId> make_star_tree(std::size_t leaves);
[[nodiscard]] std::vector<NodeId> make_mixed_degree_tree(std::size_t leaves, std::uint64_t stream);
[[nodiscard]] std::vector<NodeId> make_random_reduced_tree(
    std::size_t leaves,
    std::uint32_t maximum_degree,
    std::uint64_t stream
);
[[nodiscard]] RecoveryInstance make_recovery_instance(
    const ReducedTree& domain,
    std::size_t recovery_nodes,
    std::size_t menu_options,
    Cost budget,
    std::uint64_t stream
);
[[nodiscard]] RecoveryInstance make_recovery_instance(
    const ReducedTree& domain,
    std::size_t recovery_nodes,
    std::size_t menu_options,
    Cost budget,
    std::uint64_t stream,
    MenuFamily family,
    Cost menu_cost_scale = 64U
);
[[nodiscard]] RecoveryInstance make_recovery_instance(
    const ReducedTree& domain,
    std::size_t recovery_nodes,
    std::size_t menu_options,
    Cost budget,
    const RecoveryGenerationStreams& streams,
    MenuFamily family,
    Cost menu_cost_scale = 64U
);
[[nodiscard]] ImageCostCalibration make_image_cost_calibration(
    const std::vector<NodeId>& recovery_parent,
    const std::vector<std::uint64_t>& atomic_image_bytes,
    Cost cost_scale,
    bool artificial_root
);
[[nodiscard]] Cost quantize_image_cost(
    std::uint64_t image_bytes,
    std::size_t replica_count,
    std::uint64_t cost_unit_bytes
);
[[nodiscard]] ImageSizeMenuResult make_image_size_calibrated_menus(
    const ReducedTree& domain,
    const std::vector<NodeId>& recovery_parent,
    const std::vector<std::uint64_t>& fragment_output_bytes,
    std::size_t menu_options,
    Cost cost_scale,
    std::uint64_t stream,
    bool null_only_root
);
[[nodiscard]] std::vector<RawEvent> make_raw_events(
    const ReducedTree& domain,
    std::size_t random_pair_count,
    std::uint64_t stream
);
[[nodiscard]] std::vector<RawEvent> make_raw_events(
    const ReducedTree& domain,
    std::size_t random_pair_count,
    std::uint64_t stream,
    MassFamily family
);

[[nodiscard]] std::uint64_t fnv1a_update(std::uint64_t hash, std::uint64_t value) noexcept;
[[nodiscard]] std::string json_escape(const std::string& value);
void atomic_write_text(const std::filesystem::path& path, const std::string& text);

}  // namespace pfl
