/* SGFix: jmp thunks for exports forwarded to the real d3d9.dll. The real DLL is loaded lazily on
   first use (EnsureReal), never inside DllMain. */
    .text
    .macro FWD name
    .globl _thunk_\name
_thunk_\name:
    cmpl $0, _g_real_loaded
    jne 1f
    push %ecx
    push %edx
    call _EnsureReal
    pop %edx
    pop %ecx
1:  jmp *_p_\name
    .endm
FWD Direct3D9EnableMaximizedWindowedModeShim
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
