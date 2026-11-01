// Test an oriented box against a world-space box with the separating-axis test.
// a0/a1 point to upper (+0) and lower (+16) corners; a2 is a 4-vector
// local-to-world frame (axes +0/+16/+32, translation +48). a3 is depth_out,
// t0 is point_out and t1 normal_out. Test face axes in X,Z,Y order, then
// their nine cross products. Reject on the first disjoint projection interval.
// All intermediate vectors and overlap depths are native locals, not guest
// stack writes. Contact generation for overlapping boxes with point_out uses
// missing natives 002f2a00/002f2cd0, so that cold path declines.
// First projection/first comparison: 202 instructions; later face projections
// 115, edge projections 117. Second comparison costs 3, overlap update 6,
// rejection mark 1, outcome branch 2. Rejection jump 2, epilogue 13.
#include "takedown/runtime/game/n3_support.hpp"
#include "takedown/guest/ee_cop2_macro.hpp"

namespace takedown::runtime {
namespace {
using Vec = guest::Gpr128;
using n3::GuestAddress;
namespace vu = guest::ee_cop2;
namespace fp = n3::fpu;
constexpr GuestAddress kWorldAxes = 0x004c3ff0U;
constexpr unsigned kLowerCorner = 16U, kFrameTranslation = 48U;
constexpr unsigned kFirstProjectionCost = 202U, kFaceProjectionCost = 115U, kEdgeProjectionCost = 117U;
constexpr unsigned kSecondCompareCost = 3U, kOverlapCost = 6U, kRejectedCost = 1U;
constexpr unsigned kOutcomeCost = 2U, kExitJumpCost = 2U, kEpilogueCost = 13U;
constexpr unsigned kContactTestCost = 2U, kCeiling = 20000U;

// Small VU kernels preserve the PS2 vector unit's rounding and accumulation.
class BoxMath {
    guest::R5900State& cpu;
public:
    explicit BoxMath(guest::R5900State& state) : cpu(state) {}
    Vec add(Vec left,Vec right,bool subtract = false) {
        cpu.write_vu0_vector(1U,left); cpu.write_vu0_vector(2U,right);
        vu::vadd_vector(cpu,1U,1U,2U,14U,subtract); return cpu.read_vu0_vector(1U);
    }
    Vec scale(Vec value,unsigned factor) {
        cpu.write_vu0_vector(1U,value); cpu.write_vu0_vector(2U,Vec{{factor,0U,0U,0U}});
        vu::vmul_broadcast(cpu,1U,1U,2U,14U,0U); return cpu.read_vu0_vector(1U);
    }
    Vec cross(Vec left,Vec right) {
        cpu.write_vu0_vector(1U,left); cpu.write_vu0_vector(2U,right);
        vu::vopmula(cpu,1U,2U); vu::vopmsub(cpu,1U,2U,1U); return cpu.read_vu0_vector(1U);
    }
    unsigned dot(Vec left,Vec right) {
        cpu.write_vu0_vector(1U,left); cpu.write_vu0_vector(2U,right);
        vu::vmul(cpu,2U,1U,2U,14U);
        vu::vadda_broadcast(cpu,2U,2U,8U,1U);
        cpu.write_vu0_vector(1U,Vec{{0x3f800000U,0U,0U,0U}});
        vu::vmadd_broadcast(cpu,1U,1U,2U,8U,2U,false,false,false);
        return cpu.read_vu0_vector(1U).words[0];
    }
    unsigned radius(Vec axis,const std::array<Vec,3>& basis,Vec half_size) {
        Vec projections{{dot(axis,basis[0]),dot(axis,basis[1]),dot(axis,basis[2]),0U}};
        cpu.write_vu0_vector(1U,projections); cpu.write_vu0_vector(2U,half_size);
        vu::vabs(cpu,1U,1U,14U); vu::vmul(cpu,1U,1U,2U,14U);
        vu::vadd_broadcast(cpu,2U,1U,1U,8U,1U,false);
        vu::vadd_broadcast(cpu,2U,2U,1U,8U,2U,false);
        return cpu.read_vu0_vector(2U).words[0];
    }
};

template<class Context>
n3::Run test_box_pair(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory();
    const GuestAddress first = call.arg(0U), second = call.arg(1U), frame = call.arg(2U);
    const auto load = [&](GuestAddress address) { return n3::load_quad_any(m,address); };
    const Vec first_upper=load(first),first_lower=load(first+kLowerCorner);
    const Vec second_upper=load(second),second_lower=load(second+kLowerCorner);
    const std::array<Vec,3> local_axes{{load(frame),load(frame+16U),load(frame+32U)}};
    const std::array<Vec,3> world_axes{{load(kWorldAxes),load(kWorldAxes+16U),load(kWorldAxes+32U)}};
    BoxMath math(call.cpu());
    constexpr unsigned kHalf = 0x3f000000U;
    const Vec local_center=math.scale(math.add(first_lower,first_upper),kHalf);
    // Match the game's separate multiplies and ordered translation additions.
    const Vec center_z=math.scale(local_axes[2],local_center.words[2]);
    const Vec center_x=math.scale(local_axes[0],local_center.words[0]);
    Vec first_center=math.add(load(frame+kFrameTranslation),center_x);
    const Vec center_y=math.scale(local_axes[1],local_center.words[1]);
    first_center=math.add(first_center,center_y);
    first_center=math.add(first_center,center_z);
    const Vec first_half_size=math.scale(math.add(first_upper,first_lower,true),kHalf);
    const Vec second_half_size=math.scale(math.add(second_upper,second_lower,true),kHalf);
    const Vec second_center=math.scale(math.add(second_lower,second_upper),kHalf);
    constexpr std::array<unsigned,3> kAxisOrder{{0U,2U,1U}};
    unsigned retired=0U;
    for(unsigned index=0U;index<15U;++index) {
        Vec axis;
        if(index<3U) axis=local_axes[kAxisOrder[index]];
        else if(index<6U) axis=world_axes[kAxisOrder[index-3U]];
        else {
            const unsigned edge=index-6U;
            axis=math.cross(local_axes[kAxisOrder[edge/3U]],world_axes[kAxisOrder[edge%3U]]);
        }
        const auto first_radius=math.radius(axis,local_axes,first_half_size);
        const auto first_projection=math.dot(first_center,axis);
        const auto first_low=fp::sub(first_projection,first_radius);
        const auto first_high=fp::add(first_projection,first_radius);
        const auto second_radius=math.radius(axis,world_axes,second_half_size);
        const auto second_projection=math.dot(second_center,axis);
        const auto second_low=fp::sub(second_projection,second_radius);
        const auto second_high=fp::add(second_projection,second_radius);
        retired += index==0U ? kFirstProjectionCost : index<6U ? kFaceProjectionCost : kEdgeProjectionCost;
        bool overlaps=fp::le(first_low,second_high);
        if(overlaps) {
            retired += kSecondCompareCost;
            overlaps=fp::le(second_low,first_high);
        }
        if(!overlaps) {
            call.return_value(0U);
            return call.finish(retired+kRejectedCost+kOutcomeCost+kExitJumpCost+kEpilogueCost);
        }
        // The overlap depth is used only by contact generation; its six guest
        // instructions still count on the boolean overlap path.
        retired += kOverlapCost+kOutcomeCost;
    }
    retired += kContactTestCost;
    if(call.arg(4U)!=0U) return call.decline();
    call.return_value(1U);
    return call.finish(retired+kExitJumpCost+kEpilogueCost);
}
} // namespace
} // namespace takedown::runtime
N3_ENTRY(002f0640,9144U,0x514d1e22f7a328f2ULL,"TestBoxPair",kCeiling,test_box_pair,EngineReturn::Integer)
