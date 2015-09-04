#define NOD3D
#define NONET
#include <xtl.h>

//////////////////////////////////////////////////////////////////////////////
// Standard emit-any-opcode intrinsic, needed for __lwsync for
// AcquireLockBarrier and ReleaseLockBarrier.
void __emit(unsigned int opcode);

static inline MemoryBarrier() {
	// __lwsync
	__emit(0x7C2004AC);
}