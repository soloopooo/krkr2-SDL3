#!/bin/bash
cd "$(dirname "$0")"
glslc -c -fshader-stage=vertex quad.vert -o quad_vert.spv
glslc -c -fshader-stage=fragment quad.frag -o quad_frag.spv
glslc -c -fshader-stage=fragment gray.frag -o gray_frag.spv
glslc -c -fshader-stage=fragment blur.frag -o blur_frag.spv
glslc -c -fshader-stage=fragment adjust_gamma.frag -o adjust_gamma_frag.spv
glslc -c -fshader-stage=fragment univ_trans.frag -o univ_trans_frag.spv
glslc -c -fshader-stage=fragment gamma.frag -o gamma_frag.spv
glslc -c -fshader-stage=fragment present.frag -o present_frag.spv
glslc -c -fshader-stage=fragment crossfade.frag -o crossfade_frag.spv
python3 -c "
import struct, os
files = ['quad_vert','quad_frag','gray_frag','blur_frag','gamma_frag','adjust_gamma_frag','univ_trans_frag']
for f in files:
    with open(f+'.spv','rb') as fh: data=fh.read()
    print(f'{f}: {len(data)} bytes')
"
