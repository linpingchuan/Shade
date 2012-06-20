//===-- JITEmitter.cpp - Write machine code to executable memory ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines a MachineCodeEmitter object that is used by the JIT to
// write machine code to memory and remember where relocatable values are.
//
//===----------------------------------------------------------------------===//

#include "emitter.hpp"
#include "engine.hpp"
#include "disassembler.hpp"

#include "llvm/ADT/OwningPtr.h"
#include "llvm/Constants.h"
#include "llvm/Module.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Analysis/DebugInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineCodeInfo.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/ExecutionEngine/JITMemoryManager.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetJITInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MutexGuard.h"
#include "llvm/Support/ValueHandle.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Disassembler.h"
#include "llvm/Support/Memory.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/ValueMap.h"
#include <algorithm>
#ifndef NDEBUG
#include <iomanip>
#endif

using namespace llvm;

// A declaration may stop being a declaration once it's fully read from bitcode.
// This function returns true if F is fully read and is still a declaration.
static bool isNonGhostDeclaration(const Function *F) {
  return F->isDeclaration() && !F->isMaterializable();
}

//===----------------------------------------------------------------------===//
// JIT lazy compilation code.
//

namespace Shade
{
Emitter::Emitter(Engine &engine, llvm::TargetMachine &TM)
	: SizeEstimate(0), engine(engine), TM(TM), TD(*TM.getTargetData()),
    EmittedFunctions(this) {
}
void Emitter::addRelocation(const MachineRelocation &MR) {
    CurrentCode->Relocations.push_back(MR);
}

void Emitter::StartMachineBasicBlock(MachineBasicBlock *MBB) {
	MBB->getBasicBlock();
    if (MBBLocations.size() <= (unsigned)MBB->getNumber())
    MBBLocations.resize((MBB->getNumber()+1)*2);
    MBBLocations[MBB->getNumber()] = getCurrentPCValue();

    DEBUG(dbgs() << "JIT: Emitting BB" << MBB->getNumber() << " at ["
                << (void*) getCurrentPCValue() << "]\n");
}

uintptr_t Emitter::getMachineBasicBlockAddress(MachineBasicBlock *MBB) const{
    assert(MBBLocations.size() > (unsigned)MBB->getNumber() &&
            MBBLocations[MBB->getNumber()] && "MBB not emitted!");
    return MBBLocations[MBB->getNumber()];
}

void Emitter::emitLabel(MCSymbol *Label) {
    LabelLocations[Label] = getCurrentPCValue();
}

DenseMap<MCSymbol*, uintptr_t> *Emitter::getLabelLocations() {
    return &LabelLocations;
}

uintptr_t Emitter::getLabelAddress(MCSymbol *Label) const {
    assert(LabelLocations.count(Label) && "Label not emitted!");
    return LabelLocations.find(Label)->second;
}


void *Emitter::getGlobalVariableAddress(const GlobalVariable *V)
{
	auto result = GlobalOffsets.find(V);

	if(result != GlobalOffsets.end())
		return (void *)(result->second | 0xDA000000);
	
	// If the global is external, just remember the address.
	if (V->isDeclaration() || V->hasAvailableExternallyLinkage()) {
		report_fatal_error("Could not resolve external global address: "
						+ V->getName());
		return 0;
	}

	Type *GlobalType = V->getType()->getElementType();
	
	size_t S = TD.getTypeAllocSize(GlobalType);
	size_t A = TD.getPreferredAlignment(V);

	size_t offset = RoundUpToAlignment(Globals.size(), A);
	
	Globals.set_size(offset + S);

	memset(&Globals[offset], 0, S);
	
	if (!V->isThreadLocal())
		engine.InitializeMemory(V->getInitializer(), &Globals[offset]);

	GlobalOffsets[V] = offset;

	return (void *)(offset | 0xDA000000);
}

void *Emitter::getGlobalAddress(const GlobalValue *V)
{
	auto result = GlobalOffsets.find(V);

	if(result != GlobalOffsets.end())
		return (void *)(result->second | 0xDA000000);

	llvm_unreachable("Global hasn't had an address allocated yet!");
}

//===----------------------------------------------------------------------===//
// JITEmitter code.
//
void *Emitter::getPointerToGlobal(GlobalValue *V, void *Reference,
                                     bool MayNeedFarStub) {
  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(V))
    return getGlobalVariableAddress(GV);

  if (GlobalAlias *GA = dyn_cast<GlobalAlias>(V))
    return getGlobalAddress(GA->resolveAliasedGlobal(false));

  // If we have already compiled the function, return a pointer to its body.
  Function *F = cast<Function>(V);

  // If we know the target can handle arbitrary-distance calls, try to
  // return a direct pointer.
  if (!MayNeedFarStub) {
    // If we have code, go ahead and return that.
    //void *ResultPtr = TheJIT->getPointerToGlobalIfAvailable(F);
   // if (ResultPtr) return ResultPtr;

    // If this is an external function pointer, we can force the JIT to
    // 'compile' it, which really just adds it to the map.
   // if (isNonGhostDeclaration(F) || F->hasAvailableExternallyLinkage())
   //   return TheJIT->getPointerToFunction(F);
  }

  return 0;
}

void *Emitter::getPointerToGVIndirectSym(GlobalValue *V, void *Reference) {
  // Make sure GV is emitted first, and create a stub containing the fully
  // resolved address.
  void *GVAddress = getPointerToGlobal(V, Reference, false);
  //void *StubAddr = Resolver.getGlobalValueIndirectSym(V, GVAddress);
  return GVAddress;//StubAddr;
}

void Emitter::processDebugLoc(DebugLoc DL, bool BeforePrintingInsn) {
}

static unsigned GetConstantPoolSizeInBytes(MachineConstantPool *MCP,
                                           const TargetData *TD) {
  const std::vector<MachineConstantPoolEntry> &Constants = MCP->getConstants();
  if (Constants.empty()) return 0;

  unsigned Size = 0;
  for (unsigned i = 0, e = Constants.size(); i != e; ++i) {
    MachineConstantPoolEntry CPE = Constants[i];
    unsigned AlignMask = CPE.getAlignment() - 1;
    Size = (Size + AlignMask) & ~AlignMask;
    Type *Ty = CPE.getType();
    Size += TD->getTypeAllocSize(Ty);
  }
  return Size;
}

void Emitter::startFunction(MachineFunction &F) {
  DEBUG(dbgs() << "JIT: Starting CodeGen of Function "
        << F.getFunction()->getName() << "\n");

  uintptr_t ActualSize = 0;

  if (SizeEstimate > 0) {
    // SizeEstimate will be non-zero on reallocation attempts.
    ActualSize = SizeEstimate;
  }

  BufferBegin = CurBufferPtr = startFunctionBody(F.getFunction(), ActualSize);
  BufferEnd = BufferBegin+ActualSize;

  EmittedCode &code = EmittedFunctions[F.getFunction()];
  code.Function = F.getFunction();

  CurrentCode = &code;

  code.FunctionBody = BufferBegin;

  // Ensure the constant pool/jump table info is at least 4-byte aligned.
  emitAlignment(16);

  emitConstantPool(F.getConstantPool());
  if (MachineJumpTableInfo *MJTI = F.getJumpTableInfo())
    initJumpTableInfo(MJTI);

  // About to start emitting the machine code for the function.
  emitAlignment(std::max(F.getFunction()->getAlignment(), 8U));
  code.Code = CurBufferPtr;

  MBBLocations.clear();
}

bool Emitter::finishFunction(MachineFunction &F) {
  if (CurBufferPtr == BufferEnd) {
    // We must call endFunctionBody before retrying, because
    // deallocateMemForFunction requires it.
    endFunctionBody(F.getFunction(), BufferBegin, CurBufferPtr);
    retryWithMoreMemory(F);
    return true;
  }

  if (MachineJumpTableInfo *MJTI = F.getJumpTableInfo())
    emitJumpTableInfo(MJTI);

  CurrentCode->Size = CurBufferPtr - (uint8_t *)CurrentCode->Code;

  // CurBufferPtr may have moved beyond FnEnd, due to memory allocation for
  // global variables that were referenced in the relocations.
  endFunctionBody(F.getFunction(), BufferBegin, CurBufferPtr);

  if (CurBufferPtr == BufferEnd) {
    retryWithMoreMemory(F);
    return true;
  } else {
    // Now that we've succeeded in emitting the function, reset the
    // SizeEstimate back down to zero.
    SizeEstimate = 0;
  }

  BufferBegin = CurBufferPtr = 0;

  DEBUG(dbgs() << "JIT: Finished CodeGen of [" << (void*)CurrentCode->Code
        << "] Function: " << F.getFunction()->getName()
        << ": " << (CurrentCode->Size) << " bytes of text, "
        << CurrentCode->Relocations.size() << " relocations\n");

  ConstPoolAddresses.clear();

  return false;
}

void Emitter::resolveRelocations()
{
	for(auto code = EmittedFunctions.begin(); code != EmittedFunctions.end(); ++code)
	{
		if (!code->second.Relocations.empty()) {
		// Resolve the relocations to concrete pointers.
		for (unsigned i = 0, e = code->second.Relocations.size(); i != e; ++i) {
		  MachineRelocation &MR = code->second.Relocations[i];
		  void *ResultPtr = 0;
		  if (!MR.letTargetResolve()) {
			if (MR.isExternalSymbol()) {
				abort();
			  ResultPtr = engine.getPointerToNamedFunction(MR.getExternalSymbol(), false);
			  DEBUG(dbgs() << "JIT: Map \'" << MR.getExternalSymbol() << "\' to ["
						   << ResultPtr << "]\n");
			} else if (MR.isGlobalValue()) {
			  ResultPtr = getPointerToGlobal(MR.getGlobalValue(),
											 BufferBegin+MR.getMachineCodeOffset(),
											 MR.mayNeedFarStub());
			} else if (MR.isIndirectSymbol()) {
			  ResultPtr = getPointerToGVIndirectSym(
				  MR.getGlobalValue(), BufferBegin+MR.getMachineCodeOffset());
			} else if (MR.isBasicBlock()) {
			  ResultPtr = (void*)getMachineBasicBlockAddress(MR.getBasicBlock());
			} else if (MR.isConstantPoolIndex()) {
			  ResultPtr =
				(void*)getConstantPoolEntryAddress(MR.getConstantPoolIndex());
			} else {
			  assert(MR.isJumpTableIndex());
			  ResultPtr=(void*)getJumpTableEntryAddress(MR.getJumpTableIndex());
			}

			MR.setResultPointer(ResultPtr);
		  }
		}

		TM.getJITInfo()->relocate(code->second.FunctionBody, &code->second.Relocations[0], code->second.Relocations.size(), nullptr);
	  }

	  Shade::disassemble_code(code->second.Code, code->second.Code, code->second.Size);
	}
}

void Emitter::retryWithMoreMemory(MachineFunction &F) {
  DEBUG(dbgs() << "JIT: Ran out of space for native code.  Reattempting.\n");
  ConstPoolAddresses.clear();
  deallocateMemForFunction(F.getFunction());
  // Try again with at least twice as much free space.
  SizeEstimate = (uintptr_t)(2 * (BufferEnd - BufferBegin));
}

/// deallocateMemForFunction - Deallocate all memory for the specified
/// function body.  Also drop any references the function has to stubs.
/// May be called while the Function is being destroyed inside ~Value().
void Emitter::deallocateMemForFunction(const Function *F) {
  ValueMap<const Function *, EmittedCode, EmittedFunctionConfig>::iterator
    Emitted = EmittedFunctions.find(F);
  if (Emitted != EmittedFunctions.end()) {
    deallocateFunctionBody(Emitted->second.FunctionBody);

    EmittedFunctions.erase(Emitted);
  }
}


void *Emitter::allocateSpace(uintptr_t Size, unsigned Alignment) {
  if (BufferBegin)
    return JITCodeEmitter::allocateSpace(Size, Alignment);

  // create a new memory block if there is no active one.
  // care must be taken so that BufferBegin is invalidated when a
  // block is trimmed
  BufferBegin = CurBufferPtr = memAllocateSpace(Size, Alignment);
  BufferEnd = BufferBegin+Size;
  return CurBufferPtr;
}

void Emitter::emitConstantPool(MachineConstantPool *MCP) {
  const std::vector<MachineConstantPoolEntry> &Constants = MCP->getConstants();
  if (Constants.empty()) return;

  unsigned Size = GetConstantPoolSizeInBytes(MCP, &TD);
  unsigned Align = MCP->getConstantPoolAlignment();
  ConstantPoolBase = allocateSpace(Size, Align);
  ConstantPool = MCP;

  if (ConstantPoolBase == 0) return;  // Buffer overflow.

  DEBUG(dbgs() << "JIT: Emitted constant pool at [" << ConstantPoolBase
               << "] (size: " << Size << ", alignment: " << Align << ")\n");

  // Initialize the memory for all of the constant pool entries.
  unsigned Offset = 0;
  for (unsigned i = 0, e = Constants.size(); i != e; ++i) {
    MachineConstantPoolEntry CPE = Constants[i];
    unsigned AlignMask = CPE.getAlignment() - 1;
    Offset = (Offset + AlignMask) & ~AlignMask;

    uintptr_t CAddr = (uintptr_t)ConstantPoolBase + Offset;
    ConstPoolAddresses.push_back(CAddr);
    if (CPE.isMachineConstantPoolEntry()) {
      // FIXME: add support to lower machine constant pool values into bytes!
      report_fatal_error("Initialize memory with machine specific constant pool"
                        "entry has not been implemented!");
    }
    //TheJIT->InitializeMemory(CPE.Val.ConstVal, (void*)CAddr);
    DEBUG(dbgs() << "JIT:   CP" << i << " at [0x";
          dbgs().write_hex(CAddr) << "]\n");

    Type *Ty = CPE.Val.ConstVal->getType();
    Offset += TD.getTypeAllocSize(Ty);
  }
}

void Emitter::initJumpTableInfo(MachineJumpTableInfo *MJTI) {
  if (TM.getJITInfo()->hasCustomJumpTables())
    return;
  if (MJTI->getEntryKind() == MachineJumpTableInfo::EK_Inline)
    return;

  const std::vector<MachineJumpTableEntry> &JT = MJTI->getJumpTables();
  if (JT.empty()) return;

  unsigned NumEntries = 0;
  for (unsigned i = 0, e = JT.size(); i != e; ++i)
    NumEntries += JT[i].MBBs.size();

  unsigned EntrySize = MJTI->getEntrySize(TD);

  // Just allocate space for all the jump tables now.  We will fix up the actual
  // MBB entries in the tables after we emit the code for each block, since then
  // we will know the final locations of the MBBs in memory.
  JumpTable = MJTI;
  JumpTableBase = allocateSpace(NumEntries * EntrySize,
                             MJTI->getEntryAlignment(TD));
}

void Emitter::emitJumpTableInfo(MachineJumpTableInfo *MJTI) {
  if (TM.getJITInfo()->hasCustomJumpTables())
    return;

  const std::vector<MachineJumpTableEntry> &JT = MJTI->getJumpTables();
  if (JT.empty() || JumpTableBase == 0) return;


  switch (MJTI->getEntryKind()) {
  case MachineJumpTableInfo::EK_Inline:
    return;
  case MachineJumpTableInfo::EK_BlockAddress: {
    // EK_BlockAddress - Each entry is a plain address of block, e.g.:
    //     .word LBB123
    assert(MJTI->getEntrySize(TD) == sizeof(void*) &&
           "Cross JIT'ing?");

    // For each jump table, map each target in the jump table to the address of
    // an emitted MachineBasicBlock.
    intptr_t *SlotPtr = (intptr_t*)JumpTableBase;

    for (unsigned i = 0, e = JT.size(); i != e; ++i) {
      const std::vector<MachineBasicBlock*> &MBBs = JT[i].MBBs;
      // Store the address of the basic block for this jump table slot in the
      // memory we allocated for the jump table in 'initJumpTableInfo'
      for (unsigned mi = 0, me = MBBs.size(); mi != me; ++mi)
        *SlotPtr++ = getMachineBasicBlockAddress(MBBs[mi]);
    }
    break;
  }

  case MachineJumpTableInfo::EK_Custom32:
  case MachineJumpTableInfo::EK_GPRel32BlockAddress:
  case MachineJumpTableInfo::EK_LabelDifference32: {
    assert(MJTI->getEntrySize(TD) == 4&&"Cross JIT'ing?");
    // For each jump table, place the offset from the beginning of the table
    // to the target address.
    int *SlotPtr = (int*)JumpTableBase;

    for (unsigned i = 0, e = JT.size(); i != e; ++i) {
      const std::vector<MachineBasicBlock*> &MBBs = JT[i].MBBs;
      // Store the offset of the basic block for this jump table slot in the
      // memory we allocated for the jump table in 'initJumpTableInfo'
      uintptr_t Base = (uintptr_t)SlotPtr;
      for (unsigned mi = 0, me = MBBs.size(); mi != me; ++mi) {
        uintptr_t MBBAddr = getMachineBasicBlockAddress(MBBs[mi]);
        /// FIXME: USe EntryKind instead of magic "getPICJumpTableEntry" hook.
        *SlotPtr++ = TM.getJITInfo()->getPICJumpTableEntry(MBBAddr, Base);
      }
    }
    break;
  }
  case MachineJumpTableInfo::EK_GPRel64BlockAddress:
    llvm_unreachable(
           "JT Info emission not implemented for GPRel64BlockAddress yet.");
  }
}

void *Emitter::allocIndirectGV(const GlobalValue *GV,
                                  const uint8_t *Buffer, size_t Size,
                                  unsigned Alignment) {
	abort();
 uint8_t *IndGV =  0;//MemMgr->allocateStub(GV, Size, Alignment);
  memcpy(IndGV, Buffer, Size);
  return IndGV;
}

// getConstantPoolEntryAddress - Return the address of the 'ConstantNum' entry
// in the constant pool that was last emitted with the 'emitConstantPool'
// method.
//
uintptr_t Emitter::getConstantPoolEntryAddress(unsigned ConstantNum) const {
  assert(ConstantNum < ConstantPool->getConstants().size() &&
         "Invalid ConstantPoolIndex!");
  return ConstPoolAddresses[ConstantNum];
}

// getJumpTableEntryAddress - Return the address of the JumpTable with index
// 'Index' in the jumpp table that was last initialized with 'initJumpTableInfo'
//
uintptr_t Emitter::getJumpTableEntryAddress(unsigned Index) const {
  const std::vector<MachineJumpTableEntry> &JT = JumpTable->getJumpTables();
  assert(Index < JT.size() && "Invalid jump table index!");

  unsigned EntrySize = JumpTable->getEntrySize(TD);

  unsigned Offset = 0;
  for (unsigned i = 0; i < Index; ++i)
    Offset += JT[i].MBBs.size();

   Offset *= EntrySize;

  return (uintptr_t)((char *)JumpTableBase + Offset);
}

uint8_t *Emitter::startFunctionBody(const Function *F, uintptr_t &ActualSize)
{
	if(ActualSize == 0)
		ActualSize = 0x1000;

	void *result = std::malloc(ActualSize);

	return (uint8_t *)result;
}

void Emitter::endFunctionBody(const Function *F, uint8_t *FunctionStart, uint8_t *FunctionEnd)
{
}

uint8_t *Emitter::memAllocateSpace(intptr_t Size, unsigned Alignment)
{
	return (uint8_t *)std::malloc(Size);
}

void Emitter::deallocateFunctionBody(void *Body)
{
}

void *Emitter::allocateGlobal(uintptr_t Size, unsigned Alignment) {
	abort();
	return 0;
}

void Emitter::EmittedFunctionConfig::onDelete(
  Emitter *Emitter, const Function *F) {
  Emitter->deallocateMemForFunction(F);
}
void Emitter::EmittedFunctionConfig::onRAUW(
  Emitter *, const Function*, const Function*) {
  llvm_unreachable("The JIT doesn't know how to handle a"
                   " RAUW on a value it has emitted.");
}
};