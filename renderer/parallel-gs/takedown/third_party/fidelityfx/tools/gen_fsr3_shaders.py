#!/usr/bin/env python3
"""Compile the FSR 3.1 upscaler's GLSL passes into the shader table the
FidelityFX Vulkan backend reads (generated/ffx_fsr3upscaler_takedown_shaders.h).

AMD's build does this with FidelityFX_SC.exe (Windows only). This does the same
with glslangValidator and a small SPIR-V reflection reader, and builds only the
permutations the Takedown renderer can ask for:
  HDR colour input off, low-resolution motion vectors, motion vectors without
  jitter; Lanczos LUT on and off (the SDK picks by the GPU's wave sizes);
  inverted depth on and off; sharpening on and off (the accumulate pass).
FP16 and wave64 permutations are not built: they only change speed, and the
accessor (ffx_fsr3upscaler_takedown_blobs.cpp) answers them with the 32-bit,
wave-agnostic shader.

usage: gen_fsr3_shaders.py   (rerun after updating the vendored SDK)
"""
import itertools
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
GPU = HERE / 'include/FidelityFX/gpu'
SHADERS = HERE / 'src/backends/vk/shaders/fsr3upscaler'
OUT = HERE / 'generated/ffx_fsr3upscaler_takedown_shaders.h'

# FfxFsr3UpscalerPass values (include/FidelityFX/host/ffx_fsr3upscaler.h).
PASSES = [
    (0, 'prepare_inputs'), (1, 'luma_pyramid'), (2, 'shading_change_pyramid'), (3, 'shading_change'),
    (4, 'prepare_reactivity'), (5, 'luma_instability'), (6, 'accumulate'), (7, 'accumulate'),
    (8, 'rcas'), (9, 'debug_view'), (10, 'autogen_reactive'),
]

BASE = ['-DFFX_GPU=1', '-DFFX_GLSL=1',
        '-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0',
        '-DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0',
        '-DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1',
        '-DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0',
        '-DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2',
        '-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=0',
        '-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1',
        '-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0']

# FSR3UPSCALER_SHADER_PERMUTATION_* bits (src/components/fsr3upscaler/ffx_fsr3upscaler_private.h).
LUT, LOW_RES_MV, INVERTED, SHARPEN = 1 << 0, 1 << 2, 1 << 4, 1 << 5


def reflect(spirv: bytes):
    words = struct.unpack('<%dI' % (len(spirv) // 4), spirv)
    assert words[0] == 0x07230203
    names, decorations, types, variables = {}, {}, {}, []
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xffff
        operands = words[i + 1:i + count]
        if op == 5:  # OpName
            raw = struct.pack('<%dI' % (count - 2), *operands[1:])
            names[operands[0]] = raw.split(b'\0', 1)[0].decode()
        elif op == 71:  # OpDecorate
            decorations.setdefault(operands[0], {})[operands[1]] = operands[2] if len(operands) > 2 else True
        elif op in (25, 26, 28, 29, 30, 32, 43):
            # OpTypeImage, OpTypeSampler, OpTypeArray, OpTypeRuntimeArray, OpTypeStruct, OpTypePointer, OpConstant
            types[operands[0] if op != 32 and op != 43 else (operands[0] if op == 32 else operands[1])] = (op, operands)
        elif op == 59:  # OpVariable
            variables.append((operands[0], operands[1], operands[2]))
        i += count

    def constant(value_id):
        for key, (op, operands) in types.items():
            if op == 43 and operands[1] == value_id:
                return operands[2]
        raise ValueError('array length %d' % value_id)

    groups = {'cbv': [], 'srv_tex': [], 'uav_tex': [], 'srv_buf': [], 'uav_buf': []}
    for result_type, var, storage in variables:
        if storage not in (0, 2, 12):
            continue
        _, pointer = types[result_type]
        pointee = pointer[2]
        count = 1
        op, operands = types[pointee]
        if op == 28:
            count = constant(operands[2])
            pointee = operands[1]
            op, operands = types[pointee]
        deco = decorations.get(var, {})
        binding, space = deco.get(33, 0), deco.get(34, 0)
        name = names.get(var, '')
        if op == 26:
            continue  # samplers come from the pipeline description
        if op == 25:
            dim, sampled = operands[2], operands[6]
            buffer = dim == 5
            kind = ('uav_buf' if buffer else 'uav_tex') if sampled == 2 else ('srv_buf' if buffer else 'srv_tex')
            groups[kind].append((name, binding, count, space))
        elif op == 30:
            if not name:
                name = names.get(pointee, '')
            if storage == 2 and 2 in decorations.get(pointee, {}):  # Block
                groups['cbv'].append((name, binding, count, space))
            else:
                groups['uav_buf'].append((name, binding, count, space))
    for value in groups.values():
        value.sort(key=lambda entry: entry[1])
    return groups


def compile_pass(source: Path, flags: int, work: Path) -> bytes:
    output = work / 'out.spv'
    defines = ['-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE=%d' % (1 if flags & LUT else 0),
               '-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=%d' % (1 if flags & INVERTED else 0),
               '-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING=%d' % (1 if flags & SHARPEN else 0)]
    subprocess.run(['glslangValidator', '-V', '--target-env', 'vulkan1.2', '-S', 'comp', '-Os', '--quiet',
                    *BASE, *defines, '-I' + str(GPU), '-I' + str(GPU / 'fsr3upscaler'), str(source), '-o', str(output)],
                   check=True)
    return output.read_bytes()


def c_strings(entries):
    return '{' + ', '.join('"%s"' % entry[0] for entry in entries) + '}' if entries else 'nullptr'


def c_numbers(entries, index):
    return '{' + ', '.join(str(entry[index]) for entry in entries) + '}' if entries else 'nullptr'


def main() -> int:
    lines = ['// Generated by tools/gen_fsr3_shaders.py from the FidelityFX SDK v1.1.4 FSR 3.1 upscaler GLSL',
             '// (MIT, see ../LICENSE.txt). Do not edit.', '#pragma once', '#include <cstdint>', '',
             'namespace takedown_fsr3 {', '']
    entries = []
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        built = {}
        for pass_id, name in PASSES:
            source = SHADERS / ('ffx_fsr3upscaler_%s_pass.glsl' % name)
            sharpen_values = [SHARPEN] if pass_id == 7 else [0]
            for lut, inverted, sharpen in itertools.product([0, LUT], [0, INVERTED], sharpen_values):
                flags = lut | inverted | sharpen | LOW_RES_MV
                key = (name, flags)
                if key not in built:
                    spirv = compile_pass(source, flags, work)
                    symbol = 'k_%s_%d' % (name, flags)
                    groups = reflect(spirv)
                    words = struct.unpack('<%dI' % (len(spirv) // 4), spirv)
                    lines.append('alignas(4) static const uint32_t %s_spirv[] = {' % symbol)
                    for start in range(0, len(words), 12):
                        lines.append('    ' + ', '.join('0x%08xu' % w for w in words[start:start + 12]) + ',')
                    lines.append('};')
                    for kind, items in groups.items():
                        if items:
                            lines.append('static const char *%s_%s_names[] = %s;' % (symbol, kind, c_strings(items)))
                            for field, index in (('bindings', 1), ('counts', 2), ('spaces', 3)):
                                lines.append('static const uint32_t %s_%s_%s[] = %s;' % (symbol, kind, field, c_numbers(items, index)))
                    built[key] = (symbol, groups, len(spirv))
                    print('built %s flags %#x: %d bytes, %s' % (name, flags, len(spirv),
                          {k: [e[0] for e in v] for k, v in groups.items() if v}))
                entries.append((pass_id, flags, built[key]))
    lines += ['', 'struct Shader', '{', '    uint32_t pass, flags;', '    const uint32_t *spirv;', '    uint32_t size;']
    kinds = ['cbv', 'srv_tex', 'uav_tex', 'srv_buf', 'uav_buf']
    for kind in kinds:
        lines.append('    uint32_t %s_count; const char **%s_names; const uint32_t *%s_bindings, *%s_counts, *%s_spaces;'
                     % (kind, kind, kind, kind, kind))
    lines += ['};', '', 'static const Shader kShaders[] = {']
    for pass_id, flags, (symbol, groups, size) in entries:
        fields = ['%d' % pass_id, '%#x' % flags, '%s_spirv' % symbol, '%d' % size]
        for kind in kinds:
            if groups[kind]:
                fields += ['%d' % len(groups[kind])] + ['%s_%s_%s' % (symbol, kind, f) for f in ('names', 'bindings', 'counts', 'spaces')]
            else:
                fields += ['0', 'nullptr', 'nullptr', 'nullptr', 'nullptr']
        lines.append('    { ' + ', '.join(fields) + ' },')
    lines += ['};', '', '} // namespace takedown_fsr3', '']
    OUT.write_text('\n'.join(lines))
    print('wrote', OUT, len(entries), 'entries')
    return 0


if __name__ == '__main__':
    sys.exit(main())
