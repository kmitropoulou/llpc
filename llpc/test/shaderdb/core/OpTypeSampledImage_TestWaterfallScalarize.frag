#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 7) uniform sampler2D _11[];

layout(location = 0) out vec4 _3;
layout(location = 3) flat in int _4;
layout(location = 1) flat in vec2 _6;

void main()
{
    int _12 = _4;
    _3 = texture(_11[nonuniformEXT(_12)], _6);
}

// RUN: amdllpc -scalarize-waterfall-descriptor-loads -v %gfxip %s | FileCheck -check-prefix=GFX %s
// Explicitly check GFX10.3 ASIC variants:
// RUN: amdllpc -scalarize-waterfall-descriptor-loads -v --gfxip=10.3.0 %s | FileCheck -check-prefix=GFX %s
// RUN: amdllpc -scalarize-waterfall-descriptor-loads -v --gfxip=10.3.2 %s | FileCheck -check-prefix=GFX_10_3_2 %s

// GFX-LABEL: {{^// LLPC}} pipeline patching results
// GFX: %[[mul:[0-9]+]] = mul i32 %{{.*}}, 48
// GFX-NEXT: %[[begin:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.begin.i32(i32 0, i32 %[[mul]])
// GFX-NEXT: %[[readfirstlane:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32(i32 %[[begin]], i32 %[[mul]])
// GFX-NEXT: %[[sext:[0-9]+]] = sext i32 %[[readfirstlane]] to i64
// GFX-NEXT: %[[gep1:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext]]
// GFX-NEXT: %[[load1:[0-9]+]] = load <8 x i32>, ptr addrspace(4) %[[gep1]], align 32
// GFX-NEXT: %[[gep2:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext]]
// GFX-NEXT: %[[load2:[0-9]+]] = load <4 x i32>, ptr addrspace(4) %[[gep2]], align 16
// GFX-NEXT: %[[image_call:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32(i32 15, float %{{.*}}, float %{{.*}}, <8 x i32> %[[load1]], <4 x i32> %[[load2]], i1 false, i32 0, i32 0)
// GFX-NEXT: %[[end:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.waterfall.end.v4f32(i32 %[[begin]], <4 x float> %[[image_call]])
// GFX: AMDLLPC SUCCESS

// GFX_10_3_2-LABEL: {{^// LLPC}} pipeline patching results
// GFX_10_3_2: %[[mul:[0-9]+]] = mul i32 %{{.*}}, 48
// GFX_10_3_2-NEXT: %[[begin:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.begin.i32(i32 0, i32 %[[mul]])
// GFX_10_3_2-NEXT: %[[readfirstlane:[0-9]+]] = call i32 @llvm.amdgcn.waterfall.readfirstlane.i32.i32(i32 %[[begin]], i32 %[[mul]])
// GFX_10_3_2-NEXT: %[[sext:[0-9]+]] = sext i32 %[[readfirstlane]] to i64
// GFX_10_3_2-NEXT: %[[gep1:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext]]
// GFX_10_3_2-NEXT: %[[load1:[0-9]+]] = load <8 x i32>, ptr addrspace(4) %[[gep1]], align 32
// GFX_10_3_2-NEXT: %[[extract:[.a-z0-9]+]] = extractelement <8 x i32> %[[load1]], i64 6
// GFX_10_3_2-NEXT: %[[and:[0-9]+]] = and i32 %[[extract]], -1048577
// GFX_10_3_2-NEXT: %[[insert:[.a-z0-9]+]] = insertelement <8 x i32> %[[load1]], i32 %[[and]], i64 6
// GFX_10_3_2-NEXT: %[[shufflevector:[0-9]+]] = shufflevector <8 x i32> %[[insert]], <8 x i32> %[[load1]], <8 x i32> <i32 0, i32 1, i32 2, i32 3, i32 4, i32 5, i32 6, i32 15>
// GFX_10_3_2-NEXT: %[[gep2:[0-9]+]] = getelementptr i8, ptr addrspace(4) %{{.*}}, i64 %[[sext]]
// GFX_10_3_2-NEXT: %[[load2:[0-9]+]] = load <4 x i32>, ptr addrspace(4) %[[gep2]], align 16
// GFX_10_3_2-NEXT: %[[image_call:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.image.sample.2d.v4f32.f32(i32 15, float %{{.*}}, float %{{.*}}, <8 x i32> %[[shufflevector]], <4 x i32> %[[load2]], i1 false, i32 0, i32 0)
// GFX_10_3_2-NEXT: %[[end:[0-9]+]] = call reassoc nnan nsz arcp contract afn <4 x float> @llvm.amdgcn.waterfall.end.v4f32(i32 %[[begin]], <4 x float> %[[image_call]])
// GFX_10_3_2: AMDLLPC SUCCESS
