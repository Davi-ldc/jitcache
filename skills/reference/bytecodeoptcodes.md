194 opcodes (the `Bytecode` section of `BytecodeList.rb`, each `op_group` expanded):

── arithmetic / logic ───────────────────────────────────────────────────────────
add  sub  mul  div  mod  pow  negate  inc  dec
bitand  bitor  bitxor  bitnot  lshift  rshift  urshift  unsigned
eq  neq  stricteq  nstricteq  eq_null  neq_null
less  lesseq  greater  greatereq  below  beloweq
not  typeof  instanceof

── type tests ───────────────────────────────────────────────────────────────────
is_empty  is_boolean  is_number  is_big_int  is_object  is_callable  is_constructor
is_undefined_or_null  is_cell_with_type  has_structure_with_flags
typeof_is_undefined  typeof_is_object  typeof_is_function

── coercion ─────────────────────────────────────────────────────────────────────
to_this  to_object  to_string  to_number  to_numeric  to_primitive
to_property_key  to_property_key_or_number  strcat

── properties ───────────────────────────────────────────────────────────────────
get_by_id  get_by_id_direct  get_by_id_with_this  get_length
put_by_id  put_by_id_with_this  del_by_id  in_by_id
get_by_val  get_by_val_with_this  put_by_val  put_by_val_direct
put_by_val_with_this  del_by_val  in_by_val
get_private_name  put_private_name  has_private_name
set_private_brand  check_private_brand  has_private_brand
put_getter_by_id  put_setter_by_id  put_getter_setter_by_id
put_getter_by_val  put_setter_by_val
define_data_property  define_accessor_property
get_prototype_of  get_internal_field  put_internal_field

── construction ─────────────────────────────────────────────────────────────────
new_object  new_array  new_array_with_size  new_array_buffer
new_array_with_spread  new_array_with_species  new_reg_exp  spread
create_this  create_promise  new_promise  create_generator  new_generator
create_async_generator  new_async_function_generator
new_func  new_func_exp  new_generator_func  new_generator_func_exp
new_async_func  new_async_func_exp  new_async_generator_func
new_async_generator_func_exp  set_function_name

── call and return ──────────────────────────────────────────────────────────────
call  call_ignore_result  call_varargs  call_direct_eval
tail_call  tail_call_varargs
construct  construct_varargs  super_construct  super_construct_varargs
ret  argument_count

── control flow ─────────────────────────────────────────────────────────────────
jmp  jtrue  jfalse  jeq  jneq  jstricteq  jnstricteq  jeq_ptr  jneq_ptr
jeq_null  jneq_null  jundefined_or_null  jnundefined_or_null
jless  jlesseq  jgreater  jgreatereq  jnless  jnlesseq  jngreater  jngreatereq
jbelow  jbeloweq  switch_imm  switch_char  switch_string
loop_hint  check_traps  throw  throw_static_error  catch

── scope and environment ────────────────────────────────────────────────────────
enter  get_scope  get_parent_scope  push_with_scope
resolve_scope  get_from_scope  put_to_scope
resolve_scope_for_hoisting_func_decl_in_eval
create_lexical_environment  create_generator_frame_environment
create_direct_arguments  create_scoped_arguments  create_cloned_arguments
create_rest  get_argument  get_from_arguments  put_to_arguments
check_tdz

── iteration ────────────────────────────────────────────────────────────────────
iterator_open  iterator_next  async_iterator_open  async_iterator_next
get_property_enumerator
enumerator_next  enumerator_get_by_val  enumerator_put_by_val
enumerator_in_by_val  enumerator_has_own_property
yield

── data movement and meta ───────────────────────────────────────────────────────
mov  identity_with_profile  nop  unreachable  debug
wide16  wide32  profile_type  profile_control_flow
super_sampler_begin  super_sampler_end
log_shadow_chicken_prologue  log_shadow_chicken_tail
