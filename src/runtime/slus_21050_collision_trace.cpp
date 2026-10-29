// RECOVERY: not identical to the original object: Probe::finish_sample and Trace::finish_sample (block layout), capture_slice clones (call-site counts), write_summary/observe_summary/slice_float/__uninit_default_n (inlined here), write_sample, maybe_pin, ctor, dtor, Trace::begin_sample, finalize, capture_world_contact_snapshot.part.0, find_target (PHI order).
#include "takedown/runtime/slus_21050_collision_trace.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace takedown::runtime {
namespace {

using guest::EeMemory;
using guest::GuestAddress;

// One instruction the trace observes: the retail word expected at the PC, a
// label for the TSV, and the FPU operands / comparison kind for the compare
// targets (comparison: 0 none, 1 less-than, 2 less-or-equal).
enum class Comparison : std::uint8_t { kNone = 0, kLessThan = 1, kLessEqual = 2 };

struct Target {
    GuestAddress pc{};
    std::uint32_t word{};
    const char* label{};
    std::uint8_t fs{};
    std::uint8_t ft{};
    Comparison comparison{};
};

constexpr std::array<Target, 29> kTargets{{
    {0x0019A660U, 0x0320F809U, "object_physics_dispatch", 0, 0, Comparison::kNone},
    {0x00143CB8U, 0x0320F809U, "world_contact_dispatch", 0, 0, Comparison::kNone},
    {0x001423E0U, 0x27BDE530U, "world_contact_entry", 0, 0, Comparison::kNone},
    {0x0014244CU, 0x14400272U, "world_contact_early_bypass", 0, 0, Comparison::kNone},
    {0x0014251CU, 0x78429CF0U, "position_store_0_complete", 0, 0, Comparison::kNone},
    {0x00142618U, 0x97A51ACEU, "position_store_1_complete", 0, 0, Comparison::kNone},
    {0x0014261CU, 0x0C052358U, "topology_query_call", 0, 0, Comparison::kNone},
    {0x00142624U, 0x104003C8U, "topology_query_decision", 0, 0, Comparison::kNone},
    {0x00142634U, 0x0C051874U, "world_node_traversal_call", 0, 0, Comparison::kNone},
    {0x0014263CU, 0xC6202434U, "world_node_traversal_result", 0, 0, Comparison::kNone},
    {0x00142658U, 0x0C0521A4U, "triangle_side_mask_call", 0, 0, Comparison::kNone},
    {0x00142660U, 0x0040802DU, "triangle_side_mask_result", 0, 0, Comparison::kNone},
    {0x00142668U, 0x144003CCU, "triangle_side_bit0_decision", 0, 0, Comparison::kNone},
    {0x00142674U, 0x10400046U, "triangle_side_bit1_decision", 0, 0, Comparison::kNone},
    {0x001426A0U, 0x0C052328U, "route_query_call", 0, 0, Comparison::kNone},
    {0x001426CCU, 0x0C051FB8U, "route_vector_call", 0, 0, Comparison::kNone},
    {0x00142790U, 0xDA220000U, "position_store_2_complete", 0, 0, Comparison::kNone},
    {0x001428D4U, 0x46010036U, "contact_distance_compare_0", 2, 0, Comparison::kLessThan},
    {0x001428ECU, 0x46010036U, "contact_distance_compare_1", 2, 0, Comparison::kLessThan},
    {0x00142904U, 0x46010036U, "contact_distance_compare_2", 2, 0, Comparison::kLessThan},
    {0x00142D88U, 0x0C06AD84U, "physics_integrator_call", 0, 0, Comparison::kNone},
    {0x00142D90U, 0x240203CAU, "physics_integrator_return", 0, 0, Comparison::kNone},
    {0x001436B4U, 0xDA220020U, "position_fallback_store_complete", 0, 0, Comparison::kNone},
    {0x00143840U, 0x7BB30030U, "world_contact_return", 0, 0, Comparison::kNone},
    {0x0019A704U, 0x0C0983CCU, "global_object_collision_pass", 0, 0, Comparison::kNone},
    {0x0019A770U, 0x0C097294U, "global_world_collision_pass", 0, 0, Comparison::kNone},
    {0x00263CF0U, 0x46002834U, "response_compare_0", 1, 5, Comparison::kNone},
    {0x00263D00U, 0x46006836U, "response_compare_1", 2, 13, Comparison::kNone},
    {0x00263D44U, 0x46000836U, "response_compare_2", 2, 1, Comparison::kNone},
}};

constexpr Target kFinalResponseTarget{0x00263D70U, 0x46000834U, "response_compare_3", 1, 1, Comparison::kNone};

constexpr std::array<Target, 59> kPhysicsTargets{{
    {0x001BD6F8U, 0x10400006U, "normal_physics_selection", 0, 0, Comparison::kNone},
    {0x001BD704U, 0x0C074A58U, "alternate_physics_call", 0, 0, Comparison::kNone},
    {0x001BD718U, 0x0C071224U, "normal_physics_call", 0, 0, Comparison::kNone},
    {0x001C4AC0U, 0x0C07150CU, "contact_gather_call", 0, 0, Comparison::kNone},
    {0x001C4AC8U, 0x144000F6U, "contact_gather_result", 0, 0, Comparison::kNone},
    {0x001C5430U, 0x27BDF2E0U, "contact_gather_entry", 0, 0, Comparison::kNone},
    {0x001C5484U, 0x106200F5U, "contact_disabled_bit0", 0, 0, Comparison::kNone},
    {0x001C5494U, 0x106200F1U, "contact_disabled_bit2", 0, 0, Comparison::kNone},
    {0x001C5580U, 0x0C071884U, "primary_shape_query_call", 0, 0, Comparison::kNone},
    {0x001C5588U, 0x26100001U, "primary_shape_query_return", 0, 0, Comparison::kNone},
    {0x001C55F0U, 0x0C071884U, "secondary_shape_query_call", 0, 0, Comparison::kNone},
    {0x001C55F8U, 0x26730001U, "secondary_shape_query_return", 0, 0, Comparison::kNone},
    {0x001C5650U, 0x8EA20000U, "primary_contact_count_load", 0, 0, Comparison::kNone},
    {0x001C5654U, 0x14400084U, "primary_contact_count_decision", 0, 0, Comparison::kNone},
    {0x001C5660U, 0x8C420000U, "secondary_contact_count_load", 0, 0, Comparison::kNone},
    {0x001C5664U, 0x1040007BU, "secondary_contact_count_decision", 0, 0, Comparison::kNone},
    {0x001C6210U, 0x27BDFBA0U, "narrowphase_entry", 0, 0, Comparison::kNone},
    {0x001C6248U, 0x14400076U, "narrowphase_special_path", 0, 0, Comparison::kNone},
    {0x001C6338U, 0x146000A8U, "narrowphase_edge_reject_0", 0, 0, Comparison::kNone},
    {0x001C63A4U, 0x1460008DU, "narrowphase_edge_reject_1", 0, 0, Comparison::kNone},
    {0x001C6414U, 0x10600019U, "narrowphase_clip_decision", 0, 0, Comparison::kNone},
    {0x001C642CU, 0x1065006BU, "narrowphase_special_filter", 0, 0, Comparison::kNone},
    {0x001C6454U, 0x10650061U, "narrowphase_shape_filter", 0, 0, Comparison::kNone},
    {0x001C6488U, 0x0C0AFB70U, "overlap_clip_call", 0, 0, Comparison::kNone},
    {0x001C6490U, 0x28430003U, "overlap_clip_vertex_count", 0, 0, Comparison::kNone},
    {0x001C6494U, 0x14600051U, "overlap_clip_minimum_decision", 0, 0, Comparison::kNone},
    {0x001C64B4U, 0x45000002U, "contact_accumulator_selector", 0, 0, Comparison::kNone},
    {0x001C6544U, 0x45000025U, "contact_normal_reject", 0, 0, Comparison::kNone},
    {0x001C6570U, 0xAE0300D4U, "secondary_contact_accepted", 0, 0, Comparison::kNone},
    {0x001C65D0U, 0xAE0300D0U, "primary_contact_accepted", 0, 0, Comparison::kNone},
    {0x001C558CU, 0x8E830200U, "primary_shape_container_load", 0, 0, Comparison::kNone},
    {0x001C5590U, 0x8C620000U, "primary_shape_count_load", 0, 0, Comparison::kNone},
    {0x001C5594U, 0x0202102AU, "primary_shape_count_compare", 0, 0, Comparison::kNone},
    {0x001C5598U, 0x1440FFF1U, "primary_shape_loop_decision", 0, 0, Comparison::kNone},
    {0x001C55A0U, 0x1240002AU, "secondary_shape_owner_gate", 0, 0, Comparison::kNone},
    {0x001C55A8U, 0x8E423510U, "secondary_shape_count_load", 0, 0, Comparison::kNone},
    {0x001C55ACU, 0x18400027U, "secondary_shape_count_decision", 0, 0, Comparison::kNone},
    {0x001C5168U, 0x8E020200U, "candidate_list_reset_load", 0, 0, Comparison::kNone},
    {0x001C51C8U, 0x0C0C06C0U, "broadphase_query_call", 0, 0, Comparison::kNone},
    {0x001C51D0U, 0x92031351U, "broadphase_query_result", 0, 0, Comparison::kNone},
    {0x00301B00U, 0x27BDFFD0U, "broadphase_entry", 0, 0, Comparison::kNone},
    {0x00301B2CU, 0x14A2000BU, "broadphase_current_slot_gate", 0, 0, Comparison::kNone},
    {0x00301B4CU, 0x0C0EFA6CU, "broadphase_traversal_call", 0, 0, Comparison::kNone},
    {0x00301B5CU, 0x04A00012U, "broadphase_negative_slot_gate", 0, 0, Comparison::kNone},
    {0x00301B6CU, 0x1020000EU, "broadphase_slot_range_gate", 0, 0, Comparison::kNone},
    {0x00301B90U, 0x14620003U, "broadphase_page_state_gate", 0, 0, Comparison::kNone},
    {0x00301BB0U, 0x0000102DU, "broadphase_rejected_return", 0, 0, Comparison::kNone},
    {0x001C5330U, 0x27BDFF90U, "broadphase_candidate_callback", 0, 0, Comparison::kNone},
    {0x001C541CU, 0x0C06AC24U, "broadphase_candidate_append_call", 0, 0, Comparison::kNone},
    {0x001AB090U, 0x9382A094U, "candidate_append_entry", 0, 0, Comparison::kNone},
    {0x001AB108U, 0xAC660000U, "candidate_append_count_store", 0, 0, Comparison::kNone},
    {0x001C534CU, 0x10620010U, "candidate_kind_23_filter", 0, 0, Comparison::kNone},
    {0x001C5358U, 0x1062000DU, "candidate_kind_22_filter", 0, 0, Comparison::kNone},
    {0x001C5368U, 0x14400009U, "candidate_flag_1000_filter", 0, 0, Comparison::kNone},
    {0x001C5374U, 0x14400014U, "candidate_kind_low_gate", 0, 0, Comparison::kNone},
    {0x001C5380U, 0x10200011U, "candidate_kind_high_gate", 0, 0, Comparison::kNone},
    {0x001C5398U, 0x1440000DU, "candidate_kind_special_gate", 0, 0, Comparison::kNone},
    {0x001C53B8U, 0x45000017U, "candidate_height_filter", 0, 0, Comparison::kNone},
    {0x001C5408U, 0x4501FFE5U, "candidate_dot_filter", 0, 0, Comparison::kNone},
}};

constexpr std::array<Target, 20> kSpatialTargets{{
    {0x003BEA68U, 0x4500012FU, "spatial_root_overlap_gate", 0, 0, Comparison::kNone},
    {0x003BED1CU, 0x8E450020U, "spatial_tree_walk_entry", 0, 0, Comparison::kNone},
    {0x003BEE2CU, 0x0C09CC1CU, "spatial_leaf_helper_a_call", 0, 0, Comparison::kNone},
    {0x003BEE54U, 0x0C09D3B4U, "spatial_leaf_helper_b_call", 0, 0, Comparison::kNone},
    {0x00273070U, 0x27BDFDF0U, "spatial_leaf_helper_a_entry", 0, 0, Comparison::kNone},
    {0x002732D4U, 0x4A000178U, "spatial_vcallms_5", 0, 0, Comparison::kNone},
    {0x002732D8U, 0x48420801U, "spatial_vcallms_5_result", 0, 0, Comparison::kNone},
    {0x00273344U, 0x4A000038U, "spatial_vcallms_0", 0, 0, Comparison::kNone},
    {0x00273348U, 0x48420801U, "spatial_vcallms_0_result", 0, 0, Comparison::kNone},
    {0x00273360U, 0x1040000CU, "spatial_helper_a_result_gate", 0, 0, Comparison::kNone},
    {0x00273378U, 0x02C0F809U, "spatial_helper_a_callback", 0, 0, Comparison::kNone},
    {0x00274ED0U, 0x27BDFE70U, "spatial_leaf_helper_b_entry", 0, 0, Comparison::kNone},
    {0x002750A4U, 0x4A000338U, "spatial_vcallms_12_first", 0, 0, Comparison::kNone},
    {0x002750A8U, 0x48430801U, "spatial_vcallms_12_first_result", 0, 0, Comparison::kNone},
    {0x002750B0U, 0x1060000DU, "spatial_helper_b_first_result_gate", 0, 0, Comparison::kNone},
    {0x002750CCU, 0x0280F809U, "spatial_helper_b_first_callback", 0, 0, Comparison::kNone},
    {0x00275194U, 0x4A000338U, "spatial_vcallms_12_second", 0, 0, Comparison::kNone},
    {0x00275198U, 0x48430801U, "spatial_vcallms_12_second_result", 0, 0, Comparison::kNone},
    {0x002751A0U, 0x1060000EU, "spatial_helper_b_second_result_gate", 0, 0, Comparison::kNone},
    {0x002751C0U, 0x0280F809U, "spatial_helper_b_second_callback", 0, 0, Comparison::kNone},
}};

constexpr std::array<Target, 16> kActiveListTargets{{
    {0x0013472CU, 0x0C0667ACU, "active_list_build_call", 0, 0, Comparison::kNone},
    {0x00199EB0U, 0x27BDFFB0U, "active_list_build_entry", 0, 0, Comparison::kNone},
    {0x00199EFCU, 0xAC60AD1CU, "active_list_build_reset", 0, 0, Comparison::kNone},
    {0x00199F08U, 0x18E0004EU, "active_list_mode_decision", 0, 0, Comparison::kNone},
    {0x00199F18U, 0x8C852B08U, "active_list_source_count_load", 0, 0, Comparison::kNone},
    {0x00199F28U, 0xAC65AD1CU, "active_list_source_count_store", 0, 0, Comparison::kNone},
    {0x00199F94U, 0xACE50000U, "active_list_fixed_pointer_store", 0, 0, Comparison::kNone},
    {0x00199FD0U, 0xACE30000U, "active_list_fallback_pointer_store", 0, 0, Comparison::kNone},
    {0x0019A0C0U, 0xAD4BAD28U, "active_list_dynamic_pointer_store", 0, 0, Comparison::kNone},
    {0x0019A100U, 0x8D28AD1CU, "active_list_dynamic_count_load", 0, 0, Comparison::kNone},
    {0x0019A108U, 0xAD28AD1CU, "active_list_dynamic_count_store", 0, 0, Comparison::kNone},
    {0x0019A15CU, 0xAD2CAD28U, "active_list_alternate_pointer_store", 0, 0, Comparison::kNone},
    {0x0019A1C4U, 0x03E00008U, "active_list_build_return", 0, 0, Comparison::kNone},
    {0x0019A38CU, 0xAC60AD1CU, "active_list_cleanup_reset", 0, 0, Comparison::kNone},
    {0x0019A674U, 0x8C42AD1CU, "active_list_dispatch_count_load", 0, 0, Comparison::kNone},
    {0x0019B3C8U, 0xAC60AD1CU, "active_list_init_reset", 0, 0, Comparison::kNone},
}};


constexpr std::array<std::uint32_t, 4> kSpatialResultPcs{{2568920U, 2569032U, 2576552U, 2576792U}};

// RECOVERY: unrolled by hand; the original's PHI edge order for the switch default differs slightly.
[[nodiscard]] const Target* find_target(const GuestAddress pc) noexcept {
    if (pc == kTargets[0].pc) { return &kTargets[0]; }
    if (pc == kTargets[1].pc) { return &kTargets[1]; }
    if (pc == kTargets[2].pc) { return &kTargets[2]; }
    if (pc == kTargets[3].pc) { return &kTargets[3]; }
    if (pc == kTargets[4].pc) { return &kTargets[4]; }
    if (pc == kTargets[5].pc) { return &kTargets[5]; }
    if (pc == kTargets[6].pc) { return &kTargets[6]; }
    if (pc == kTargets[7].pc) { return &kTargets[7]; }
    if (pc == kTargets[8].pc) { return &kTargets[8]; }
    if (pc == kTargets[9].pc) { return &kTargets[9]; }
    if (pc == kTargets[10].pc) { return &kTargets[10]; }
    if (pc == kTargets[11].pc) { return &kTargets[11]; }
    if (pc == kTargets[12].pc) { return &kTargets[12]; }
    if (pc == kTargets[13].pc) { return &kTargets[13]; }
    if (pc == kTargets[14].pc) { return &kTargets[14]; }
    if (pc == kTargets[15].pc) { return &kTargets[15]; }
    if (pc == kTargets[16].pc) { return &kTargets[16]; }
    if (pc == kTargets[17].pc) { return &kTargets[17]; }
    if (pc == kTargets[18].pc) { return &kTargets[18]; }
    if (pc == kTargets[19].pc) { return &kTargets[19]; }
    if (pc == kTargets[20].pc) { return &kTargets[20]; }
    if (pc == kTargets[21].pc) { return &kTargets[21]; }
    if (pc == kTargets[22].pc) { return &kTargets[22]; }
    if (pc == kTargets[23].pc) { return &kTargets[23]; }
    if (pc == kTargets[24].pc) { return &kTargets[24]; }
    if (pc == kTargets[25].pc) { return &kTargets[25]; }
    if (pc == kTargets[26].pc) { return &kTargets[26]; }
    if (pc == kTargets[27].pc) { return &kTargets[27]; }
    if (pc == kTargets[28].pc) { return &kTargets[28]; }
    if (pc == kFinalResponseTarget.pc) { return &kFinalResponseTarget; }
    if (pc == kPhysicsTargets[0].pc) { return &kPhysicsTargets[0]; }
    if (pc == kPhysicsTargets[1].pc) { return &kPhysicsTargets[1]; }
    if (pc == kPhysicsTargets[2].pc) { return &kPhysicsTargets[2]; }
    if (pc == kPhysicsTargets[3].pc) { return &kPhysicsTargets[3]; }
    if (pc == kPhysicsTargets[4].pc) { return &kPhysicsTargets[4]; }
    if (pc == kPhysicsTargets[5].pc) { return &kPhysicsTargets[5]; }
    if (pc == kPhysicsTargets[6].pc) { return &kPhysicsTargets[6]; }
    if (pc == kPhysicsTargets[7].pc) { return &kPhysicsTargets[7]; }
    if (pc == kPhysicsTargets[8].pc) { return &kPhysicsTargets[8]; }
    if (pc == kPhysicsTargets[9].pc) { return &kPhysicsTargets[9]; }
    if (pc == kPhysicsTargets[10].pc) { return &kPhysicsTargets[10]; }
    if (pc == kPhysicsTargets[11].pc) { return &kPhysicsTargets[11]; }
    if (pc == kPhysicsTargets[12].pc) { return &kPhysicsTargets[12]; }
    if (pc == kPhysicsTargets[13].pc) { return &kPhysicsTargets[13]; }
    if (pc == kPhysicsTargets[14].pc) { return &kPhysicsTargets[14]; }
    if (pc == kPhysicsTargets[15].pc) { return &kPhysicsTargets[15]; }
    if (pc == kPhysicsTargets[16].pc) { return &kPhysicsTargets[16]; }
    if (pc == kPhysicsTargets[17].pc) { return &kPhysicsTargets[17]; }
    if (pc == kPhysicsTargets[18].pc) { return &kPhysicsTargets[18]; }
    if (pc == kPhysicsTargets[19].pc) { return &kPhysicsTargets[19]; }
    if (pc == kPhysicsTargets[20].pc) { return &kPhysicsTargets[20]; }
    if (pc == kPhysicsTargets[21].pc) { return &kPhysicsTargets[21]; }
    if (pc == kPhysicsTargets[22].pc) { return &kPhysicsTargets[22]; }
    if (pc == kPhysicsTargets[23].pc) { return &kPhysicsTargets[23]; }
    if (pc == kPhysicsTargets[24].pc) { return &kPhysicsTargets[24]; }
    if (pc == kPhysicsTargets[25].pc) { return &kPhysicsTargets[25]; }
    if (pc == kPhysicsTargets[26].pc) { return &kPhysicsTargets[26]; }
    if (pc == kPhysicsTargets[27].pc) { return &kPhysicsTargets[27]; }
    if (pc == kPhysicsTargets[28].pc) { return &kPhysicsTargets[28]; }
    if (pc == kPhysicsTargets[29].pc) { return &kPhysicsTargets[29]; }
    if (pc == kPhysicsTargets[30].pc) { return &kPhysicsTargets[30]; }
    if (pc == kPhysicsTargets[31].pc) { return &kPhysicsTargets[31]; }
    if (pc == kPhysicsTargets[32].pc) { return &kPhysicsTargets[32]; }
    if (pc == kPhysicsTargets[33].pc) { return &kPhysicsTargets[33]; }
    if (pc == kPhysicsTargets[34].pc) { return &kPhysicsTargets[34]; }
    if (pc == kPhysicsTargets[35].pc) { return &kPhysicsTargets[35]; }
    if (pc == kPhysicsTargets[36].pc) { return &kPhysicsTargets[36]; }
    if (pc == kPhysicsTargets[37].pc) { return &kPhysicsTargets[37]; }
    if (pc == kPhysicsTargets[38].pc) { return &kPhysicsTargets[38]; }
    if (pc == kPhysicsTargets[39].pc) { return &kPhysicsTargets[39]; }
    if (pc == kPhysicsTargets[40].pc) { return &kPhysicsTargets[40]; }
    if (pc == kPhysicsTargets[41].pc) { return &kPhysicsTargets[41]; }
    if (pc == kPhysicsTargets[42].pc) { return &kPhysicsTargets[42]; }
    if (pc == kPhysicsTargets[43].pc) { return &kPhysicsTargets[43]; }
    if (pc == kPhysicsTargets[44].pc) { return &kPhysicsTargets[44]; }
    if (pc == kPhysicsTargets[45].pc) { return &kPhysicsTargets[45]; }
    if (pc == kPhysicsTargets[46].pc) { return &kPhysicsTargets[46]; }
    if (pc == kPhysicsTargets[47].pc) { return &kPhysicsTargets[47]; }
    if (pc == kPhysicsTargets[48].pc) { return &kPhysicsTargets[48]; }
    if (pc == kPhysicsTargets[49].pc) { return &kPhysicsTargets[49]; }
    if (pc == kPhysicsTargets[50].pc) { return &kPhysicsTargets[50]; }
    if (pc == kPhysicsTargets[51].pc) { return &kPhysicsTargets[51]; }
    if (pc == kPhysicsTargets[52].pc) { return &kPhysicsTargets[52]; }
    if (pc == kPhysicsTargets[53].pc) { return &kPhysicsTargets[53]; }
    if (pc == kPhysicsTargets[54].pc) { return &kPhysicsTargets[54]; }
    if (pc == kPhysicsTargets[55].pc) { return &kPhysicsTargets[55]; }
    if (pc == kPhysicsTargets[56].pc) { return &kPhysicsTargets[56]; }
    if (pc == kPhysicsTargets[57].pc) { return &kPhysicsTargets[57]; }
    if (pc == kPhysicsTargets[58].pc) { return &kPhysicsTargets[58]; }
    if (pc == kSpatialTargets[0].pc) { return &kSpatialTargets[0]; }
    if (pc == kSpatialTargets[1].pc) { return &kSpatialTargets[1]; }
    if (pc == kSpatialTargets[2].pc) { return &kSpatialTargets[2]; }
    if (pc == kSpatialTargets[3].pc) { return &kSpatialTargets[3]; }
    if (pc == kSpatialTargets[4].pc) { return &kSpatialTargets[4]; }
    if (pc == kSpatialTargets[5].pc) { return &kSpatialTargets[5]; }
    if (pc == kSpatialTargets[6].pc) { return &kSpatialTargets[6]; }
    if (pc == kSpatialTargets[7].pc) { return &kSpatialTargets[7]; }
    if (pc == kSpatialTargets[8].pc) { return &kSpatialTargets[8]; }
    if (pc == kSpatialTargets[9].pc) { return &kSpatialTargets[9]; }
    if (pc == kSpatialTargets[10].pc) { return &kSpatialTargets[10]; }
    if (pc == kSpatialTargets[11].pc) { return &kSpatialTargets[11]; }
    if (pc == kSpatialTargets[12].pc) { return &kSpatialTargets[12]; }
    if (pc == kSpatialTargets[13].pc) { return &kSpatialTargets[13]; }
    if (pc == kSpatialTargets[14].pc) { return &kSpatialTargets[14]; }
    if (pc == kSpatialTargets[15].pc) { return &kSpatialTargets[15]; }
    if (pc == kSpatialTargets[16].pc) { return &kSpatialTargets[16]; }
    if (pc == kSpatialTargets[17].pc) { return &kSpatialTargets[17]; }
    if (pc == kSpatialTargets[18].pc) { return &kSpatialTargets[18]; }
    if (pc == kSpatialTargets[19].pc) { return &kSpatialTargets[19]; }
    if (pc == kActiveListTargets[0].pc) { return &kActiveListTargets[0]; }
    if (pc == kActiveListTargets[1].pc) { return &kActiveListTargets[1]; }
    if (pc == kActiveListTargets[2].pc) { return &kActiveListTargets[2]; }
    if (pc == kActiveListTargets[3].pc) { return &kActiveListTargets[3]; }
    if (pc == kActiveListTargets[4].pc) { return &kActiveListTargets[4]; }
    if (pc == kActiveListTargets[5].pc) { return &kActiveListTargets[5]; }
    if (pc == kActiveListTargets[6].pc) { return &kActiveListTargets[6]; }
    if (pc == kActiveListTargets[7].pc) { return &kActiveListTargets[7]; }
    if (pc == kActiveListTargets[8].pc) { return &kActiveListTargets[8]; }
    if (pc == kActiveListTargets[9].pc) { return &kActiveListTargets[9]; }
    if (pc == kActiveListTargets[10].pc) { return &kActiveListTargets[10]; }
    if (pc == kActiveListTargets[11].pc) { return &kActiveListTargets[11]; }
    if (pc == kActiveListTargets[12].pc) { return &kActiveListTargets[12]; }
    if (pc == kActiveListTargets[13].pc) { return &kActiveListTargets[13]; }
    if (pc == kActiveListTargets[14].pc) { return &kActiveListTargets[14]; }
    if (pc == kActiveListTargets[15].pc) { return &kActiveListTargets[15]; }
    return nullptr;
}

// Up to sixteen guest words read from consecutive addresses; valid_mask marks
// the words whose address was readable.
struct WordSlice {
    std::uint32_t base{};
    std::uint16_t valid_mask{};
    std::uint8_t count{};
    bool base_present{};
    std::array<std::uint32_t, 16U> words{};
};

[[nodiscard]] std::optional<std::uint32_t> checked_add(const std::uint64_t base,
                                                       const std::uint64_t offset) {
    const auto sum = base + offset;
    if (sum > 0xFFFFFFFFULL) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(sum);
}

[[nodiscard]] WordSlice capture_slice(const EeMemory& memory,
                                      const std::optional<std::uint32_t> base,
                                      const std::uint8_t count) {
    WordSlice slice{};
    slice.count = std::min<std::uint8_t>(count, 16U);
    if (!base) {
        return slice;
    }
    slice.base_present = true;
    slice.base = *base;
    for (std::size_t index = 0; index < slice.count; ++index) {
        const auto address = checked_add(*base, static_cast<std::uint32_t>(index) * 4U);
        if (!address || !memory.contains(*address, 4U)) {
            continue;
        }
        slice.words[index] = memory.read_u32_le(*address);
        slice.valid_mask = static_cast<std::uint16_t>(slice.valid_mask | (1U << index));
    }
    return slice;
}

[[nodiscard]] std::optional<std::uint32_t> read_word(const EeMemory& memory,
                                    const std::optional<GuestAddress> address) {
    if (!address || !memory.contains(*address, 4U)) {
        return std::nullopt;
    }
    return memory.read_u32_le(*address);
}

[[nodiscard]] bool valid_snapshot_pointer(const EeMemory& memory, const GuestAddress pointer) {
    return pointer != 0U && (pointer & 3U) == 0U && memory.is_main_ram_range(pointer, 4U);
}

[[nodiscard]] std::optional<std::uint32_t> read_snapshot_word(
    const EeMemory& memory, const std::optional<GuestAddress> address) {
    if (!address || (*address & 3U) != 0U || !memory.is_main_ram_range(*address, 4U)) {
        return std::nullopt;
    }
    return memory.read_u32_le(*address);
}

template <std::size_t Count>
[[nodiscard]] std::optional<std::array<std::uint32_t, Count>> capture_snapshot_words(
    const EeMemory& memory, const GuestAddress base, const std::uint32_t offset) {
    const std::optional<GuestAddress> start = checked_add(base, offset);
    if (!start || (*start & 3U) != 0U ||
        !memory.is_main_ram_range(*start, static_cast<std::uint32_t>(Count * 4U))) {
        return std::nullopt;
    }
    std::array<std::uint32_t, Count> words{};
    for (std::size_t index = 0; index < Count; ++index) {
        words[index] = memory.read_u32_le(*start + static_cast<std::uint32_t>(index) * 4U);
    }
    return words;
}

[[nodiscard]] std::optional<std::uint32_t> slice_word(const WordSlice& slice,
                                                      const std::size_t index) {
    if (index >= slice.count || index >= slice.words.size() ||
        (slice.valid_mask & static_cast<std::uint16_t>(1U << index)) == 0U) {
        return std::nullopt;
    }
    return slice.words[index];
}

// The PS2 FPU has no denormals or infinities: flush denormals to signed zero
// and clamp the exponent-all-ones case to the largest finite magnitude.
[[nodiscard]] std::uint32_t normalize_float_bits(const std::uint32_t bits) {
    const auto exponent = bits & 0x7F800000U;
    if (exponent == 0U) {
        return bits & 0x80000000U;
    }
    if (exponent == 0x7F800000U) {
        return (bits & 0x80000000U) | 0x7F7FFFFFU;
    }
    return bits;
}

[[nodiscard]] std::optional<float> slice_float(const WordSlice& slice, const std::size_t index) {
    const auto word = slice_word(slice, index);
    if (!word) {
        return std::nullopt;
    }
    return std::bit_cast<float>(normalize_float_bits(*word));
}

[[nodiscard]] bool is_candidate_filter_label(const std::string_view label) {
    return label == "candidate_kind_23_filter" ||
           label == "candidate_kind_22_filter" ||
           label == "candidate_flag_1000_filter" ||
           label == "candidate_kind_low_gate" ||
           label == "candidate_kind_high_gate" ||
           label == "candidate_kind_special_gate" ||
           label == "candidate_height_filter" ||
           label == "candidate_dot_filter";
}

void write_hex32(std::ostream& output, const std::uint32_t value) {
    output << "0x" << std::hex << std::setw(8) << std::setfill('0') << value << std::dec;
}

void write_u16_array(std::ostream& output, const std::array<std::uint16_t, 16U>& values) {
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0U) {
            output << ';';
        }
        output << "0x" << std::hex << std::setw(4) << std::setfill('0') << values[index] << std::dec;
    }
}

void write_slice(std::ostream& output, const WordSlice& slice) {
    output << (slice.base_present ? 1 : 0) << ':';
    write_hex32(output, slice.base);
    output << ":0x" << std::hex << std::setw(4) << std::setfill('0') << slice.valid_mask << std::dec << ':'
           << static_cast<unsigned>(slice.count) << ':';
    for (std::size_t index = 0; index < slice.count; ++index) {
        if (index != 0U) {
            output << ';';
        }
        write_hex32(output, slice.words[index]);
    }
}

// One observed instruction plus the guest state around it.  Members are grouped
// by size so the large sample array stays compact.
struct CollisionCompareSample {
    std::uint64_t sequence{};
    Slus21050CollisionTraceContext context{};
    GuestAddress pc_before{};
    GuestAddress next_pc_before{};
    GuestAddress pc_after{};
    GuestAddress next_pc_after{};
    std::uint32_t instruction_word{};
    std::uint32_t expected_word{};
    std::uint32_t fcr31_before{};
    std::uint32_t fcr31_after{};
    std::uint32_t fpu_accumulator{};
    std::uint32_t fs_raw{};
    std::uint32_t ft_raw{};
    std::uint32_t fs_normalized{};
    std::uint32_t ft_normalized{};
    std::uint32_t step_status{0xFFFFFFFFU};
    std::uint8_t fs{};
    std::uint8_t ft{};
    const char* label{};
    bool less_equal{};
    bool is_float_comparison{};
    bool expected_fcc0{};
    bool delay_slot_before{};
    bool complete{};
    std::array<guest::Gpr128, 32U> gprs{};
    std::array<std::uint32_t, 32U> fprs{};
    std::array<guest::Gpr128, 9U> vu0_vectors{};
    std::array<std::uint16_t, 16U> vu0_integers{};
    std::array<std::uint32_t, 32U> vu0_controls{};
    std::array<std::uint32_t, 4U> vu0_accumulator{};
    WordSlice stack{};
    WordSlice ring_header{};
    WordSlice candidate{};
    WordSlice output_slot{};
    WordSlice coefficients{};
    GuestAddress vehicle_object{};
    GuestAddress physics_component{};
    GuestAddress dispatched_method{};
    GuestAddress physics_object{};
    GuestAddress owner_vehicle{};
    GuestAddress contact_accumulator{};
    GuestAddress primary_shape_container{};
    GuestAddress broadphase_provider{};
    GuestAddress broadphase_slot_page{};
    GuestAddress broadphase_page_base{};
    GuestAddress broadphase_traversal_root{};
    GuestAddress broadphase_query_pointer{};
    GuestAddress broadphase_callback_pointer{};
    GuestAddress broadphase_candidate{};
    GuestAddress broadphase_candidate_descriptor{};
    GuestAddress spatial_leaf_pointer{};
    GuestAddress active_list_store_address{};
    GuestAddress world_node{};
    std::uint32_t query_result{};
    std::uint32_t owner_collision_mode{};
    std::uint32_t primary_shape_count{};
    std::uint32_t secondary_shape_count{};
    std::uint32_t broadphase_slot{};
    std::uint32_t broadphase_page_state{};
    std::uint32_t broadphase_expected_page_state{};
    std::uint32_t broadphase_page_state_memory{};
    std::uint32_t broadphase_candidate_type_flags{};
    std::uint32_t candidate_append_overflow_latch{};
    std::uint32_t candidate_append_old_count{};
    std::uint32_t candidate_append_new_count{};
    std::uint32_t active_list_store_value{};
    std::uint32_t world_contact_vehicle_register{};
    std::uint32_t primary_contact_count{};
    std::uint32_t secondary_contact_count{};
    std::uint16_t world_triangle{};
    bool physics_method_matches{};
    bool query_result_valid{};
    bool owner_collision_mode_valid{};
    bool primary_shape_count_valid{};
    bool secondary_shape_count_valid{};
    bool broadphase_slot_valid{};
    bool broadphase_slot_page_valid{};
    bool broadphase_page_state_valid{};
    bool broadphase_page_state_memory_valid{};
    bool broadphase_candidate_type_flags_valid{};
    bool candidate_append_overflow_latch_valid{};
    bool candidate_append_old_count_valid{};
    bool candidate_append_new_count_valid{};
    bool active_list_store_valid{};
    bool primary_contact_count_valid{};
    bool secondary_contact_count_valid{};
    WordSlice vehicle_contact_state{};
    WordSlice vehicle_transform{};
    WordSlice vehicle_vectors{};
    WordSlice physics_state{};
    WordSlice physics_object_state{};
    WordSlice physics_owner_region{};
    WordSlice physics_shape_slots{};
    WordSlice primary_shape_header{};
    WordSlice secondary_shape_region{};
    WordSlice broadphase_provider_state{};
    WordSlice broadphase_query_state{};
    WordSlice broadphase_candidate_state{};
    WordSlice broadphase_candidate_descriptor_state{};
    WordSlice broadphase_owner_filter_vector{};
    WordSlice spatial_leaf_state{};
    WordSlice active_list_state{};
    WordSlice active_list_caller_config{};
    WordSlice active_list_fixed_source{};
    WordSlice contact_accumulator_tail{};
    WordSlice shape_geometry{};
    WordSlice clip_bounds{};
    WordSlice clip_vertices{};
    WordSlice clip_output{};
    std::optional<Slus21050VehicleWorldContactSnapshot> vehicle_snapshot{};
    std::uint64_t diagnostic_candidate_id{~0ULL};
};

// Per-frame, per-car roll-up of the collision pipeline counters.
struct CollisionFrameCarSummary {
    std::uint64_t vblank{};
    std::uint64_t first_sequence{};
    std::uint64_t last_sequence{};
    GuestAddress vehicle_object{};
    GuestAddress physics_object{};
    std::uint32_t dispatches{};
    std::uint32_t broadphase_queries{};
    std::uint32_t broadphase_query_hits{};
    std::uint32_t candidate_callbacks{};
    std::uint32_t candidate_filter_probes{};
    std::uint32_t candidate_filter_rejects{};
    std::uint32_t candidate_append_calls{};
    std::uint32_t candidate_append_stores{};
    std::uint32_t candidate_append_overflows{};
    std::uint32_t narrowphase_calls{};
    std::uint32_t overlap_clip_calls{};
    std::uint32_t overlap_clip_zero{};
    std::uint32_t overlap_clip_nonzero{};
    std::uint32_t primary_contact_count_max{};
    std::uint32_t secondary_contact_count_max{};
    std::uint32_t primary_contacts_accepted{};
    std::uint32_t secondary_contacts_accepted{};
    std::uint32_t gather_calls{};
    std::uint32_t gather_nonzero{};
    std::uint32_t response_probes{};
    bool role_known{};
    bool ai{};
};

void write_sample(std::ostream& output, const CollisionCompareSample& sample) {
    output << "sample\t" << sample.sequence << '\t' << sample.context.committed_steps << '\t'
           << sample.context.ee_cycles << '\t' << sample.context.vblank_starts << '\t'
           << sample.context.thread_id << '\t';
    write_hex32(output, sample.pc_before);
    output << '\t';
    write_hex32(output, sample.next_pc_before);
    output << '\t' << (sample.delay_slot_before ? 1 : 0) << '\t';
    write_hex32(output, sample.instruction_word);
    output << '\t';
    write_hex32(output, sample.expected_word);
    output << '\t' << (sample.instruction_word == sample.expected_word ? 1 : 0) << '\t'
           << sample.label << '\t' << static_cast<unsigned>(sample.fs) << '\t'
           << static_cast<unsigned>(sample.ft) << '\t'
           << (!sample.is_float_comparison ? "-" : (sample.less_equal ? "le" : "lt")) << '\t';
    write_hex32(output, sample.fs_raw);
    output << '\t';
    write_hex32(output, sample.ft_raw);
    output << '\t';
    write_hex32(output, sample.fs_normalized);
    output << '\t';
    write_hex32(output, sample.ft_normalized);
    output << '\t' << (sample.expected_fcc0 ? 1 : 0) << '\t';
    write_hex32(output, sample.fcr31_before);
    output << '\t';
    write_hex32(output, sample.fcr31_after);
    output << '\t' << static_cast<int>((sample.fcr31_after >> 23U) & 1U) << '\t';
    write_hex32(output, sample.fpu_accumulator);
    output << '\t' << sample.step_status << '\t';
    write_hex32(output, sample.pc_after);
    output << '\t';
    write_hex32(output, sample.next_pc_after);
    output << '\t' << (sample.complete ? 1 : 0) << '\t';
    bool first = true;
    for (const auto& gpr : sample.gprs) {
        for (const auto word : gpr.words) {
            if (!first) {
                output << ';';
            }
            first = false;
            write_hex32(output, word);
        }
    }
    output << '\t';
    for (std::size_t index = 0; index < sample.fprs.size(); ++index) {
        if (index != 0U) {
            output << ';';
        }
        write_hex32(output, sample.fprs[index]);
    }
    output << '\t';
    first = true;
    for (const auto& vector : sample.vu0_vectors) {
        for (const auto word : vector.words) {
            if (!first) {
                output << ';';
            }
            first = false;
            write_hex32(output, word);
        }
    }
    output << '\t';
    write_u16_array(output, sample.vu0_integers);
    output << '\t';
    for (std::size_t index = 0; index < sample.vu0_controls.size(); ++index) {
        if (index != 0U) {
            output << ';';
        }
        write_hex32(output, sample.vu0_controls[index]);
    }
    output << '\t';
    for (std::size_t index = 0; index < sample.vu0_accumulator.size(); ++index) {
        if (index != 0U) {
            output << ';';
        }
        write_hex32(output, sample.vu0_accumulator[index]);
    }
    output << '\t';
    write_slice(output, sample.stack);
    output << '\t';
    write_slice(output, sample.ring_header);
    output << '\t';
    write_slice(output, sample.candidate);
    output << '\t';
    write_slice(output, sample.output_slot);
    output << '\t';
    write_slice(output, sample.coefficients);
    output << '\t';
    write_hex32(output, sample.vehicle_object);
    output << '\t';
    write_hex32(output, sample.physics_component);
    output << '\t';
    write_hex32(output, sample.dispatched_method);
    output << '\t' << (sample.physics_method_matches ? 1 : 0) << '\t';
    write_hex32(output, sample.physics_object);
    output << '\t';
    write_hex32(output, sample.owner_vehicle);
    output << '\t';
    write_hex32(output, sample.contact_accumulator);
    output << '\t' << (sample.query_result_valid ? 1 : 0) << '\t';
    write_hex32(output, sample.query_result);
    output << '\t' << (sample.owner_collision_mode_valid ? 1 : 0) << '\t';
    write_hex32(output, sample.owner_collision_mode);
    output << '\t';
    write_hex32(output, sample.primary_shape_container);
    output << '\t' << (sample.primary_shape_count_valid ? 1 : 0) << '\t'
           << sample.primary_shape_count << '\t' << (sample.secondary_shape_count_valid ? 1 : 0)
           << '\t' << sample.secondary_shape_count << '\t';
    write_hex32(output, sample.broadphase_provider);
    output << '\t' << (sample.broadphase_slot_valid ? 1 : 0) << '\t';
    write_hex32(output, sample.broadphase_slot);
    output << '\t' << (sample.broadphase_slot_page_valid ? 1 : 0) << '\t';
    write_hex32(output, sample.broadphase_slot_page);
    output << '\t';
    write_hex32(output, sample.broadphase_page_base);
    output << '\t' << (sample.broadphase_page_state_valid ? 1 : 0) << '\t'
           << sample.broadphase_page_state << '\t' << sample.broadphase_expected_page_state << '\t'
           << (sample.broadphase_page_state_memory_valid ? 1 : 0) << '\t'
           << sample.broadphase_page_state_memory << '\t';
    write_hex32(output, sample.broadphase_traversal_root);
    output << '\t';
    write_hex32(output, sample.broadphase_query_pointer);
    output << '\t';
    write_hex32(output, sample.broadphase_callback_pointer);
    output << '\t';
    write_hex32(output, sample.broadphase_candidate);
    output << '\t';
    write_hex32(output, sample.broadphase_candidate_descriptor);
    output << '\t';
    write_hex32(output, sample.spatial_leaf_pointer);
    output << '\t' << (sample.active_list_store_valid ? 1 : 0) << '\t';
    write_hex32(output, sample.active_list_store_address);
    output << '\t';
    write_hex32(output, sample.active_list_store_value);
    output << '\t' << (sample.broadphase_candidate_type_flags_valid ? 1 : 0) << '\t'
           << sample.broadphase_candidate_type_flags << '\t'
           << (sample.candidate_append_overflow_latch_valid ? 1 : 0) << '\t'
           << sample.candidate_append_overflow_latch << '\t'
           << (sample.candidate_append_old_count_valid ? 1 : 0) << '\t'
           << sample.candidate_append_old_count << '\t'
           << (sample.candidate_append_new_count_valid ? 1 : 0) << '\t'
           << sample.candidate_append_new_count << '\t'
           << (sample.primary_contact_count_valid ? 1 : 0) << '\t' << sample.primary_contact_count
           << '\t' << (sample.secondary_contact_count_valid ? 1 : 0) << '\t'
           << sample.secondary_contact_count << '\t';
    write_hex32(output, sample.world_node);
    output << '\t' << sample.world_triangle << '\t';
    write_slice(output, sample.vehicle_contact_state);
    output << '\t';
    write_slice(output, sample.vehicle_transform);
    output << '\t';
    write_slice(output, sample.vehicle_vectors);
    output << '\t';
    write_slice(output, sample.physics_state);
    output << '\t';
    write_slice(output, sample.physics_object_state);
    output << '\t';
    write_slice(output, sample.physics_owner_region);
    output << '\t';
    write_slice(output, sample.physics_shape_slots);
    output << '\t';
    write_slice(output, sample.primary_shape_header);
    output << '\t';
    write_slice(output, sample.secondary_shape_region);
    output << '\t';
    write_slice(output, sample.broadphase_provider_state);
    output << '\t';
    write_slice(output, sample.broadphase_query_state);
    output << '\t';
    write_slice(output, sample.broadphase_candidate_state);
    output << '\t';
    write_slice(output, sample.broadphase_candidate_descriptor_state);
    output << '\t';
    write_slice(output, sample.broadphase_owner_filter_vector);
    output << '\t';
    write_slice(output, sample.spatial_leaf_state);
    output << '\t';
    write_slice(output, sample.active_list_state);
    output << '\t';
    write_slice(output, sample.active_list_caller_config);
    output << '\t';
    write_slice(output, sample.active_list_fixed_source);
    output << '\t';
    write_slice(output, sample.contact_accumulator_tail);
    output << '\t';
    write_slice(output, sample.shape_geometry);
    output << '\t';
    write_slice(output, sample.clip_bounds);
    output << '\t';
    write_slice(output, sample.clip_vertices);
    output << '\t';
    write_slice(output, sample.clip_output);
    output << '\n';
}

void write_summary(std::ostream& output, const CollisionFrameCarSummary& summary) {
    output << "frame_car\t" << summary.vblank << '\t'
           << (!summary.role_known ? "unknown" : (summary.ai ? "ai" : "player")) << '\t';
    write_hex32(output, summary.vehicle_object);
    output << '\t';
    write_hex32(output, summary.physics_object);
    output << '\t' << summary.first_sequence << '\t' << summary.last_sequence << '\t'
           << summary.dispatches << '\t' << summary.broadphase_queries << '\t'
           << summary.broadphase_query_hits << '\t' << summary.candidate_callbacks << '\t'
           << summary.candidate_filter_probes << '\t' << summary.candidate_filter_rejects << '\t'
           << summary.candidate_append_calls << '\t' << summary.candidate_append_stores << '\t'
           << summary.candidate_append_overflows << '\t' << summary.narrowphase_calls << '\t'
           << summary.overlap_clip_calls << '\t' << summary.overlap_clip_zero << '\t'
           << summary.overlap_clip_nonzero << '\t' << summary.primary_contact_count_max << '\t'
           << summary.secondary_contact_count_max << '\t' << summary.primary_contacts_accepted << '\t'
           << summary.secondary_contacts_accepted << '\t' << summary.gather_calls << '\t'
           << summary.gather_nonzero << '\t' << summary.response_probes << '\n';
}

// One recorded step of a broadphase candidate's journey through the pipeline.
struct CollisionCandidateStage {
    std::uint64_t sequence{};
    Slus21050CollisionTraceContext context{};
    GuestAddress pc_before{};
    GuestAddress pc_after{};
    GuestAddress next_pc_after{};
    std::uint32_t step_status{0xFFFFFFFFU};
    std::uint32_t query_result{};
    std::uint32_t candidate_append_old_count{};
    std::uint32_t candidate_append_new_count{};
    std::uint32_t primary_contact_count{};
    std::uint32_t secondary_contact_count{};
    const char* label{};
    bool query_result_valid{};
    bool candidate_append_old_count_valid{};
    bool candidate_append_new_count_valid{};
    bool primary_contact_count_valid{};
    bool secondary_contact_count_valid{};
    bool complete{};
};

// Everything observed about one broadphase candidate of one AI car.
struct CollisionCandidateEvidence {
    std::uint64_t id{};
    std::uint64_t vblank{};
    GuestAddress vehicle_object{};
    GuestAddress physics_object{};
    GuestAddress broadphase_candidate{};
    GuestAddress broadphase_candidate_descriptor{};
    GuestAddress narrowphase_shape{};
    std::uint32_t normal_y_bits{};
    std::uint32_t append_index{};
    std::uint32_t clip_vertex_count{};
    bool normal_y_valid{};
    bool vertical_normal{};
    bool aabb_penetration{};
    bool append_call_seen{};
    bool append_entry_seen{};
    bool append_store_seen{};
    bool append_index_valid{};
    bool narrowphase_seen{};
    bool clip_call_seen{};
    bool clip_vertex_count_valid{};
    bool primary_contact_accepted{};
    bool secondary_contact_accepted{};
    bool filter_rejected{};
    bool finalized{};
    WordSlice candidate_state{};
    WordSlice candidate_descriptor_state{};
    WordSlice shape_geometry{};
    WordSlice clip_bounds{};
    WordSlice clip_vertices{};
    std::vector<CollisionCandidateStage> stages;
};

struct SpatialDecisionEvidence {
    CollisionCompareSample sample{};
    std::uint16_t result{};
};

struct SpatialResultAggregate {
    std::uint64_t total{};
    std::uint64_t accepted{};
    std::uint64_t rejected{};
};

enum class SnapshotCaptureFailure : std::uint8_t {
    kNone = 0,
    kInstructionMismatch = 1,
    kPointerRejection = 2,
    kCrossLink = 3,
    kPlayerIdentity = 4,
    kIncompleteRead = 5,
};

struct SnapshotCaptureResult {
    std::optional<Slus21050VehicleWorldContactSnapshot> snapshot{};
    SnapshotCaptureFailure failure{SnapshotCaptureFailure::kNone};
};

void count_snapshot_failure(Slus21050CollisionSnapshotDiagnostics& diagnostics,
                            const SnapshotCaptureFailure failure) noexcept {
    switch (failure) {
    case SnapshotCaptureFailure::kInstructionMismatch:
        ++diagnostics.instruction_mismatches;
        break;
    case SnapshotCaptureFailure::kPointerRejection:
        ++diagnostics.pointer_rejections;
        break;
    case SnapshotCaptureFailure::kCrossLink:
        ++diagnostics.cross_link_rejections;
        break;
    case SnapshotCaptureFailure::kPlayerIdentity:
        ++diagnostics.player_identity_rejections;
        break;
    case SnapshotCaptureFailure::kIncompleteRead:
        ++diagnostics.incomplete_read_rejections;
        break;
    default:
        break;
    }
}

// Rebuilds the endpoint snapshot only for the exact sequential post-state.
[[nodiscard]] std::optional<Slus21050VehicleWorldContactSnapshot> complete_world_contact_snapshot(
    const Slus21050VehicleWorldContactSnapshot& authenticated_snapshot,
    const guest::R5900State& state, const std::uint32_t step_status,
    const Slus21050CollisionTraceContext& post_context) {
    if (step_status != 0U || state.is_executing_delay_slot() || state.pc() != 0x00143844U ||
        state.next_pc() != 0x00143848U ||
        authenticated_snapshot.context.committed_steps == ~0ULL ||
        authenticated_snapshot.context.ee_cycles == ~0ULL ||
        post_context.committed_steps != authenticated_snapshot.context.committed_steps + 1U ||
        post_context.ee_cycles != authenticated_snapshot.context.ee_cycles + 1U ||
        post_context.vblank_starts != authenticated_snapshot.context.vblank_starts ||
        post_context.thread_id != authenticated_snapshot.context.thread_id) {
        return std::nullopt;
    }
    Slus21050VehicleWorldContactSnapshot completed_snapshot = authenticated_snapshot;
    completed_snapshot.context = post_context;
    completed_snapshot.complete = true;
    return completed_snapshot;
}

[[nodiscard]] std::optional<std::uint16_t> read_snapshot_half(
    const EeMemory& memory, const std::optional<GuestAddress> address) {
    if (!address || !memory.is_main_ram_range(*address, 2U)) {
        return std::nullopt;
    }
    return memory.read_u16_le(*address);
}

[[nodiscard]] SnapshotCaptureResult capture_world_contact_snapshot(
    const GuestAddress pc, const GuestAddress next_pc, const std::uint32_t instruction_word,
    const bool delay_slot, const std::uint32_t vehicle_register, const EeMemory& memory,
    const Slus21050CollisionTraceContext& context, const std::uint64_t sequence) {
    if (pc != 0x00143840U || next_pc != 0x00143844U || delay_slot ||
        instruction_word != 0x7BB30030U) {
        return {.failure = SnapshotCaptureFailure::kInstructionMismatch};
    }
    const auto player_pointer = read_snapshot_word(memory, 0x0064D388U);
    if (!player_pointer || !valid_snapshot_pointer(memory, *player_pointer) ||
        !valid_snapshot_pointer(memory, vehicle_register)) {
        return {.failure = SnapshotCaptureFailure::kPointerRejection};
    }
    if (*player_pointer != vehicle_register) {
        return {.failure = SnapshotCaptureFailure::kPlayerIdentity};
    }
    const auto physics_address = checked_add(vehicle_register, 9264U);
    const auto physics = read_snapshot_word(memory, physics_address);
    if (!physics || !valid_snapshot_pointer(memory, *physics)) {
        return {.failure = SnapshotCaptureFailure::kPointerRejection};
    }
    const auto owner_address = checked_add(*physics, 5108U);
    const auto owner = read_snapshot_word(memory, owner_address);
    if (!owner) {
        return {.failure = SnapshotCaptureFailure::kIncompleteRead};
    }
    if (!valid_snapshot_pointer(memory, *owner)) {
        return {.failure = SnapshotCaptureFailure::kPointerRejection};
    }
    if (vehicle_register != *owner) {
        return {.failure = SnapshotCaptureFailure::kCrossLink};
    }
    const auto component = read_snapshot_word(memory, checked_add(vehicle_register, 6416U));
    if (!component || !valid_snapshot_pointer(memory, *component)) {
        return {.failure = SnapshotCaptureFailure::kPointerRejection};
    }
    const auto world_node_address = checked_add(vehicle_register, 6324U);
    const auto world_node = read_snapshot_word(memory, world_node_address);
    const auto world_triangle =
        read_snapshot_half(memory, checked_add(vehicle_register, 6328U));
    if (!world_node || !world_triangle) {
        return {.failure = SnapshotCaptureFailure::kIncompleteRead};
    }
    if (*world_node != 0U && !valid_snapshot_pointer(memory, *world_node)) {
        return {.failure = SnapshotCaptureFailure::kPointerRejection};
    }
    const auto transform = capture_snapshot_words<16U>(memory, vehicle_register, 0U);
    const auto vectors = capture_snapshot_words<16U>(memory, vehicle_register, 6576U);
    const auto contact = capture_snapshot_words<8U>(memory, vehicle_register, 6320U);
    const auto component_words = capture_snapshot_words<16U>(memory, *component, 0U);
    if (!transform || !vectors || !contact || !component_words) {
        return {.failure = SnapshotCaptureFailure::kIncompleteRead};
    }
    const bool has_world_contact = *world_node != 0U;
    return {Slus21050VehicleWorldContactSnapshot{
                .sequence = sequence,
                .context = context,
                .vehicle_object = vehicle_register,
                .physics_object = *physics,
                .physics_component = *component,
                .owner_vehicle = *owner,
                .world_node = *world_node,
                .world_triangle = *world_triangle,
                .transform_words = *transform,
                .vector_words = *vectors,
                .contact_words = *contact,
                .component_words = *component_words,
                .world_node_valid = has_world_contact,
                .world_triangle_valid = has_world_contact,
                .role_valid = true,
                .complete = false},
            SnapshotCaptureFailure::kNone};
}

[[nodiscard]] std::optional<std::uint16_t> read_half(const EeMemory& memory,
                                                     const std::optional<GuestAddress> address) {
    if (!address || !memory.contains(*address, 2U)) {
        return std::nullopt;
    }
    return memory.read_u16_le(*address);
}

[[nodiscard]] std::optional<std::uint8_t> read_byte(const EeMemory& memory,
                                                    const std::optional<GuestAddress> address) {
    if (!address || !memory.contains(*address, 1U)) {
        return std::nullopt;
    }
    return memory.read_u8(*address);
}

[[nodiscard]] bool expected_fcc0(const Target& target, const std::uint32_t fs_raw,
                                 const std::uint32_t ft_raw) {
    const Comparison comparison = target.comparison;
    if (comparison == Comparison::kNone) {
        return false;
    }
    const float fs = std::bit_cast<float>(normalize_float_bits(fs_raw));
    const float ft = std::bit_cast<float>(normalize_float_bits(ft_raw));
    return comparison == Comparison::kLessEqual ? fs <= ft : fs < ft;
}

[[nodiscard]] bool is_vertical_normal(const std::uint32_t bits) {
    const float value = std::bit_cast<float>(normalize_float_bits(bits));
    return std::isfinite(value) && std::fabs(value) < 0.5F;
}

[[nodiscard]] bool is_narrowphase_label(const std::string_view label) {
    return label == "narrowphase_entry" || label.starts_with("narrowphase_") ||
           label.starts_with("overlap_clip_") || label == "contact_accumulator_selector" ||
           label == "contact_normal_reject" || label == "primary_contact_accepted" ||
           label == "secondary_contact_accepted";
}

[[nodiscard]] bool vertices_inside_bounds(const WordSlice& vertices, const WordSlice& bounds) {
    const std::array<std::size_t, 3U> kAxes{0U, 1U, 2U};
    for (const std::size_t axis : kAxes) {
        const auto vertex_0 = slice_float(vertices, axis);
        const auto vertex_1 = slice_float(vertices, axis + 4U);
        const auto vertex_2 = slice_float(vertices, axis + 8U);
        const auto bound_0 = slice_float(bounds, axis);
        const auto bound_1 = slice_float(bounds, axis + 4U);
        if (!vertex_0 || !vertex_1 || !vertex_2 || !bound_0 || !bound_1) {
            return false;
        }
        const std::array<float, 3U> minimum_values{*vertex_0, *vertex_1, *vertex_2};
        const std::array<float, 3U> maximum_values{*vertex_0, *vertex_1, *vertex_2};
        const float minimum = *std::min_element(minimum_values.begin(), minimum_values.end());
        const float maximum = *std::max_element(maximum_values.begin(), maximum_values.end());
        const float low = std::min(*bound_0, *bound_1);
        const float high = std::max(*bound_0, *bound_1);
        if (minimum < low || maximum > high) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] const char* candidate_outcome(const CollisionCandidateEvidence& evidence) {
    if (evidence.primary_contact_accepted) {
        return "primary_contact";
    }
    if (evidence.secondary_contact_accepted) {
        return "secondary_contact";
    }
    if (evidence.clip_vertex_count_valid && evidence.clip_vertex_count <= 2U) {
        return "clip_rejected";
    }
    if (evidence.narrowphase_seen) {
        return "narrowphase_no_contact";
    }
    if (evidence.append_store_seen) {
        return "appended_no_narrowphase";
    }
    if (evidence.append_call_seen) {
        return "append_incomplete";
    }
    if (evidence.append_entry_seen) {
        return "append_incomplete";
    }
    if (evidence.filter_rejected) {
        return "filter_rejected";
    }
    return "pending";
}


void write_candidate(std::ostream& output, const CollisionCandidateEvidence& pinned) {
    output << "pinned_candidate\t" << pinned.id << '\t' << (pinned.vertical_normal ? 1 : 0)
           << '\t' << (pinned.aabb_penetration ? 1 : 0) << '\t'
           << candidate_outcome(pinned) << '\t' << pinned.vblank << '\t';
    write_hex32(output, pinned.vehicle_object);
    output << '\t';
    write_hex32(output, pinned.physics_object);
    output << '\t';
    write_hex32(output, pinned.broadphase_candidate);
    output << '\t';
    write_hex32(output, pinned.broadphase_candidate_descriptor);
    output << '\t';
    write_hex32(output, pinned.narrowphase_shape);
    output << '\t' << (pinned.normal_y_valid ? 1 : 0) << '\t';
    write_hex32(output, pinned.normal_y_bits);
    output << '\t' << (pinned.append_call_seen ? 1 : 0) << '\t'
           << (pinned.append_entry_seen ? 1 : 0) << '\t'
           << (pinned.append_store_seen ? 1 : 0) << '\t'
           << (pinned.append_index_valid ? 1 : 0) << '\t' << pinned.append_index << '\t'
           << (pinned.narrowphase_seen ? 1 : 0) << '\t' << (pinned.clip_call_seen ? 1 : 0)
           << '\t' << (pinned.clip_vertex_count_valid ? 1 : 0) << '\t'
           << pinned.clip_vertex_count << '\t' << (pinned.primary_contact_accepted ? 1 : 0)
           << '\t' << (pinned.secondary_contact_accepted ? 1 : 0) << '\t'
           << (pinned.filter_rejected ? 1 : 0) << '\t';
    write_slice(output, pinned.candidate_state);
    output << '\t';
    write_slice(output, pinned.candidate_descriptor_state);
    output << '\t';
    write_slice(output, pinned.shape_geometry);
    output << '\t';
    write_slice(output, pinned.clip_bounds);
    output << '\t';
    write_slice(output, pinned.clip_vertices);
    output << '\n';
}

void write_spatial_decision(std::ostream& output, const SpatialDecisionEvidence& evidence) {
    const auto& sample = evidence.sample;
    output << "spatial_decision\t" << sample.sequence << '\t'
           << sample.context.committed_steps << '\t' << sample.context.ee_cycles << '\t'
           << sample.context.vblank_starts << '\t' << sample.context.thread_id << '\t'
           << (sample.label != nullptr ? sample.label : "") << '\t';
    write_hex32(output, sample.pc_before);
    output << '\t' << evidence.result << '\t';
    write_hex32(output, sample.vehicle_object);
    output << '\t';
    write_hex32(output, sample.physics_object);
    output << '\t';
    write_hex32(output, sample.spatial_leaf_pointer);
    output << '\t';
    write_hex32(output, sample.broadphase_callback_pointer);
    output << '\t';
    write_hex32(output, sample.gprs[16].words[0]);
    output << '\t';
    write_hex32(output, sample.gprs[18].words[0]);
    output << '\t' << (sample.complete ? 1 : 0) << '\t' << sample.step_status << '\t';
    write_hex32(output, sample.pc_after);
    output << '\t';
    write_hex32(output, sample.next_pc_after);
    output << '\t';
    bool first = true;
    for (const auto& vector : sample.vu0_vectors) {
        for (const auto word : vector.words) {
            if (!first) {
                output << ';';
            }
            first = false;
            write_hex32(output, word);
        }
    }
    output << '\t';
    write_u16_array(output, sample.vu0_integers);
    output << '\t';
    for (std::size_t index = 0; index < sample.vu0_controls.size(); ++index) {
        if (index != 0U) {
            output << ';';
        }
        write_hex32(output, sample.vu0_controls[index]);
    }
    output << '\t';
    for (std::size_t index = 0; index < sample.vu0_accumulator.size(); ++index) {
        if (index != 0U) {
            output << ';';
        }
        write_hex32(output, sample.vu0_accumulator[index]);
    }
    output << '\t';
    write_slice(output, sample.broadphase_query_state);
    output << '\t';
    write_slice(output, sample.spatial_leaf_state);
    output << '\n';
}
}  // namespace

struct Slus21050CollisionTrace::Impl {
    Impl(std::filesystem::path output_path, const Slus21050CollisionTraceConfig& requested)
        : output_path(std::move(output_path)),
          partial_path(this->output_path.string() + ".partial"),
          config(requested) {
        if (this->output_path.empty()) {
            throw std::invalid_argument("collision trace output path is empty");
        }
        if (config.first_capacity == 0U || config.tail_capacity == 0U) {
            throw std::invalid_argument("collision trace capacity must not be zero");
        }
        std::error_code error;
        if (std::filesystem::exists(this->output_path, error) || error) {
            throw std::invalid_argument("refusing to overwrite collision trace: " +
                                        this->output_path.string());
        }
        if (std::filesystem::exists(partial_path, error) || error) {
            throw std::invalid_argument("refusing to overwrite partial collision trace: " +
                                        partial_path.string());
        }
        const auto parent = this->output_path.parent_path();
        if (!parent.empty() &&
            (!std::filesystem::exists(parent, error) || error ||
             !std::filesystem::is_directory(parent, error) || error)) {
            throw std::invalid_argument("collision trace parent directory is unavailable: " +
                                        parent.string());
        }
        first.reserve(config.first_capacity);
        tail.resize(config.tail_capacity);
    }

    std::filesystem::path output_path;
    std::filesystem::path partial_path;
    Slus21050CollisionTraceConfig config;
    std::vector<CollisionCompareSample> first;
    std::vector<CollisionCompareSample> tail;
    std::size_t tail_count{};
    std::size_t tail_next{};
    std::uint64_t total{};
    std::optional<std::uint64_t> current_vblank;
    std::vector<CollisionFrameCarSummary> current_summaries;
    std::vector<CollisionFrameCarSummary> summaries;
    std::vector<CollisionCandidateEvidence> frame_candidates;
    std::optional<CollisionCandidateEvidence> pinned_candidate;
    std::array<SpatialResultAggregate, 4U> spatial_results{};
    std::vector<SpatialDecisionEvidence> spatial_evidence;
    std::size_t spatial_evidence_next{};
    std::optional<Slus21050VehicleWorldContactSnapshot> latest_vehicle_snapshot;
    Slus21050CollisionSnapshotDiagnostics snapshot_diagnostics{};
    std::uint64_t current_callback_candidate{~0ULL};
    std::uint64_t current_append_candidate{~0ULL};
    std::uint64_t current_narrowphase_candidate{~0ULL};
    std::uint32_t active_query_vehicle{};
    std::uint32_t active_query_physics{};
    bool finalized{};

    // Closes the current frame: finalizes every candidate and rolls the per-car
    // summaries into the retained list.
    void flush_frame() {
        for (auto& evidence : frame_candidates) {
            finalize_candidate(evidence);
        }
        summaries.insert(summaries.end(), current_summaries.begin(), current_summaries.end());
        current_summaries.clear();
        frame_candidates.clear();
        current_callback_candidate = ~0ULL;
        current_append_candidate = ~0ULL;
        current_narrowphase_candidate = ~0ULL;
    }

    CollisionFrameCarSummary& summary_for(const CollisionCompareSample& sample,
                                          const bool role_known, const bool ai) {
        const auto found = std::find_if(
            current_summaries.begin(), current_summaries.end(),
            [&sample](const CollisionFrameCarSummary& candidate) {
                if (sample.vehicle_object != 0U) {
                    return candidate.vehicle_object == sample.vehicle_object;
                }
                return sample.physics_object != 0U &&
                       candidate.physics_object == sample.physics_object;
            });
        if (found != current_summaries.end()) {
            found->last_sequence = sample.sequence;
            if (found->vehicle_object == 0U) {
                found->vehicle_object = sample.vehicle_object;
            }
            if (found->physics_object == 0U) {
                found->physics_object = sample.physics_object;
            }
            found->role_known = found->role_known || role_known;
            if (role_known) {
                found->ai = ai;
            }
            return *found;
        }
        current_summaries.push_back(CollisionFrameCarSummary{
            .vblank = sample.context.vblank_starts,
            .first_sequence = sample.sequence,
            .last_sequence = sample.sequence,
            .vehicle_object = sample.vehicle_object,
            .physics_object = sample.physics_object,
            .role_known = role_known,
            .ai = ai});
        return current_summaries.back();
    }

    void observe_summary(const CollisionCompareSample& sample, const bool role_known,
                         const bool ai) {
        if (sample.vehicle_object == 0U && sample.physics_object == 0U) {
            return;
        }
        auto& summary = summary_for(sample, role_known, ai);
        const std::string_view label{sample.label != nullptr ? sample.label : ""};
        if (label == "object_physics_dispatch") {
            ++summary.dispatches;
        } else if (label == "broadphase_query_call") {
            ++summary.broadphase_queries;
        } else if (label == "broadphase_query_result" && sample.query_result_valid &&
                   sample.query_result != 0U) {
            ++summary.broadphase_query_hits;
        } else if (label == "broadphase_candidate_callback") {
            ++summary.candidate_callbacks;
        } else if (is_candidate_filter_label(label)) {
            ++summary.candidate_filter_probes;
        } else if (label == "broadphase_candidate_append_call") {
            ++summary.candidate_append_calls;
        } else if (label == "candidate_append_count_store") {
            ++summary.candidate_append_stores;
        } else if (label == "candidate_append_entry" && sample.candidate_append_overflow_latch_valid &&
                   sample.candidate_append_overflow_latch != 0U) {
            ++summary.candidate_append_overflows;
        } else if (label == "narrowphase_entry") {
            ++summary.narrowphase_calls;
        } else if (label == "overlap_clip_call") {
            ++summary.overlap_clip_calls;
        } else if (label == "overlap_clip_vertex_count" && sample.query_result_valid) {
            if (sample.query_result == 0U) {
                ++summary.overlap_clip_zero;
            } else {
                ++summary.overlap_clip_nonzero;
            }
        } else if (label == "primary_contact_accepted") {
            ++summary.primary_contacts_accepted;
        } else if (label == "secondary_contact_accepted") {
            ++summary.secondary_contacts_accepted;
        } else if (label == "contact_gather_call") {
            ++summary.gather_calls;
        } else if (label == "contact_gather_result" && sample.query_result_valid &&
                   sample.query_result != 0U) {
            ++summary.gather_nonzero;
        } else if (label.starts_with("response_compare_")) {
            ++summary.response_probes;
        }
        if (sample.primary_contact_count_valid) {
            summary.primary_contact_count_max =
                std::max(summary.primary_contact_count_max, sample.primary_contact_count);
        }
        if (sample.secondary_contact_count_valid) {
            summary.secondary_contact_count_max =
                std::max(summary.secondary_contact_count_max, sample.secondary_contact_count);
        }
    }

    void add_stage(CollisionCompareSample& sample, CollisionCandidateEvidence& candidate) {
        sample.diagnostic_candidate_id = candidate.id;
        candidate.stages.push_back(CollisionCandidateStage{
            .sequence = sample.sequence,
            .context = sample.context,
            .pc_before = sample.pc_before,
            .query_result = sample.query_result,
            .candidate_append_old_count = sample.candidate_append_old_count,
            .candidate_append_new_count = sample.candidate_append_new_count,
            .primary_contact_count = sample.primary_contact_count,
            .secondary_contact_count = sample.secondary_contact_count,
            .label = sample.label,
            .query_result_valid = sample.query_result_valid,
            .candidate_append_old_count_valid = sample.candidate_append_old_count_valid,
            .candidate_append_new_count_valid = sample.candidate_append_new_count_valid,
            .primary_contact_count_valid = sample.primary_contact_count_valid,
            .secondary_contact_count_valid = sample.secondary_contact_count_valid});
        refresh_pinned(candidate);
    }

    void refresh_pinned(const CollisionCandidateEvidence& evidence) {
        if (pinned_candidate && pinned_candidate->id == evidence.id) {
            *pinned_candidate = evidence;
        }
    }

    void maybe_pin(CollisionCandidateEvidence& candidate) {
        if (!candidate.vertical_normal && !candidate.aabb_penetration) {
            return;
        }
        if (!pinned_candidate) {
            pinned_candidate = candidate;
        } else if (pinned_candidate->id == candidate.id) {
            *pinned_candidate = candidate;
        }
    }

    CollisionCandidateEvidence* find_candidate(const std::uint64_t id) {
        if (id == ~0ULL) {
            return nullptr;
        }
        const auto it =
            std::find_if(frame_candidates.begin(), frame_candidates.end(),
                         [id](const CollisionCandidateEvidence& candidate) {
                             return candidate.id == id;
                         });
        return it == frame_candidates.end() ? nullptr : &*it;
    }

    // Back-fills the post-instruction state into the stage recorded for this sample.
    void complete_stage(const CollisionCompareSample& sample) {
        CollisionCandidateEvidence* candidate = find_candidate(sample.diagnostic_candidate_id);
        if (candidate == nullptr) {
            return;
        }
        const auto stage = std::find_if(candidate->stages.rbegin(), candidate->stages.rend(),
                                        [&sample](const CollisionCandidateStage& entry) {
                                            return entry.sequence == sample.sequence;
                                        });
        if (stage == candidate->stages.rend()) {
            return;
        }
        stage->pc_after = sample.pc_after;
        stage->next_pc_after = sample.next_pc_after;
        stage->step_status = sample.step_status;
        stage->complete = sample.complete;
        if (pinned_candidate && pinned_candidate->id == candidate->id) {
            *pinned_candidate = *candidate;
        }
    }

    CollisionFrameCarSummary* find_summary(const GuestAddress vehicle, const GuestAddress physics) {
        const auto it = std::find_if(
            current_summaries.begin(), current_summaries.end(),
            [vehicle, physics](const CollisionFrameCarSummary& summary) {
                return summary.vehicle_object == vehicle && summary.physics_object == physics;
            });
        return it == current_summaries.end() ? nullptr : &*it;
    }

    void finalize_candidate(CollisionCandidateEvidence& evidence) {
        if (evidence.finalized) {
            return;
        }
        evidence.filter_rejected =
            !(evidence.append_call_seen || evidence.append_entry_seen || evidence.append_store_seen);
        evidence.finalized = true;
        if (evidence.filter_rejected) {
            if (auto* summary = find_summary(evidence.vehicle_object, evidence.physics_object);
                summary != nullptr) {
                ++summary->candidate_filter_rejects;
            }
        }
        refresh_pinned(evidence);
    }
};

Slus21050CollisionTrace::Slus21050CollisionTrace(std::filesystem::path output_path,
                                                 const Slus21050CollisionTraceConfig config)
    : impl_(std::make_unique<Impl>(std::move(output_path), config)) {}

Slus21050CollisionTrace::~Slus21050CollisionTrace() = default;

Slus21050CollisionTrace::Slus21050CollisionTrace(Slus21050CollisionTrace&&) noexcept = default;

Slus21050CollisionTrace& Slus21050CollisionTrace::operator=(Slus21050CollisionTrace&&) noexcept =
    default;

std::optional<std::uint32_t> Slus21050CollisionTrace::expected_instruction(
    const guest::GuestAddress pc) noexcept {
    const Target* target = find_target(pc);
    return target == nullptr ? std::nullopt : std::optional<std::uint32_t>{target->word};
}

bool Slus21050CollisionTrace::is_target(const guest::GuestAddress pc) noexcept {
    return find_target(pc) != nullptr;
}
std::optional<Slus21050CollisionTrace::SampleToken> Slus21050CollisionTrace::begin_sample(
    const guest::R5900State& state, const guest::EeMemory& memory,
    const Slus21050CollisionTraceContext& context) {
    if (!impl_ || impl_->finalized) {
        throw std::logic_error("collision trace is not writable");
    }
    const Target* target = find_target(state.pc());
    if (target == nullptr) {
        return std::nullopt;
    }

    CollisionCompareSample sample{};
    sample.sequence = impl_->total++;
    sample.context = context;
    sample.pc_before = state.pc();
    sample.next_pc_before = state.next_pc();
    sample.delay_slot_before = state.is_executing_delay_slot();
    sample.expected_word = target->word;
    sample.label = target->label;
    sample.instruction_word =
        memory.contains(state.pc(), 4U) ? memory.read_u32_le(state.pc()) : 0U;
    sample.fcr31_before = state.fcr31_bits();
    sample.fpu_accumulator = state.fpu_accumulator_bits();
    sample.fs = target->fs;
    sample.ft = target->ft;
    sample.is_float_comparison = target->comparison != Comparison::kNone;
    sample.less_equal = target->comparison == Comparison::kLessEqual;
    if (target->comparison != Comparison::kNone) {
        sample.fs_raw = state.read_fpr_bits(target->fs);
        sample.ft_raw = state.read_fpr_bits(target->ft);
        sample.fs_normalized = normalize_float_bits(sample.fs_raw);
        sample.ft_normalized = normalize_float_bits(sample.ft_raw);
    }
    sample.expected_fcc0 = expected_fcc0(*target, sample.fs_raw, sample.ft_raw);
    for (std::size_t index = 0; index < sample.gprs.size(); ++index) {
        sample.gprs[index] = state.read_gpr(index);
        sample.fprs[index] = state.read_fpr_bits(index);
    }
    for (std::size_t index = 0; index < sample.vu0_vectors.size(); ++index) {
        sample.vu0_vectors[index] = state.read_vu0_vector(index);
    }
    for (std::size_t index = 0; index < sample.vu0_integers.size(); ++index) {
        sample.vu0_integers[index] = state.read_vu0_integer(index);
    }
    for (std::size_t index = 0; index < sample.vu0_controls.size(); ++index) {
        sample.vu0_controls[index] = state.read_vu0_control(index);
    }
    sample.vu0_accumulator = state.vu0_accumulator().words;

    const GuestAddress pc = state.pc();
    const std::uint32_t sp = state.read_gpr_u32(29U);
    const std::uint32_t a0 = state.read_gpr_u32(4U);
    const std::uint32_t a1 = state.read_gpr_u32(5U);
    const std::uint32_t a2 = state.read_gpr_u32(6U);
    const std::uint32_t a3 = state.read_gpr_u32(7U);
    const std::uint32_t t0 = state.read_gpr_u32(8U);
    const std::uint32_t t2 = state.read_gpr_u32(10U);
    sample.stack = capture_slice(memory, checked_add(sp, 224U), 16U);
    sample.ring_header = capture_slice(memory, checked_add(a1, 64U), 3U);
    sample.candidate = capture_slice(memory, a2, 4U);
    sample.output_slot = capture_slice(memory, t2, 8U);

    std::optional<GuestAddress> coefficient_base;
    if (pc >= 0x00263CF0U && pc <= 0x00263D00U) {
        const auto pointer_address = checked_add(a0, 152U);
        coefficient_base = read_word(memory, pointer_address);
    } else if (pc >= 0x00263D44U && pc <= 0x00263D70U) {
        coefficient_base = a3;
    }
    sample.coefficients = capture_slice(
        memory, coefficient_base ? checked_add(*coefficient_base, 80U) : std::nullopt, 2U);

    const bool active_list = pc == 0x0013472CU || pc == 0x00199EB0U || pc == 0x00199EFCU ||
                             pc == 0x00199F08U || pc == 0x00199F18U || pc == 0x00199F28U ||
                             pc == 0x00199F94U || pc == 0x00199FD0U || pc == 0x0019A0C0U ||
                             pc == 0x0019A100U || pc == 0x0019A108U || pc == 0x0019A15CU ||
                             pc == 0x0019A1C4U || pc == 0x0019A38CU || pc == 0x0019A674U ||
                             pc == 0x0019B3C8U;
    if (active_list) {
        sample.active_list_state = capture_slice(memory, 0x0064D37CU, 16U);
        sample.active_list_fixed_source = capture_slice(memory, 0x01E32AE0U, 16U);
        if (pc == 0x0013472CU) {
            sample.active_list_caller_config =
                capture_slice(memory, checked_add(state.read_gpr_u32(17U), 160U), 16U);
        }
        sample.active_list_store_valid = true;
        switch (pc) {
        case 0x00199EFCU:
        case 0x0019A38CU:
        case 0x0019B3C8U:
            sample.active_list_store_address = 0x0064D37CU;
            sample.active_list_store_value = 0U;
            break;
        case 0x00199F28U:
            sample.active_list_store_address = 0x0064D37CU;
            sample.active_list_store_value = a1;
            break;
        case 0x00199F94U:
            sample.active_list_store_address = a3;
            sample.active_list_store_value = a1;
            break;
        case 0x00199FD0U:
            sample.active_list_store_address = a3;
            sample.active_list_store_value = state.read_gpr_u32(3U);
            break;
        case 0x0019A0C0U:
            if (t2 >= 0x52D8U) {
                sample.active_list_store_address = t2 - 0x52D8U;
                sample.active_list_store_value = state.read_gpr_u32(11U);
            } else {
                sample.active_list_store_valid = false;
            }
            break;
        case 0x0019A108U:
            if (state.read_gpr_u32(9U) >= 0x52E4U) {
                sample.active_list_store_address = state.read_gpr_u32(9U) - 0x52E4U;
                sample.active_list_store_value = t0;
            } else {
                sample.active_list_store_valid = false;
            }
            break;
        case 0x0019A15CU:
            if (state.read_gpr_u32(9U) >= 0x52D8U) {
                sample.active_list_store_address = state.read_gpr_u32(9U) - 0x52D8U;
                sample.active_list_store_value = state.read_gpr_u32(12U);
            } else {
                sample.active_list_store_valid = false;
            }
            break;
        default:
            sample.active_list_store_valid = false;
            break;
        }
    }

    const bool at_dispatch = pc == 0x0019A660U;
    const bool at_world_contact_dispatch = pc == 0x00143CB8U;
    if (at_dispatch || at_world_contact_dispatch) {
        sample.vehicle_object = at_dispatch ? a0 : state.read_gpr_u32(16U);
        sample.dispatched_method = state.read_gpr_u32(25U);
        const auto component_address = checked_add(sample.vehicle_object, 6416U);
        if (component_address && memory.contains(*component_address, 4U)) {
            sample.physics_component = memory.read_u32_le(*component_address);
        }
        const auto method_address =
            checked_add(sample.physics_component, at_dispatch ? 12U : 44U);
        if (sample.physics_component != 0U && method_address &&
            memory.contains(*method_address, 4U)) {
            sample.physics_method_matches =
                sample.dispatched_method == memory.read_u32_le(*method_address);
        }
    } else if (pc >= 0x001423E0U && pc <= 0x00143840U) {
        sample.vehicle_object = pc == 0x001423E0U ? a0 : state.read_gpr_u32(17U);
    }

    if (pc == 0x001AB090U || pc == 0x001AB108U) {
        sample.physics_object = a0;
    } else if (pc >= 0x001C5168U && pc <= 0x001C51D0U) {
        sample.physics_object = state.read_gpr_u32(16U);
    } else if (pc >= 0x00301B00U && pc <= 0x00301BB0U) {
        sample.physics_object = t0;
    } else if (pc >= 0x001C5330U && pc <= 0x001C5408U) {
        sample.physics_object = a1;
    } else if (pc == 0x001C541CU) {
        sample.physics_object = a0;
    } else if (pc >= 0x001BD6F8U && pc <= 0x001BD718U) {
        sample.physics_object = state.read_gpr_u32(19U);
    } else if (pc >= 0x001C4AC0U && pc <= 0x001C4AC8U) {
        sample.physics_object = state.read_gpr_u32(18U);
    } else if (pc == 0x001C5430U) {
        sample.physics_object = a0;
    } else if (pc >= 0x001C5484U && pc <= 0x001C5664U) {
        sample.physics_object = state.read_gpr_u32(20U);
    } else if (pc == 0x001C6210U) {
        sample.physics_object = a0;
    } else if (pc >= 0x001C6248U && pc <= 0x001C65D0U) {
        sample.physics_object = state.read_gpr_u32(18U);
    }

    if (pc >= 0x001C5580U && pc <= 0x001C5664U) {
        sample.contact_accumulator = checked_add(sp, 3120U).value_or(0U);
    } else if (pc == 0x001C6210U) {
        sample.contact_accumulator = a3;
    } else if (pc >= 0x001C6248U && pc <= 0x001C65D0U) {
        sample.contact_accumulator = state.read_gpr_u32(16U);
    }

    if (sample.physics_object != 0U) {
        sample.owner_vehicle =
            read_word(memory, checked_add(sample.physics_object, 5108U)).value_or(0U);
    }
    if (sample.vehicle_object == 0U) {
        sample.vehicle_object = sample.owner_vehicle;
    }
    if (sample.vehicle_object != 0U) {
        const auto world_node_address = checked_add(sample.vehicle_object, 6324U);
        if (world_node_address && memory.contains(*world_node_address, 4U)) {
            sample.world_node = memory.read_u32_le(*world_node_address);
        }
        const auto triangle_address = checked_add(sample.vehicle_object, 6328U);
        if (triangle_address && memory.contains(*triangle_address, 2U)) {
            sample.world_triangle = memory.read_u16_le(*triangle_address);
        }
        const auto physics_address = checked_add(sample.vehicle_object, 9264U);
        if (physics_address && memory.contains(*physics_address, 4U)) {
            const std::uint32_t physics = memory.read_u32_le(*physics_address);
            if (sample.physics_object == 0U) {
                sample.physics_object = physics;
            }
        }
        sample.vehicle_contact_state =
            capture_slice(memory, checked_add(sample.vehicle_object, 6320U), 8U);
        sample.vehicle_transform = capture_slice(memory, sample.vehicle_object, 8U);
        sample.vehicle_vectors =
            capture_slice(memory, checked_add(sample.vehicle_object, 6576U), 8U);
    }
    if (sample.physics_object != 0U && sample.owner_vehicle == 0U) {
        sample.owner_vehicle =
            read_word(memory, checked_add(sample.physics_object, 5108U)).value_or(0U);
    }
    if (sample.owner_vehicle != 0U) {
        const auto mode = read_word(memory, checked_add(sample.owner_vehicle, 6420U));
        sample.owner_collision_mode_valid = mode.has_value();
        sample.owner_collision_mode = mode.value_or(0U);
    }
    sample.physics_state = capture_slice(
        memory,
        sample.physics_component == 0U ? std::nullopt
                                       : std::optional<std::uint32_t>{sample.physics_component},
        16U);
    sample.physics_object_state = capture_slice(
        memory,
        sample.physics_object == 0U ? std::nullopt
                                    : std::optional<std::uint32_t>{sample.physics_object},
        4U);
    sample.physics_owner_region = capture_slice(
        memory,
        sample.physics_object == 0U ? std::nullopt : checked_add(sample.physics_object, 5072U),
        16U);
    sample.physics_shape_slots = capture_slice(
        memory,
        sample.physics_object == 0U ? std::nullopt : checked_add(sample.physics_object, 512U),
        16U);
    const auto primary_shape_container = read_word(
        memory,
        sample.physics_object == 0U ? std::nullopt : checked_add(sample.physics_object, 512U));
    sample.primary_shape_container = primary_shape_container.value_or(0U);
    const auto primary_shape_count =
        primary_shape_container ? read_word(memory, *primary_shape_container) : std::nullopt;
    sample.primary_shape_count_valid = primary_shape_count.has_value();
    sample.primary_shape_count = primary_shape_count.value_or(0U);
    sample.primary_shape_header = capture_slice(memory, primary_shape_container, 8U);
    const auto secondary_shape_count = read_word(
        memory,
        sample.physics_object == 0U ? std::nullopt : checked_add(sample.physics_object, 13584U));
    sample.secondary_shape_count_valid = secondary_shape_count.has_value();
    sample.secondary_shape_count = secondary_shape_count.value_or(0U);
    sample.secondary_shape_region = capture_slice(
        memory,
        sample.physics_object == 0U ? std::nullopt : checked_add(sample.physics_object, 13568U),
        4U);
    if (pc == 0x001C51C8U) {
        sample.broadphase_provider = state.read_gpr_u32(2U) + state.read_gpr_u32(1U);
    } else if (pc == 0x00301B00U || pc == 0x00301B2CU || pc == 0x00301B5CU ||
               pc == 0x00301B6CU) {
        sample.broadphase_provider = a0;
    }
    sample.broadphase_provider_state = capture_slice(
        memory,
        sample.broadphase_provider == 0U
            ? std::nullopt
            : std::optional<std::uint32_t>{sample.broadphase_provider},
        12U);
    const bool has_slot = pc == 0x001C51C8U || pc == 0x00301B00U || pc == 0x00301B2CU ||
                          pc == 0x00301B5CU || pc == 0x00301B6CU || pc == 0x00301B90U ||
                          pc == 0x00301BB0U;
    sample.broadphase_slot_valid = has_slot;
    if (sample.broadphase_slot_valid) {
        sample.broadphase_slot = a1;
    }
    if (sample.broadphase_provider != 0U && sample.broadphase_slot_valid &&
        static_cast<std::int32_t>(sample.broadphase_slot) >= 0) {
        const auto slot_page =
            read_word(memory, checked_add(sample.broadphase_provider + 380ULL,
                                          sample.broadphase_slot * 4ULL));
        sample.broadphase_slot_page_valid = slot_page.has_value();
        sample.broadphase_slot_page = slot_page.value_or(0U);
    }
    if (pc == 0x001C51C8U || pc == 0x00301B00U) {
        sample.broadphase_query_pointer = a2;
        sample.broadphase_callback_pointer = a3;
    } else if (pc == 0x00301B4CU) {
        sample.broadphase_traversal_root = a0;
        sample.broadphase_query_pointer = a1;
        sample.broadphase_callback_pointer = a2;
    } else if (pc == 0x003BEA68U || pc == 0x003BED1CU) {
        sample.broadphase_traversal_root = state.read_gpr_u32(18U);
        sample.broadphase_query_pointer = checked_add(sp, 2032U).value_or(0U);
        sample.broadphase_callback_pointer = state.read_gpr_u32(17U);
    } else if (pc == 0x003BEE2CU || pc == 0x003BEE54U) {
        sample.broadphase_traversal_root = state.read_gpr_u32(18U);
        sample.broadphase_query_pointer = checked_add(sp, 2032U).value_or(0U);
        sample.broadphase_callback_pointer = state.read_gpr_u32(17U);
        sample.spatial_leaf_pointer = state.read_gpr_u32(2U) + state.read_gpr_u32(3U);
    } else if (pc == 0x00273070U || pc == 0x00274ED0U) {
        sample.broadphase_query_pointer = a0;
        sample.broadphase_callback_pointer = a2;
        sample.spatial_leaf_pointer = a1;
    } else if (pc >= 0x002732D4U && pc <= 0x00273378U) {
        sample.broadphase_query_pointer = checked_add(sp, 512U).value_or(0U);
        sample.broadphase_callback_pointer = state.read_gpr_u32(22U);
        sample.spatial_leaf_pointer = state.read_gpr_u32(20U);
    } else if (pc >= 0x002750A4U && pc <= 0x002751C0U) {
        sample.broadphase_callback_pointer = state.read_gpr_u32(20U);
        sample.spatial_leaf_pointer = state.read_gpr_u32(17U);
    }
    sample.broadphase_query_state = capture_slice(
        memory,
        sample.broadphase_query_pointer == 0U
            ? std::nullopt
            : std::optional<std::uint32_t>{sample.broadphase_query_pointer},
        16U);
    sample.spatial_leaf_state = capture_slice(
        memory,
        sample.spatial_leaf_pointer == 0U
            ? std::nullopt
            : std::optional<std::uint32_t>{sample.spatial_leaf_pointer},
        8U);
    if (pc == 0x00301B90U) {
        sample.broadphase_page_base = a0;
        sample.broadphase_page_state_valid = true;
        sample.broadphase_page_state = state.read_gpr_u32(3U);
        sample.broadphase_expected_page_state = state.read_gpr_u32(2U);
        const auto aliased_page_state = read_word(memory, a0 | 0x20000000U);
        sample.broadphase_page_state_memory_valid = aliased_page_state.has_value();
        sample.broadphase_page_state_memory = aliased_page_state.value_or(0U);
    }
    if (pc == 0x001C5330U) {
        sample.broadphase_candidate = a0;
    } else if (pc >= 0x001C534CU && pc <= 0x001C5408U) {
        sample.broadphase_candidate = a2;
    } else if (pc == 0x001C541CU) {
        sample.broadphase_candidate = a2;
    } else if (pc == 0x001AB090U) {
        sample.broadphase_candidate = a1;
    }
    sample.broadphase_candidate_state = capture_slice(
        memory,
        sample.broadphase_candidate == 0U
            ? std::nullopt
            : std::optional<std::uint32_t>{sample.broadphase_candidate},
        16U);
    if (sample.broadphase_candidate != 0U) {
        sample.broadphase_candidate_descriptor =
            read_word(memory, checked_add(sample.broadphase_candidate, 96U)).value_or(0U);
    }
    sample.broadphase_candidate_descriptor_state = capture_slice(
        memory,
        sample.broadphase_candidate_descriptor == 0U
            ? std::nullopt
            : std::optional<std::uint32_t>{sample.broadphase_candidate_descriptor},
        16U);
    sample.broadphase_owner_filter_vector = capture_slice(
        memory,
        sample.broadphase_candidate != 0U && sample.physics_object != 0U
            ? checked_add(sample.physics_object, 176U)
            : std::nullopt,
        8U);
    const auto candidate_type_flags = read_half(
        memory, sample.broadphase_candidate_descriptor == 0U
                    ? std::nullopt
                    : checked_add(sample.broadphase_candidate_descriptor, 4U));
    sample.broadphase_candidate_type_flags_valid = candidate_type_flags.has_value();
    sample.broadphase_candidate_type_flags = candidate_type_flags.value_or(0U);
    if (pc == 0x001AB090U) {
        const std::uint32_t overflow_base = state.read_gpr_u32(28U);
        const std::optional<std::uint32_t> overflow_address =
            overflow_base > 0x5F6BU ? std::optional<std::uint32_t>{overflow_base - 0x5F6CU}
                                    : std::nullopt;
        const auto overflow = read_byte(memory, overflow_address);
        sample.candidate_append_overflow_latch_valid = overflow.has_value();
        sample.candidate_append_overflow_latch = overflow.value_or(0U);
    }
    if (pc == 0x001AB090U || pc == 0x001C541CU || pc == 0x001AB108U) {
        sample.candidate_append_old_count_valid = primary_shape_count.has_value();
        sample.candidate_append_old_count = primary_shape_count.value_or(0U);
        if (pc == 0x001AB108U) {
            const auto old_count = read_word(memory, state.read_gpr_u32(3U));
            sample.candidate_append_old_count_valid = old_count.has_value();
            sample.candidate_append_old_count = old_count.value_or(0U);
            sample.candidate_append_new_count_valid = true;
            sample.candidate_append_new_count = a2;
        }
    }
    sample.query_result = state.read_gpr_u32(2U);
    sample.query_result_valid = pc == 0x001C4AC8U || pc == 0x001C5654U || pc == 0x001C5664U ||
                                pc == 0x001C6490U || pc == 0x001C6494U || pc == 0x001C51D0U;
    if (sample.contact_accumulator != 0U) {
        const auto primary = read_word(memory, checked_add(sample.contact_accumulator, 208U));
        const auto secondary = read_word(memory, checked_add(sample.contact_accumulator, 212U));
        sample.primary_contact_count_valid = primary.has_value();
        sample.secondary_contact_count_valid = secondary.has_value();
        sample.primary_contact_count = primary.value_or(0U);
        sample.secondary_contact_count = secondary.value_or(0U);
        sample.contact_accumulator_tail =
            capture_slice(memory, checked_add(sample.contact_accumulator, 192U), 16U);
    }
    if (pc == 0x001C5580U || pc == 0x001C55F0U || pc == 0x001C6210U) {
        sample.shape_geometry = capture_slice(memory, a1, 16U);
    } else if (pc == 0x001C5588U) {
        const auto container = read_word(
            memory,
            sample.physics_object == 0U ? std::nullopt : checked_add(sample.physics_object, 512U));
        const auto shape_base =
            container ? read_word(memory, checked_add(*container, 4U)) : std::nullopt;
        sample.shape_geometry = capture_slice(
            memory,
            shape_base ? checked_add(*shape_base, state.read_gpr_u32(16U) * 64ULL) : std::nullopt,
            16U);
    } else if (pc == 0x001C55F8U) {
        const std::uint32_t shape_owner = state.read_gpr_u32(18U);
        sample.shape_geometry = capture_slice(
            memory,
            shape_owner == 0U
                ? std::nullopt
                : checked_add(shape_owner + 5520ULL, state.read_gpr_u32(19U) * 64ULL),
            16U);
    } else if (pc >= 0x001C6248U && pc <= 0x001C65D0U) {
        sample.shape_geometry = capture_slice(memory, state.read_gpr_u32(17U), 16U);
    }
    if (pc >= 0x001C6248U && pc <= 0x001C65D0U) {
        sample.clip_bounds = capture_slice(
            memory,
            sample.contact_accumulator == 0U ? std::nullopt
                                             : checked_add(sample.contact_accumulator, 176U),
            16U);
        sample.clip_vertices = capture_slice(memory, checked_add(sp, 1072U), 16U);
        sample.clip_output = capture_slice(memory, checked_add(sp, 832U), 16U);
    }
    if (pc == 0x00143840U) {
        ++impl_->snapshot_diagnostics.return_observations;
        sample.world_contact_vehicle_register = state.read_gpr_u32(17U);
        const auto capture = capture_world_contact_snapshot(
            sample.pc_before, sample.next_pc_before, sample.instruction_word,
            sample.delay_slot_before, sample.world_contact_vehicle_register, memory, context,
            sample.sequence);
        if (capture.snapshot) {
            sample.vehicle_snapshot = capture.snapshot;
        } else {
            count_snapshot_failure(impl_->snapshot_diagnostics, capture.failure);
        }
    }

    {
        const std::string_view label{sample.label != nullptr ? sample.label : ""};
        if (label == "broadphase_query_call") {
            impl_->active_query_vehicle = sample.vehicle_object;
            impl_->active_query_physics = sample.physics_object;
        } else {
            if (impl_->active_query_vehicle != 0U &&
                (label.starts_with("spatial_") || label == "broadphase_query_result")) {
                if (sample.vehicle_object == 0U) {
                    sample.vehicle_object = impl_->active_query_vehicle;
                }
                if (sample.physics_object == 0U) {
                    sample.physics_object = impl_->active_query_physics;
                }
            }
            if (label == "broadphase_query_result") {
                impl_->active_query_vehicle = 0U;
                impl_->active_query_physics = 0U;
            }
        }
    }
    const std::uint64_t vblank = context.vblank_starts;
    if (!impl_->current_vblank) {
        impl_->current_vblank = vblank;
    } else if (*impl_->current_vblank != vblank) {
        impl_->flush_frame();
        impl_->current_vblank = vblank;
    }
    const auto player_vehicle = read_word(memory, 0x0064D388U);
    const bool role_known = sample.vehicle_object != 0U && player_vehicle.has_value();
    const bool ai = role_known && sample.vehicle_object != *player_vehicle;
    impl_->observe_summary(sample, role_known, ai);
    {
        const std::string_view label{sample.label != nullptr ? sample.label : ""};
        if (label == "broadphase_candidate_callback") {
            if (impl_->current_callback_candidate != ~0ULL) {
                auto* previous = impl_->find_candidate(impl_->current_callback_candidate);
                if (previous != nullptr && !previous->append_call_seen) {
                    impl_->finalize_candidate(*previous);
                }
            }
            impl_->current_callback_candidate = ~0ULL;
            impl_->current_append_candidate = ~0ULL;
            if (role_known && ai && sample.vehicle_object != 0U && sample.physics_object != 0U) {
                CollisionCandidateEvidence candidate{
                    .id = sample.sequence,
                    .vblank = sample.context.vblank_starts,
                    .vehicle_object = sample.vehicle_object,
                    .physics_object = sample.physics_object,
                    .broadphase_candidate = sample.broadphase_candidate,
                    .broadphase_candidate_descriptor = sample.broadphase_candidate_descriptor,
                    .candidate_state = sample.broadphase_candidate_state,
                    .candidate_descriptor_state = sample.broadphase_candidate_descriptor_state};
                const auto normal_y = slice_word(candidate.candidate_state, 5U);
                if (normal_y) {
                    candidate.normal_y_bits = *normal_y;
                    candidate.normal_y_valid = true;
                    candidate.vertical_normal = is_vertical_normal(*normal_y);
                }
                impl_->frame_candidates.push_back(std::move(candidate));
                auto& stored = impl_->frame_candidates.back();
                impl_->current_callback_candidate = stored.id;
                impl_->add_stage(sample, stored);
                impl_->maybe_pin(stored);
            }
        } else if (is_candidate_filter_label(label) ||
                   label == "broadphase_candidate_append_call") {
            if (impl_->current_callback_candidate != ~0ULL) {
                auto* candidate = impl_->find_candidate(impl_->current_callback_candidate);
                if (candidate != nullptr && candidate->physics_object == sample.physics_object &&
                    (sample.broadphase_candidate_descriptor == 0U ||
                     sample.broadphase_candidate_descriptor ==
                         candidate->broadphase_candidate_descriptor)) {
                    if (label == "broadphase_candidate_append_call") {
                        candidate->append_call_seen = true;
                        if (sample.candidate_append_old_count_valid) {
                            candidate->append_index = sample.candidate_append_old_count;
                            candidate->append_index_valid = true;
                        }
                        impl_->current_append_candidate = candidate->id;
                    }
                    impl_->add_stage(sample, *candidate);
                }
            }
        } else if (label == "candidate_append_entry") {
            if (impl_->current_callback_candidate != ~0ULL) {
                auto* candidate = impl_->find_candidate(impl_->current_callback_candidate);
                if (candidate != nullptr && candidate->physics_object == sample.physics_object) {
                    candidate->append_entry_seen = true;
                    if (sample.candidate_append_old_count_valid) {
                        candidate->append_index = sample.candidate_append_old_count;
                        candidate->append_index_valid = true;
                    }
                    impl_->current_append_candidate = candidate->id;
                    impl_->add_stage(sample, *candidate);
                }
            }
        } else if (label == "candidate_append_count_store") {
            if (impl_->current_append_candidate != ~0ULL) {
                auto* candidate = impl_->find_candidate(impl_->current_append_candidate);
                if (candidate != nullptr && candidate->physics_object == sample.physics_object) {
                    candidate->append_store_seen = true;
                    if (!candidate->append_index_valid && sample.candidate_append_old_count_valid) {
                        candidate->append_index = sample.candidate_append_old_count;
                        candidate->append_index_valid = true;
                    }
                    impl_->add_stage(sample, *candidate);
                }
            }
        } else if (label == "narrowphase_entry") {
            impl_->current_narrowphase_candidate = ~0ULL;
            const auto shape_base = slice_word(sample.primary_shape_header, 1U);
            if (shape_base && sample.shape_geometry.base_present &&
                *shape_base <= sample.shape_geometry.base) {
                const std::uint32_t byte_offset = sample.shape_geometry.base - *shape_base;
                if ((byte_offset & 63U) == 0U) {
                    const std::uint32_t append_index = byte_offset >> 6U;
                    const auto found = std::find_if(
                        impl_->frame_candidates.begin(), impl_->frame_candidates.end(),
                        [&sample, append_index](const CollisionCandidateEvidence& candidate) {
                            return candidate.physics_object == sample.physics_object &&
                                   candidate.append_store_seen && candidate.append_index_valid &&
                                   candidate.append_index == append_index;
                        });
                    if (found != impl_->frame_candidates.end()) {
                        found->narrowphase_seen = true;
                        found->narrowphase_shape = sample.shape_geometry.base;
                        found->shape_geometry = sample.shape_geometry;
                        impl_->current_narrowphase_candidate = found->id;
                        impl_->add_stage(sample, *found);
                    }
                }
            }
        } else if (is_narrowphase_label(label)) {
            if (impl_->current_narrowphase_candidate != ~0ULL) {
                auto* candidate = impl_->find_candidate(impl_->current_narrowphase_candidate);
                if (candidate != nullptr && candidate->physics_object == sample.physics_object &&
                    !(candidate->narrowphase_shape != 0U && sample.shape_geometry.base_present &&
                      candidate->narrowphase_shape != sample.shape_geometry.base)) {
                    candidate->shape_geometry = sample.shape_geometry;
                    candidate->clip_bounds = sample.clip_bounds;
                    candidate->clip_vertices = sample.clip_vertices;
                    if (!candidate->normal_y_valid) {
                        const auto normal_y = slice_word(sample.shape_geometry, 13U);
                        if (normal_y) {
                            candidate->normal_y_bits = *normal_y;
                            candidate->normal_y_valid = true;
                            candidate->vertical_normal = is_vertical_normal(*normal_y);
                        }
                    }
                    if (label == "overlap_clip_call") {
                        candidate->clip_call_seen = true;
                        candidate->aabb_penetration =
                            candidate->aabb_penetration ||
                            vertices_inside_bounds(sample.clip_vertices, sample.clip_bounds);
                    } else if (label == "overlap_clip_vertex_count" && sample.query_result_valid) {
                        candidate->clip_vertex_count_valid = true;
                        candidate->clip_vertex_count = sample.query_result;
                    } else if (label == "primary_contact_accepted") {
                        candidate->primary_contact_accepted = true;
                    } else if (label == "secondary_contact_accepted") {
                        candidate->secondary_contact_accepted = true;
                    }
                    impl_->add_stage(sample, *candidate);
                    impl_->maybe_pin(*candidate);
                }
            }
        }
    }

    {
        const auto found =
            std::find(kSpatialResultPcs.begin(), kSpatialResultPcs.end(), sample.pc_before);
        const std::optional<std::size_t> index =
            found == kSpatialResultPcs.end()
                ? std::nullopt
                : std::optional<std::size_t>{
                      static_cast<std::size_t>(found - kSpatialResultPcs.begin())};
        if (index) {
            const std::uint16_t result = sample.vu0_integers[1];
            auto& aggregate = impl_->spatial_results[*index];
            ++aggregate.total;
            if (result == 0U) {
                ++aggregate.rejected;
            } else {
                ++aggregate.accepted;
            }
            SpatialDecisionEvidence evidence;
            evidence.sample = sample;
            evidence.result = result;
            if (impl_->spatial_evidence.size() < 256U) {
                impl_->spatial_evidence.push_back(evidence);
            } else {
                impl_->spatial_evidence[impl_->spatial_evidence_next] = evidence;
                impl_->spatial_evidence_next = (impl_->spatial_evidence_next + 1U) % 256U;
            }
        }
    }

    if (impl_->first.size() < impl_->config.first_capacity) {
        impl_->first.push_back(sample);
        return SampleToken{impl_->first.back().sequence, impl_->first.size() - 1U, false};
    }
    if (impl_->tail.empty()) {
        return std::nullopt;
    }
    const std::size_t index = impl_->tail_next;
    impl_->tail[index] = sample;
    impl_->tail_next = (index + 1U) % impl_->tail.size();
    impl_->tail_count = std::min(impl_->tail_count + 1U, impl_->tail.size());
    return SampleToken{impl_->tail[index].sequence, index, true};
}


void Slus21050CollisionTrace::finish_sample(SampleToken token,
                                            const guest::R5900State& state,
                                            const std::uint32_t step_status) noexcept {
    if (!impl_ || impl_->finalized) {
        return;
    }
    CollisionCompareSample* sample = nullptr;
    if (token.tail) {
        if (token.index < impl_->tail.size()) {
            sample = &impl_->tail[token.index];
        }
    } else if (token.index < impl_->first.size()) {
        sample = &impl_->first[token.index];
    }
    if (sample == nullptr || sample->sequence != token.sequence) {
        return;
    }
    sample->step_status = step_status;
    sample->pc_after = state.pc();
    sample->next_pc_after = state.next_pc();
    sample->fcr31_after = state.fcr31_bits();
    sample->complete = true;
    if (sample->vehicle_snapshot) {
        if (step_status == 0U && !state.is_executing_delay_slot() &&
            sample->pc_after == 0x00143844U && sample->next_pc_after == 0x00143848U) {
            if (!impl_->latest_vehicle_snapshot ||
                sample->vehicle_snapshot->sequence > impl_->latest_vehicle_snapshot->sequence) {
                sample->vehicle_snapshot->complete = true;
                impl_->latest_vehicle_snapshot = *sample->vehicle_snapshot;
                ++impl_->snapshot_diagnostics.valid_completions;
            } else {
                ++impl_->snapshot_diagnostics.older_completions;
            }
        } else {
            ++impl_->snapshot_diagnostics.incomplete_completions;
        }
    }
    impl_->complete_stage(*sample);
}

void Slus21050CollisionTrace::finalize() {
    if (!impl_) {
        throw std::logic_error("collision trace has no implementation");
    }
    if (impl_->finalized) {
        return;
    }
    if (impl_->current_vblank) {
        impl_->flush_frame();
        impl_->current_vblank.reset();
    }
    std::error_code error;
    if (std::filesystem::exists(impl_->output_path, error) || error ||
        std::filesystem::exists(impl_->partial_path, error) || error) {
        throw std::runtime_error("collision trace output appeared while the run was active");
    }
    try {
        std::ofstream output(impl_->partial_path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("could not create collision trace partial file: " +
                                     impl_->partial_path.string());
        }
        const auto stats = statistics();
        output << "B3_COLLISION_TRACE\t8\n"
               << "meta\troutines\t0x0013472c;0x001423e0-0x00143840;0x00143cb8;0x00199eb0-0x0019a1c4;0x0019a38c;0x0019a660;0x0019a674;0x0019b3c8;0x0019a704;0x0019a770;0x001bd6f8-0x001bd718;0x001ab090-0x001ab108;0x001c4ac0-0x001c4ac8;0x001c5168-0x001c51d0;0x001c5330-0x001c5664;0x001c6210-0x001c65d0;0x00263cf0-0x00263d70;0x00273070-0x00273378;0x00274ed0-0x002751c0;0x00301b00-0x00301bb0;0x003bea68-0x003bee54\n"
               << "meta\ttotal\t" << stats.total_samples << "\n"
               << "meta\tretained\t" << stats.retained_samples << "\n"
               << "meta\toverwritten\t" << stats.overwritten_samples << "\n"
               << "meta\tframe_car_summaries\t" << impl_->summaries.size() << "\n"
               << "meta\tpinned_candidates\t" << (impl_->pinned_candidate ? 1U : 0U) << "\n"
               << "meta\tspatial_decisions_retained\t" << impl_->spatial_evidence.size() << "\n";
        for (std::size_t index = 0; index < kSpatialResultPcs.size(); ++index) {
            output << "spatial_result\t";
            const auto& aggregate = impl_->spatial_results[index];
            write_hex32(output, kSpatialResultPcs[index]);
            output << '\t' << aggregate.total << '\t' << aggregate.accepted << '\t'
                   << aggregate.rejected << '\n';
        }
        output << "columns\tseq\tsteps\tcycles\tvblank\tthread\tpc_before\tnext_pc_before\tdelay_slot\tword\texpected_word\tword_match\tlabel\tfs\tft\tcomparison\tfs_raw\tft_raw\tfs_normalized\tft_normalized\texpected_fcc0\tfcr31_before\tfcr31_after\tactual_fcc0\tfpu_acc\tstep_status\tpc_after\tnext_pc_after\tcomplete\tgprs128\tfprs\tvf0_to_vf8\tvi0_to_vi15\tvu0_controls\tvu0_acc\tstack_sp_e0\tring_a1_40\tcandidate_a2\toutput_t2\tcoeff_50\tvehicle_object\tphysics_component\tdispatched_method\tphysics_method_matches\tphysics_object\towner_vehicle\tcontact_accumulator\tquery_result_valid\tquery_result\towner_collision_mode_valid\towner_collision_mode\tprimary_shape_container\tprimary_shape_count_valid\tprimary_shape_count\tsecondary_shape_count_valid\tsecondary_shape_count\tbroadphase_provider\tbroadphase_slot_valid\tbroadphase_slot\tbroadphase_slot_page_valid\tbroadphase_slot_page\tbroadphase_page_base\tbroadphase_page_state_valid\tbroadphase_page_state\tbroadphase_expected_page_state\tbroadphase_page_state_memory_valid\tbroadphase_page_state_memory\tbroadphase_traversal_root\tbroadphase_query_pointer\tbroadphase_callback_pointer\tbroadphase_candidate\tbroadphase_candidate_descriptor\tspatial_leaf_pointer\tactive_list_store_valid\tactive_list_store_address\tactive_list_store_value\tbroadphase_candidate_type_flags_valid\tbroadphase_candidate_type_flags\tcandidate_append_overflow_latch_valid\tcandidate_append_overflow_latch\tcandidate_append_old_count_valid\tcandidate_append_old_count\tcandidate_append_new_count_valid\tcandidate_append_new_count\tprimary_contact_count_valid\tprimary_contact_count\tsecondary_contact_count_valid\tsecondary_contact_count\tworld_node\tworld_triangle\tvehicle_contact_state\tvehicle_transform\tvehicle_vectors\tphysics_state\tphysics_object_state\tphysics_owner_region\tphysics_shape_slots\tprimary_shape_header\tsecondary_shape_region\tbroadphase_provider_state\tbroadphase_query_state\tbroadphase_candidate_state\tbroadphase_candidate_descriptor_state\tbroadphase_owner_filter_vector\tspatial_leaf_state\tactive_list_state\tactive_list_caller_config\tactive_list_fixed_source\tcontact_accumulator_tail\tshape_geometry\tclip_bounds\tclip_vertices\tclip_output\n";
        output << "summary_columns\tvblank\trole\tvehicle_object\tphysics_object\tfirst_seq\tlast_seq\tdispatches\tbroadphase_queries\tbroadphase_query_hits\tcandidate_callbacks\tcandidate_filter_probes\tcandidate_filter_rejects\tcandidate_append_calls\tcandidate_append_stores\tcandidate_append_overflows\tnarrowphase_calls\toverlap_clip_calls\toverlap_clip_zero\toverlap_clip_nonzero\tprimary_contact_count_max\tsecondary_contact_count_max\tprimary_contacts_accepted\tsecondary_contacts_accepted\tgather_calls\tgather_nonzero\tresponse_probes\n"
        "pin_columns\tid\tvertical_normal\taabb_penetration\toutcome\tvblank\tvehicle_object\tphysics_object\tbroadphase_candidate\tbroadphase_candidate_descriptor\tnarrowphase_shape\tnormal_y_valid\tnormal_y_bits\tappend_call_seen\tappend_entry_seen\tappend_store_seen\tappend_index_valid\tappend_index\tnarrowphase_seen\tclip_call_seen\tclip_vertex_count_valid\tclip_vertex_count\tprimary_contact_accepted\tsecondary_contact_accepted\tfilter_rejected\tcandidate_state\tcandidate_descriptor_state\tshape_geometry\tclip_bounds\tclip_vertices\n"
        "pin_stage_columns\tid\tseq\tsteps\tcycles\tvblank\tthread\tlabel\tpc_before\tpc_after\tnext_pc_after\tcomplete\tstep_status\tquery_result_valid\tquery_result\tcandidate_append_old_count_valid\tcandidate_append_old_count\tcandidate_append_new_count_valid\tcandidate_append_new_count\tprimary_contact_count_valid\tprimary_contact_count\tsecondary_contact_count_valid\tsecondary_contact_count\n"
        "spatial_columns\tseq\tsteps\tcycles\tvblank\tthread\tlabel\tpc\tvi1_result\tvehicle_object\tphysics_object\tspatial_leaf_pointer\tcallback_pointer\tvertex_base_s0\tcompact_index_s2\tcomplete\tstep_status\tpc_after\tnext_pc_after\tvf0_to_vf8\tvi0_to_vi15\tvu0_controls\tvu0_acc\tbroadphase_query_state\tspatial_leaf_state\n";
        for (const auto& sample : impl_->first) {
            write_sample(output, sample);
        }
        if (impl_->tail_count != 0U) {
            const std::uint64_t start =
                impl_->tail_count == impl_->tail.size() ? impl_->tail_next : 0U;
            for (std::size_t offset = 0; offset < impl_->tail_count; ++offset) {
                write_sample(output, impl_->tail[(start + offset) % impl_->tail.size()]);
            }
        }
        for (const auto& summary : impl_->summaries) {
            write_summary(output, summary);
        }
        if (auto& pinned_candidate = impl_->pinned_candidate) {
            write_candidate(output, *pinned_candidate);
            for (const auto& stage : impl_->pinned_candidate->stages) {
                output << "pinned_stage\t" << impl_->pinned_candidate->id << '\t' << stage.sequence
                       << '\t' << stage.context.committed_steps << '\t' << stage.context.ee_cycles
                       << '\t' << stage.context.vblank_starts << '\t' << stage.context.thread_id
                       << '\t' << (stage.label != nullptr ? stage.label : "") << '\t';
                write_hex32(output, stage.pc_before);
                output << '\t';
                write_hex32(output, stage.pc_after);
                output << '\t';
                write_hex32(output, stage.next_pc_after);
                output << '\t' << (stage.complete ? 1 : 0) << '\t' << stage.step_status << '\t'
                       << (stage.query_result_valid ? 1 : 0) << '\t';
                write_hex32(output, stage.query_result);
                output << '\t' << (stage.candidate_append_old_count_valid ? 1 : 0) << '\t'
                       << stage.candidate_append_old_count << '\t'
                       << (stage.candidate_append_new_count_valid ? 1 : 0) << '\t'
                       << stage.candidate_append_new_count << '\t'
                       << (stage.primary_contact_count_valid ? 1 : 0) << '\t'
                       << stage.primary_contact_count << '\t'
                       << (stage.secondary_contact_count_valid ? 1 : 0) << '\t'
                       << stage.secondary_contact_count << '\n';
            }
        }
        if (!impl_->spatial_evidence.empty()) {
            const std::uint64_t start =
                impl_->spatial_evidence.size() == 256U ? impl_->spatial_evidence_next : 0U;
            for (std::size_t offset = 0; offset < impl_->spatial_evidence.size(); ++offset) {
                write_spatial_decision(
                    output,
                    impl_->spatial_evidence[(start + offset) % impl_->spatial_evidence.size()]);
            }
        }
        output.flush();
        if (!output) {
            throw std::runtime_error("could not flush collision trace partial file: " +
                                     impl_->partial_path.string());
        }
        output.close();
        if (!output) {
            throw std::runtime_error("could not close collision trace partial file: " +
                                     impl_->partial_path.string());
        }
        std::filesystem::rename(impl_->partial_path, impl_->output_path);
        impl_->finalized = true;
    } catch (...) {
        error.clear();
        std::filesystem::remove(impl_->partial_path, error);
        throw;
    }
}

Slus21050CollisionTraceStatistics Slus21050CollisionTrace::statistics() const noexcept {
    if (!impl_) {
        return {};
    }
    const auto retained = impl_->first.size() + impl_->tail_count;
    return {impl_->total, retained,
            impl_->total > retained ? impl_->total - retained : std::uint64_t{0U}};
}

std::optional<Slus21050VehicleWorldContactSnapshot>
Slus21050CollisionTrace::latest_vehicle_snapshot() const noexcept {
    if (!impl_ || !impl_->latest_vehicle_snapshot) {
        return std::nullopt;
    }
    return impl_->latest_vehicle_snapshot;
}

Slus21050CollisionSnapshotDiagnostics Slus21050CollisionTrace::snapshot_diagnostics()
    const noexcept {
    if (!impl_) {
        return {};
    }
    return impl_->snapshot_diagnostics;
}

const std::filesystem::path& Slus21050CollisionTrace::output_path() const noexcept {
    return impl_->output_path;
}

std::optional<std::uint32_t> Slus21050VehicleWorldContactEndpointProbe::expected_instruction(
    const guest::GuestAddress pc) noexcept {
    return pc == 0x00143840U ? std::optional<std::uint32_t>{0x7BB30030U} : std::nullopt;
}

bool Slus21050VehicleWorldContactEndpointProbe::is_target(const guest::GuestAddress pc) noexcept {
    return pc == 0x00143840U;
}

bool Slus21050VehicleWorldContactEndpointProbe::bind_session_epoch(
    const std::uint64_t session_epoch) noexcept {
    reset();
    bound_session_epoch_ = session_epoch;
    return session_epoch != 0U;
}

void Slus21050VehicleWorldContactEndpointProbe::reset() noexcept {
    pending_sample_.reset();
    latest_snapshot_.reset();
    diagnostics_ = {};
    bound_session_epoch_ = 0U;
}

std::optional<Slus21050VehicleWorldContactEndpointProbe::SampleToken>
Slus21050VehicleWorldContactEndpointProbe::begin_sample(
    const guest::R5900State& state, const guest::EeMemory& memory,
    const Slus21050CollisionTraceContext& context) {
    if (state.pc() != 0x00143840U) {
        return std::nullopt;
    }
    pending_sample_.reset();
    ++diagnostics_.return_observations;
    if (next_sequence_ == ~0ULL) {
        ++diagnostics_.sequence_overflows;
        return std::nullopt;
    }
    const std::uint64_t sequence = next_sequence_++;
    const unsigned instruction_word =
        memory.contains(state.pc(), 4U) ? memory.read_u32_le(state.pc()) : 0U;
    const auto capture = capture_world_contact_snapshot(
        state.pc(), state.next_pc(), instruction_word, state.is_executing_delay_slot(),
        state.read_gpr_u32(17U), memory, context, sequence);
    const bool captured = capture.snapshot.has_value();
    if (captured) {
        pending_sample_ = PendingSample{sequence, *capture.snapshot};
    } else {
        count_snapshot_failure(diagnostics_, capture.failure);
    }
    return SampleToken{sequence, captured};
}

std::optional<Slus21050VehicleWorldContactPublicationToken>
Slus21050VehicleWorldContactEndpointProbe::finish_sample(
    SampleToken token, const guest::R5900State& state, const std::uint32_t step_status,
    const Slus21050CollisionTraceContext& post_context) noexcept {
    if (!pending_sample_ || !token.snapshot_captured_ ||
        pending_sample_->sequence != token.sequence_) {
        if (token.snapshot_captured_) {
            ++diagnostics_.token_rejections;
        }
        return std::nullopt;
    }
    const Slus21050VehicleWorldContactSnapshot authenticated_snapshot = pending_sample_->snapshot;
    pending_sample_.reset();
    const auto completed_snapshot =
        complete_world_contact_snapshot(authenticated_snapshot, state, step_status, post_context);
    if (!completed_snapshot) {
        ++diagnostics_.incomplete_completions;
        return std::nullopt;
    }
    if (latest_snapshot_ && completed_snapshot->sequence <= latest_snapshot_->sequence) {
        ++diagnostics_.older_completions;
        return std::nullopt;
    }
    latest_snapshot_ = *completed_snapshot;
    ++diagnostics_.valid_completions;
    if (bound_session_epoch_ == 0U) {
        return std::nullopt;
    }
    return Slus21050VehicleWorldContactPublicationToken{bound_session_epoch_, *completed_snapshot};
}

std::optional<Slus21050VehicleWorldContactSnapshot>
Slus21050VehicleWorldContactEndpointProbe::latest_snapshot() const noexcept {
    return latest_snapshot_;
}

Slus21050CollisionSnapshotDiagnostics Slus21050VehicleWorldContactEndpointProbe::diagnostics()
    const noexcept {
    return diagnostics_;
}
}  // namespace takedown::runtime
