mechanical

Requirement. SPEC-image.md section 9, the paragraph before step 1: "It reads only the `BaselineJITCode`, its record, its MathICs, its executable memory up to the linked sizes, and the UCB's identifiers, constants, arithmetic profiles and switch tables."

Why the code cannot meet it as written. The section's own steps read more than that list:

- Step 3 classifies a mold identifier that is not a UCB identifier by "the immortal name with that impl (`vm.propertyNames`)", which reads the VM's property names in every mode.
- S1 and S2 decode each fixup "to the producer's resolution of its target", and `resolveTarget` reads the VM's thunks (`VM::getCTIStub`, under `JITThunks::m_lock`, which the same paragraph allows), its small strings and `structureIDBase()`.
- S1 follows "unconditional `b` instructions through jump islands", which lie in the executable pool outside the image's linked bytes.
- The UCB is reached through the CB the glue passes, `CodeBlock::unlinkedCodeBlock()`.

What the code does instead. `captureImage` (`jitcache/ImageCapture.cpp`) runs the steps as written: `immortalNameFor` in step 3, `resolveTarget` through `ImageCaptureInternal::ProducerResolution` and the island walk of `ImageCaptureInternal::footprintReachesTarget` under strict, and the UCB from `codeBlock.unlinkedCodeBlock()`. The sentence's purpose still holds: none of these reads changes while JS is paused, since property names and thunks are immortal for the VM, and `FixedVMPoolExecutableAllocator` keys each island by the location of the jump that uses it and frees it only with the code holding that jump (`handleWillBeReleased`), which the capture's `Ref<BaselineJITCode>` keeps alive. The SPEC only has to add these reads to the list. Nothing is asked of other tasks.

Evidence. `JSC::JITCache::captureImage`, `ImageCaptureInternal::footprintReachesTarget` and `ImageCaptureInternal::ProducerResolution` in `jitcache/ImageCapture.cpp`; `immortalNameFor` in `jitcache/ImageSection.cpp`; `resolveTarget` and `resolveSupport` in `jitcache/ImageSupport.cpp`; `FixedVMPoolExecutableAllocator::islandForJumpLocation` and `handleWillBeReleased` in `jit/ExecutableAllocator.cpp`.
