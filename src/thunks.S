/* SGFix: jmp thunks for exports forwarded to the real d3d9.dll. The real DLL is loaded on first
   use (EnsureRealFrom); if it is not available the call ends in thunk_missing, which logs and
   exits rather than jumping through a null pointer. */
    .text
    .macro FWD name
    .globl _thunk_\name
_thunk_\name:
    cmpl $0, _g_real_loaded
    jne 1f
    push %ecx
    push %edx
    pushl 8(%esp)
    call _EnsureRealFrom
    add $4, %esp
    pop %edx
    pop %ecx
    cmpl $0, _g_real_loaded
    je _thunk_missing@0
1:  jmp *_p_\name
    .endm
FWD Direct3DCreate9Ex
FWD Direct3DCreate9On12
FWD Direct3DCreate9On12Ex
FWD Direct3DShaderValidatorCreate9
FWD PSGPError
FWD PSGPSampleTexture
FWD D3DPERF_BeginEvent
FWD D3DPERF_EndEvent
FWD D3DPERF_GetStatus
FWD D3DPERF_QueryRepeatFrame
FWD D3DPERF_SetMarker
FWD D3DPERF_SetOptions
FWD D3DPERF_SetRegion
FWD DebugSetLevel
FWD DebugSetMute
