// Integrate a rigid body for f12 seconds. a0 holds reference axes at +16..48,
// world inertia axes at +64..96, inverse pose at +112..160, velocity/speed at
// +176, direction +192, world/local spin +208/+224, force/torque +240/+256,
// impulses +272/+288/+304, mass +496, lever arm +500 and pose pointer +516.
// Flags +526 (reset), +528 (offset gravity), +529 (wake), type +533 select paths.
// Rotation axes are integrated, normalized and repaired by cross products.
// Counts below are block/path costs; six byte-copy loops each run exactly eight
// times (8 instructions/pass), and are replaced with native quadword copies.
#include "takedown/runtime/game/n3_support.hpp"
#include "takedown/guest/ee_cop2_macro.hpp"

namespace takedown::runtime {
namespace {
using n3::GuestAddress;
using Vec = guest::Gpr128;
namespace vu = guest::ee_cop2;
namespace fp = n3::fpu;
constexpr unsigned kVelocity = 176U, kDirection = 192U, kSpin = 208U, kLocalSpin = 224U;
constexpr unsigned kForce = 240U, kTorque = 256U, kImpulse = 272U, kAngularImpulse = 288U;
constexpr unsigned kTranslationImpulse = 304U, kMass = 496U, kPose = 516U;
constexpr GuestAddress kGravity = 0x004c9cc0U;
constexpr unsigned kEntryCost = 11U, kGravitySetupCost = 12U, kTypeTestCost = 4U;
constexpr unsigned kSimpleGravityCost = 4U, kOffsetGravityCost = 23U, kWakeCost = 2U;
constexpr unsigned kResetTestCost = 3U, kResetCost = 24U, kLinearCost = 40U;
constexpr unsigned kVerticalClampCost = 7U, kSpinCost = 33U, kSpinClampCost = 36U;
constexpr unsigned kRotationCost = 119U, kAxisTestCost = 6U, kDotTestCost = 46U;
constexpr unsigned kRepairCost = 45U, kLastRepairCost = 44U, kPairTestCost = 9U;
constexpr unsigned kAlternateRepairCost = 43U;
constexpr unsigned kComposeCost = 48U + 12U + 15U + 5U + 30U + 5U + 55U + 6U * 8U * 8U;
constexpr unsigned kStoppedCost = 11U, kMovingCost = 28U, kReturnCost = 4U;

// VU arithmetic on named vector values. W is deliberately inherited from the
// destination: these records use it for homogeneous coordinates and speed.
class VectorMath {
    guest::R5900State& cpu;
public:
    explicit VectorMath(guest::R5900State& state) : cpu(state) {}
    Vec add(Vec a, Vec b, Vec destination, bool subtract = false) {
        cpu.write_vu0_vector(1U, a); cpu.write_vu0_vector(2U, b); cpu.write_vu0_vector(3U, destination);
        vu::vadd_vector(cpu, 3U, 1U, 2U, 14U, subtract);
        return cpu.read_vu0_vector(3U);
    }
    Vec mul(Vec a, Vec b, Vec destination) {
        cpu.write_vu0_vector(1U, a); cpu.write_vu0_vector(2U, b); cpu.write_vu0_vector(3U, destination);
        vu::vmul(cpu, 3U, 1U, 2U, 14U); return cpu.read_vu0_vector(3U);
    }
    Vec scale(Vec a, unsigned factor) {
        cpu.write_vu0_vector(1U, a); cpu.write_vu0_vector(2U, Vec{{factor,0U,0U,0U}});
        vu::vmul_broadcast(cpu, 1U, 1U, 2U, 14U, 0U); return cpu.read_vu0_vector(1U);
    }
    Vec cross(Vec a, Vec b, Vec destination) {
        cpu.write_vu0_vector(1U, a); cpu.write_vu0_vector(2U, b); cpu.write_vu0_vector(3U, destination);
        vu::vopmula(cpu, 1U, 2U); vu::vopmsub(cpu, 3U, 2U, 1U); return cpu.read_vu0_vector(3U);
    }
    unsigned dot(Vec a, Vec b) {
        cpu.write_vu0_vector(1U, a); cpu.write_vu0_vector(2U, b);
        vu::vmul(cpu, 2U, 1U, 2U, 14U);
        vu::vadda_broadcast(cpu, 2U, 2U, 8U, 1U);
        cpu.write_vu0_vector(1U, n3::kVf0);
        cpu.write_vu0_vector(3U, Vec{{0x3f800000U,0U,0U,0U}});
        vu::vmadd_broadcast(cpu, 1U, 3U, 2U, 8U, 2U, false, false, false);
        return cpu.read_vu0_vector(1U).words[0];
    }
    Vec normalize(Vec value, unsigned* length = nullptr) {
        const auto squared = dot(value, value);
        cpu.write_vu0_vector(1U, Vec{{squared,0U,0U,0U}});
        vu::vdiv(cpu, 0U, 3U, 1U, 0U, true);
        if (length) *length = fp::sqrt(squared);
        cpu.write_vu0_vector(1U, value);
        vu::vmul_control(cpu, 1U, 1U, 14U, vu::kControlQ);
        return cpu.read_vu0_vector(1U);
    }
    Vec transform(Vec x, Vec y, Vec z, Vec value) {
        cpu.write_vu0_vector(1U, x); cpu.write_vu0_vector(2U, y);
        cpu.write_vu0_vector(3U, z); cpu.write_vu0_vector(4U, value);
        vu::vmula_broadcast(cpu, 1U, 4U, 14U, false);
        vu::vmadd_broadcast(cpu, 0U, 2U, 4U, 14U, 1U, false, true, false);
        // Original destination is the last matrix column, preserving its W.
        vu::vmadd_broadcast(cpu, 3U, 3U, 4U, 14U, 2U, false, false, false);
        return cpu.read_vu0_vector(3U);
    }
};

template<class Context>
n3::Run integrate_rigid_body(Context& context) {
    auto call = n3::guest_call(context);
    auto& m = call.memory(); auto& cpu = call.cpu();
    const GuestAddress body = call.arg(0U);
    const auto timestep = cpu.read_fpr_bits(12U);
    const auto load = [&](unsigned offset) { return n3::load_quad_any(m, body + offset); };
    const auto store = [&](unsigned offset, Vec value) { n3::store_quad_any(m, body + offset, value); };
    // The initial zero-force stores retain these incoming VU W lanes.
    const auto incoming_zero_w = cpu.read_vu0_vector(3U).words[3];
    const auto incoming_force_w = cpu.read_vu0_vector(4U).words[3];
    VectorMath math(cpu);
    auto force_clear_w = incoming_zero_w;
    unsigned retired = kEntryCost;
    if (m.read_u8(body + 529U)) { m.write_u8(body + 526U, 0U); retired += kWakeCost; }
    retired += kGravitySetupCost;
    Vec gravity = n3::load_quad_any(m, kGravity);
    gravity = math.mul(gravity, Vec{{0x3f800000U,m.read_u32_le(body+kMass),0x3f800000U,0U}}, gravity);
    bool offset_gravity = m.read_u8(body + 528U) != 0U;
    if (!offset_gravity) { retired += kTypeTestCost; offset_gravity = m.read_u8(body + 533U) == 6U; }
    store(kForce, math.add(load(kForce), gravity, load(kForce)));
    if (offset_gravity) {
        const auto pose = m.read_u32_le(body + kPose);
        force_clear_w = n3::load_quad_any(m,pose+16U).words[3];
        Vec lever = math.scale(n3::load_quad_any(m, pose + 16U), m.read_u32_le(body + 500U));
        lever = math.add(lever, n3::load_quad_any(m, pose+48U), lever);
        lever = math.add(lever, n3::load_quad_any(m, m.read_u32_le(body+kPose)+48U), n3::load_quad_any(m,m.read_u32_le(body+kPose)+48U), true);
        const Vec torque = math.cross(lever, gravity, lever);
        store(kTorque, math.add(load(kTorque), torque, load(kTorque)));
        retired += kOffsetGravityCost;
    } else retired += kSimpleGravityCost;
    retired += kResetTestCost;
    if (m.read_u8(body + 526U)) {
        const auto current_pose = m.read_u32_le(body+kPose);
        const auto zero_w = offset_gravity ? n3::load_quad_any(m,current_pose+48U).words[3]
                                          : m.read_u32_le(call.stack_pointer()-3232U+1564U);
        const auto force_w = offset_gravity ? m.read_u32_le(call.stack_pointer()-3232U+2460U)
                                           : incoming_force_w;
        const auto impulse_w = offset_gravity ? load(kTorque).words[3] : load(kForce).words[3];
        const Vec zero{{0U,0U,0U,zero_w}};
        store(kSpin, zero); store(kLocalSpin, zero); store(kVelocity, Vec{});
        store(kDirection, n3::load_quad_any(m,current_pose+32U));
        store(kForce, Vec{{0U,0U,0U,force_w}});
        store(kTorque, Vec{{0U,0U,0U,force_clear_w}});
        store(kImpulse, Vec{{0U,0U,0U,impulse_w}});
        store(kAngularImpulse, zero); store(kTranslationImpulse, zero);
        m.write_u32_le(body+404U,0U); m.write_u32_le(body+408U,0U);
        m.write_u8(body+530U,0U); m.write_u8(body+531U,0U); retired += kResetCost;
    }
    retired += kLinearCost;
    store(kImpulse, math.add(load(kImpulse), math.scale(load(kForce),timestep), load(kImpulse)));
    store(kForce, Vec{{0U,0U,0U,force_clear_w}});
    store(kAngularImpulse, math.add(load(kAngularImpulse),math.scale(load(kTorque),timestep),load(kAngularImpulse)));
    store(kTorque, Vec{{0U,0U,0U,load(kTorque).words[3]}});
    const auto inverse_mass = fp::div(0x3f800000U,m.read_u32_le(body+kMass));
    Vec velocity = math.add(load(kVelocity),math.scale(load(kImpulse),inverse_mass),load(kVelocity));
    velocity = math.add(n3::kVf0,velocity,load(kVelocity));
    store(kVelocity,velocity); store(kImpulse,Vec{{0U,0U,0U,load(kImpulse).words[3]}});
    if (!fp::le(velocity.words[1],0x42f00000U)) {
        velocity.words[1] = 0x42f00000U; store(kVelocity,math.add(n3::kVf0,velocity,velocity)); retired += kVerticalClampCost;
    }
    retired += kSpinCost;
    store(kLocalSpin, math.add(load(kLocalSpin),load(kAngularImpulse),load(kLocalSpin)));
    store(kAngularImpulse,Vec{{0U,0U,0U,load(kVelocity).words[3]}});
    Vec spin = math.transform(load(64U),load(80U),load(96U),load(kLocalSpin));
    store(kSpin,spin);
    const auto spin_squared = math.dot(spin,spin);
    if (!fp::le(spin_squared,fp::from_int(10000))) {
        // Normalize the spin, then apply the game's additional
        // 100/sqrt(original length squared) factor.
        spin = math.normalize(spin); store(kSpin,spin);
        cpu.write_fpr_bits(1U,0x3f800000U);
        cpu.write_fpr_bits(2U,spin_squared);
        guest::ee_cop1::rsqrt_s(cpu,1U,1U,2U);
        const auto factor = fp::mul(0x42c80000U,cpu.read_fpr_bits(1U));
        spin = math.scale(spin,factor); store(kSpin,spin);
        const auto damping = m.read_u32_le(cpu.read_gpr_u32(n3::kGlobalPointer)-32716U);
        store(kLocalSpin,math.scale(load(kLocalSpin),damping)); retired += kSpinClampCost;
    }
    retired += kRotationCost;
    auto pose = m.read_u32_le(body+kPose);
    auto position = n3::load_quad_any(m,pose+48U);
    n3::store_quad_any(m,pose+48U,math.add(position,math.scale(load(kVelocity),timestep),position));
    pose = m.read_u32_le(body+kPose);
    const auto rotation_step = math.scale(load(kSpin),timestep);
    for (unsigned offset : {32U,16U,0U}) {
        Vec axis = n3::load_quad_any(m,pose+offset);
        Vec change = math.cross(axis,rotation_step,axis);
        axis = math.add(axis,change,axis,true); n3::store_quad_any(m,pose+offset,axis);
    }
    std::array<unsigned,3> lengths{};
    for (unsigned i=0U;i<3U;++i) {
        auto axis = math.normalize(n3::load_quad_any(m,pose+16U*i), &lengths[i]);
        n3::store_quad_any(m,pose+16U*i,axis);
    }
    const auto axis = [&](unsigned i) { return n3::load_quad_any(m,pose+16U*i); };
    unsigned repaired_w = 0U;
    const auto repair = [&](unsigned destination,unsigned left,unsigned right,bool retain_scratch_w = false) {
        Vec destination_value = axis(right);
        // One repair reuses the dot-product scratch vector as its destination.
        // Its unwritten W lane survives into the pose, so read that lane only.
        if (retain_scratch_w) destination_value.words[3] = m.read_u32_le(call.stack_pointer()-3232U+2108U);
        const Vec cross = math.cross(axis(left),axis(right),destination_value);
        repaired_w = cross.words[3];
        n3::store_quad_any(m,pose+16U*destination,cross);
        n3::store_quad_any(m,pose+16U*destination,math.normalize(cross));
    };
    if (fp::le(lengths[0],0U)) { repair(0U,1U,2U); repair(2U,0U,1U); retired += kRepairCost; }
    else {
        retired += kAxisTestCost;
        if (fp::le(lengths[1],0U)) { repair(1U,2U,0U); repair(0U,1U,2U); retired += kRepairCost; }
        else {
            retired += kAxisTestCost;
            if (fp::le(lengths[2],0U)) { repair(2U,0U,1U); repair(1U,2U,0U); retired += kLastRepairCost; }
            else {
                retired += kDotTestCost;
                const auto yz = fp::abs(math.dot(axis(1U),axis(2U)));
                const auto zx = fp::abs(math.dot(axis(2U),axis(0U)));
                const auto xy = fp::abs(math.dot(axis(0U),axis(1U)));
                if (fp::lt(yz,zx)) {
                    retired += kAxisTestCost + kRepairCost;
                    if (fp::lt(yz,xy)) { repair(0U,1U,2U); repair(2U,0U,1U); }
                    else { repair(2U,0U,1U); repair(1U,2U,0U); }
                } else {
                    retired += kPairTestCost;
                    if (fp::lt(zx,xy)) { repair(1U,2U,0U); repair(0U,1U,2U); retired += kRepairCost; }
                    else { repair(2U,0U,1U,true); repair(1U,2U,0U); retired += kAlternateRepairCost; }
                }
            }
        }
    }
    retired += kComposeCost;
    position = n3::load_quad_any(m,pose+48U);
    n3::store_quad_any(m,pose+48U,math.add(position,load(kTranslationImpulse),position));
    store(kTranslationImpulse,Vec{{0U,0U,0U,repaired_w}});
    const std::array<Vec,3> rotation{{axis(0U),axis(1U),axis(2U)}};
    const std::array<Vec,3> reference{{load(16U),load(32U),load(48U)}};
    std::array<Vec,3> inertia{};
    for(unsigned i=0U;i<3U;++i) {
        Vec row{{rotation[0].words[i],rotation[1].words[i],rotation[2].words[i],0U}};
        inertia[i] = math.transform(reference[0],reference[1],reference[2],row);
    }
    for(unsigned i=0U;i<3U;++i) store(64U+16U*i,inertia[i]);
    std::array<Vec,3> world_inertia{};
    for(unsigned i=0U;i<3U;++i) {
        world_inertia[i]=math.transform(rotation[0],rotation[1],rotation[2],load(64U+16U*i));
        world_inertia[i].words[3]=inertia[i].words[3];
    }
    for(unsigned i=0U;i<3U;++i) store(64U+16U*i,world_inertia[i]);
    pose = m.read_u32_le(body+kPose);
    for(unsigned i=0U;i<4U;++i) store(112U+16U*i,n3::load_quad_any(m,pose+16U*i));
    const auto inverse_x = load(112U), inverse_y=load(128U), inverse_z=load(144U);
    for(unsigned i=0U;i<3U;++i) {
        Vec row = load(112U+16U*i);
        row.words[0]=fp::add(0U,inverse_x.words[i]);
        row.words[1]=fp::add(0U,inverse_y.words[i]);
        row.words[2]=fp::add(0U,inverse_z.words[i]);
        store(112U+16U*i,row);
    }
    position = load(160U);
    auto translated_z=math.scale(load(144U),position.words[2]);
    auto translated_x=math.scale(load(112U),position.words[0]);
    auto translated_y=math.scale(load(128U),position.words[1]);
    auto translated=math.add(translated_x,translated_y,load(128U));
    translated=math.add(translated,translated_z,translated);
    translated=math.add(n3::kVf0,translated,translated,true); store(160U,translated);
    velocity=load(kVelocity);
    store(kVelocity,math.add(n3::kVf0,velocity,velocity));
    const auto speed_squared=math.dot(velocity,velocity);
    if (fp::lt(speed_squared,0x2f800000U)) {
        velocity.words[3]=0U; store(kVelocity,velocity);
        store(kDirection,n3::load_quad_any(m,m.read_u32_le(body+kPose)+32U));
        retired += kStoppedCost;
    } else {
        store(kDirection,velocity);
        store(kDirection,math.normalize(velocity));
        velocity=load(kVelocity); velocity.words[3]=fp::add(0U,fp::sqrt(speed_squared));
        store(kVelocity,velocity); retired += kMovingCost;
    }
    return call.finish(retired+kReturnCost);
}
} // namespace
} // namespace takedown::runtime
N3_ENTRY_PLAIN(001ac470,3856U,0xe5b16f86fbe3d4d3ULL,"IntegrateRigidBody",1012U,integrate_rigid_body,EngineReturn::Nothing)
