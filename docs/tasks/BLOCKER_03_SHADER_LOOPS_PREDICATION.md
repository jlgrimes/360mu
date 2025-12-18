# Task: Implement Shader Loops and Predication

## Priority: HIGH

## Problem

The shader translator in `native/src/gpu/xenos/shader_translator.cpp` has TODO stubs for critical control flow operations:

Lines 647-659:

```cpp
case xenos_cf::COND_EXEC:
case xenos_cf::COND_EXEC_END:
    // Conditional execution based on predicate
    // TODO: Implement predicated execution
    break;

case xenos_cf::LOOP_START:
    // TODO: Implement loop support
    break;

case xenos_cf::LOOP_END:
    // TODO: Implement loop support
    break;
```

Without these, shaders using loops (blur effects, skinning, shadows) will produce incorrect output or crash.

## Solution

Implement proper SPIR-V structured control flow for loops and conditional execution.

## Background: Xenos Shader Control Flow

Xbox 360 (Xenos) shaders use a control flow program separate from ALU/fetch instructions:

- `LOOP_START` / `LOOP_END` - Hardware loop with counter (aL register)
- `COND_EXEC` / `COND_EXEC_END` - Predicated execution based on boolean
- The loop counter is stored in address register aL (loop iteration)
- Loop constants define start, end, and step values

## Implementation Steps

### Step 1: Add loop tracking to TranslationContext

In `native/src/gpu/xenos/shader_translator.h`, add to TranslationContext struct (around line 200-250):

```cpp
struct TranslationContext {
    // ... existing fields ...

    // Loop tracking for SPIR-V structured control flow
    struct LoopInfo {
        u32 header_label;      // SPIR-V label for loop header
        u32 merge_label;       // SPIR-V label for loop merge (exit)
        u32 continue_label;    // SPIR-V label for continue target
        u32 counter_var;       // SPIR-V variable ID for loop counter
        u32 loop_const_idx;    // Which loop constant this uses
    };
    std::vector<LoopInfo> loop_stack;

    // Predicate tracking
    u32 predicate_var = 0;     // SPIR-V variable for predicate register
    bool in_predicated_block = false;
    u32 predicate_merge_label = 0;

    // Address register (aL) for loop iteration
    u32 address_register_var = 0;
};
```

### Step 2: Initialize loop/predicate variables

In the shader translation initialization (likely in `translate()` or `begin_translation()`), add:

```cpp
void ShaderTranslator::init_control_flow_vars(TranslationContext& ctx) {
    // Create predicate variable (bool)
    ctx.predicate_var = ctx.builder.create_variable(
        spv::StorageClass::Function,
        ctx.builder.type_bool()
    );

    // Create address register variable (int, for loop counter)
    ctx.address_register_var = ctx.builder.create_variable(
        spv::StorageClass::Function,
        ctx.builder.type_int(32, true)
    );

    // Initialize to 0
    ctx.builder.emit_store(ctx.predicate_var, ctx.builder.const_bool(false));
    ctx.builder.emit_store(ctx.address_register_var, ctx.builder.const_int(0));
}
```

### Step 3: Implement LOOP_START

Replace the TODO at line ~653 with:

```cpp
case xenos_cf::LOOP_START: {
    // Get loop constant index from CF instruction
    u32 loop_const_idx = cf.loop_id;  // Adjust based on actual CF struct

    // Create SPIR-V labels for structured loop
    u32 header_label = ctx.builder.create_label();
    u32 merge_label = ctx.builder.create_label();
    u32 continue_label = ctx.builder.create_label();
    u32 body_label = ctx.builder.create_label();

    // Get loop constant (contains start, count, step)
    // Loop constants are typically at indices 0-31
    u32 loop_const = get_loop_constant(ctx, loop_const_idx);
    u32 loop_count = (loop_const >> 0) & 0xFF;   // iteration count
    u32 loop_start = (loop_const >> 8) & 0xFF;   // start value
    u32 loop_step = (loop_const >> 16) & 0xFF;   // step (usually 1)

    // Initialize loop counter (aL) to start value
    ctx.builder.emit_store(ctx.address_register_var,
                           ctx.builder.const_int(loop_start));

    // Branch to header
    ctx.builder.emit_branch(header_label);
    ctx.builder.set_label(header_label);

    // Loop header - emit OpLoopMerge
    ctx.builder.emit_loop_merge(merge_label, continue_label,
                                 spv::LoopControlMask::MaskNone);
    ctx.builder.emit_branch(body_label);
    ctx.builder.set_label(body_label);

    // Push loop info onto stack
    TranslationContext::LoopInfo info;
    info.header_label = header_label;
    info.merge_label = merge_label;
    info.continue_label = continue_label;
    info.loop_const_idx = loop_const_idx;
    ctx.loop_stack.push_back(info);

    LOGD("LOOP_START: const=%u, count=%u, start=%u, step=%u",
         loop_const_idx, loop_count, loop_start, loop_step);
    break;
}
```

### Step 4: Implement LOOP_END

Replace the TODO at line ~657 with:

```cpp
case xenos_cf::LOOP_END: {
    if (ctx.loop_stack.empty()) {
        LOGE("LOOP_END without matching LOOP_START");
        break;
    }

    auto loop_info = ctx.loop_stack.back();
    ctx.loop_stack.pop_back();

    // Get loop constant for this loop
    u32 loop_const = get_loop_constant(ctx, loop_info.loop_const_idx);
    u32 loop_count = (loop_const >> 0) & 0xFF;
    u32 loop_step = (loop_const >> 16) & 0xFF;
    if (loop_step == 0) loop_step = 1;

    // Branch to continue block
    ctx.builder.emit_branch(loop_info.continue_label);
    ctx.builder.set_label(loop_info.continue_label);

    // Increment loop counter: aL = aL + step
    u32 current_al = ctx.builder.emit_load(ctx.address_register_var);
    u32 step_const = ctx.builder.const_int(loop_step);
    u32 new_al = ctx.builder.emit_iadd(current_al, step_const);
    ctx.builder.emit_store(ctx.address_register_var, new_al);

    // Compare: aL < (start + count * step)
    u32 loop_start = (loop_const >> 8) & 0xFF;
    u32 end_value = loop_start + loop_count * loop_step;
    u32 end_const = ctx.builder.const_int(end_value);
    u32 condition = ctx.builder.emit_slt(new_al, end_const);  // signed less than

    // Conditional branch: if (aL < end) goto header, else goto merge
    ctx.builder.emit_conditional_branch(condition,
                                         loop_info.header_label,
                                         loop_info.merge_label);

    // Continue after merge
    ctx.builder.set_label(loop_info.merge_label);

    LOGD("LOOP_END: merged at label %u", loop_info.merge_label);
    break;
}
```

### Step 5: Implement COND_EXEC (Predicated Execution)

Replace the TODO at line ~650 with:

```cpp
case xenos_cf::COND_EXEC:
case xenos_cf::COND_EXEC_PRED: {
    // Get predicate condition from CF instruction
    bool pred_condition = cf.predicate_condition;  // true or false branch

    // Load current predicate value
    u32 pred_value = ctx.builder.emit_load(ctx.predicate_var);

    // If we need the inverse, negate it
    if (!pred_condition) {
        pred_value = ctx.builder.emit_logical_not(pred_value);
    }

    // Create labels for if-then structure
    u32 then_label = ctx.builder.create_label();
    u32 merge_label = ctx.builder.create_label();

    // Emit selection merge (required for structured control flow)
    ctx.builder.emit_selection_merge(merge_label,
                                      spv::SelectionControlMask::MaskNone);

    // Conditional branch
    ctx.builder.emit_conditional_branch(pred_value, then_label, merge_label);
    ctx.builder.set_label(then_label);

    // Track that we're in a predicated block
    ctx.in_predicated_block = true;
    ctx.predicate_merge_label = merge_label;

    LOGD("COND_EXEC: predicate=%s", pred_condition ? "true" : "false");
    break;
}

case xenos_cf::COND_EXEC_END:
case xenos_cf::COND_EXEC_PRED_END: {
    if (!ctx.in_predicated_block) {
        LOGW("COND_EXEC_END without matching COND_EXEC");
        break;
    }

    // Branch to merge and set label
    ctx.builder.emit_branch(ctx.predicate_merge_label);
    ctx.builder.set_label(ctx.predicate_merge_label);

    ctx.in_predicated_block = false;
    ctx.predicate_merge_label = 0;

    LOGD("COND_EXEC_END: merged");
    break;
}
```

### Step 6: Add helper to get loop constants

Add this helper method to ShaderTranslator:

```cpp
u32 ShaderTranslator::get_loop_constant(TranslationContext& ctx, u32 index) {
    // Loop constants are typically passed as uniform data
    // Index into the loop constant buffer
    if (index >= 32) {
        LOGW("Loop constant index %u out of range", index);
        return 0x00010100;  // Default: 1 iteration, start 0, step 1
    }

    // Load from loop constant uniform buffer
    // The exact mechanism depends on how you pass constants to shaders
    // This might be a push constant, UBO, or embedded in the shader

    // For now, return a default that does 1 iteration
    // TODO: Hook up to actual loop constant data from GPU state
    return ctx.loop_constants[index];
}
```

### Step 7: Update SPIR-V builder

Ensure your SPIR-V builder in `native/src/gpu/xenos/spirv_builder.cpp` has these methods:

```cpp
// Loop merge instruction
void SpvBuilder::emit_loop_merge(u32 merge_label, u32 continue_label,
                                  spv::LoopControlMask control) {
    // OpLoopMerge merge_label continue_label control
    emit_instruction(spv::Op::OpLoopMerge, merge_label, continue_label,
                     static_cast<u32>(control));
}

// Selection merge instruction
void SpvBuilder::emit_selection_merge(u32 merge_label,
                                       spv::SelectionControlMask control) {
    // OpSelectionMerge merge_label control
    emit_instruction(spv::Op::OpSelectionMerge, merge_label,
                     static_cast<u32>(control));
}

// Conditional branch
void SpvBuilder::emit_conditional_branch(u32 condition, u32 true_label,
                                          u32 false_label) {
    // OpBranchConditional condition true_label false_label
    emit_instruction(spv::Op::OpBranchConditional, condition,
                     true_label, false_label);
}

// Logical NOT
u32 SpvBuilder::emit_logical_not(u32 operand) {
    u32 result = next_id();
    emit_instruction(spv::Op::OpLogicalNot, type_bool(), result, operand);
    return result;
}

// Signed less than comparison
u32 SpvBuilder::emit_slt(u32 a, u32 b) {
    u32 result = next_id();
    emit_instruction(spv::Op::OpSLessThan, type_bool(), result, a, b);
    return result;
}
```

## Testing

1. Find a shader that uses loops (common in blur effects, shadow mapping)
2. Enable shader debug logging to see LOOP_START/LOOP_END being processed
3. Verify the generated SPIR-V validates with spirv-val
4. Visual test: blur effects should look correct instead of broken

## Files to Modify

- `native/src/gpu/xenos/shader_translator.h` - Add LoopInfo struct, tracking fields
- `native/src/gpu/xenos/shader_translator.cpp` - Implement LOOP_START, LOOP_END, COND_EXEC
- `native/src/gpu/xenos/spirv_builder.cpp` - Add emit methods for control flow
- `native/src/gpu/xenos/spirv_builder.h` - Declare new emit methods

## Dependencies

None - this task is independent of other blockers.

## Notes

- SPIR-V requires structured control flow (no arbitrary gotos)
- All loops must have OpLoopMerge before the header
- All conditionals must have OpSelectionMerge before the branch
- The loop counter (aL) is the "address register" in Xenos terminology
- Loop constants define iteration parameters per-loop
