; SPDX-License-Identifier: MIT
; Authored linkage witness: input declaration order differs from original QC7.
target datalayout = "e-p:64:64:64-f32:32:32-n8:16:32:64"
target triple = "air64-apple-ios17.7.0"
define <4 x float> @vpc_identity_witness(<4 x float> %position, <2 x float> %coord, <4 x float> %color) {
entry:
  %all = shufflevector <2 x float> %coord, <2 x float> poison, <4 x i32> zeroinitializer
  %result = fmul fast <4 x float> %color, %all
  ret <4 x float> %result
}
declare float @air.fast_saturate.f32(float)
!air.version = !{!0}
!air.compile_options = !{!1,!2}
!air.fragment = !{!3}
!0 = !{i32 2,i32 6,i32 0}
!1 = !{!"air.compile.denorms_disable"}
!2 = !{!"air.compile.fast_math_enable"}
!3 = !{ptr @vpc_identity_witness,!4,!6}
!4 = !{!5}
!5 = !{!"air.render_target",i32 0,i32 0,!"air.arg_type_name",!"float4"}
!6 = !{!7,!8,!9}
!7 = !{i32 0,!"air.position",!"air.center",!"air.no_perspective",!"air.arg_type_name",!"float4",!"air.arg_unused"}
!8 = !{i32 1,!"air.fragment_input",!"generated(11texcoord0_0Dv2_f)",!"air.center",!"air.perspective",!"air.arg_type_name",!"float2"}
!9 = !{i32 2,!"air.fragment_input",!"generated(5colorDv4_f)",!"air.center",!"air.perspective",!"air.arg_type_name",!"float4"}
