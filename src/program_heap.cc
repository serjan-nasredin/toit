// Copyright (C) 2018 Toitware ApS.
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; version
// 2.1 only.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// The license can be found in the file `LICENSE` in the top level
// directory of this repository.

#include "program_heap.h"

#include "flags.h"
#include "heap_report.h"
#include "interpreter.h"
#include "os.h"
#include "primitive.h"
#include "printing.h"
#include "process.h"
#include "program_memory.h"
#include "scheduler.h"
#include "utils.h"
#include "vm.h"

#include "objects_inline.h"

#ifdef TOIT_ESP32
#include "esp_heap_caps.h"
#endif

namespace toit {

ProgramHeap::ProgramHeap(Program* program)
    : ProgramRawHeap()
    , program_(program)
    , retrying_primitive_(false)
    , total_bytes_allocated_(0)
    , last_allocation_result_(ALLOCATION_SUCCESS) {
  blocks_.append(ProgramBlock::allocate_program_block());
}

ProgramHeap::~ProgramHeap() {
  set_writable(true);
  blocks_.free_blocks(this);
}

Instance* ProgramHeap::allocate_instance(Smi* class_id) {
  int size = program()->instance_size_for(class_id);
  TypeTag class_tag = program()->class_tag_for(class_id);
  return allocate_instance(class_tag, class_id, Smi::from(size));
}

Instance* ProgramHeap::allocate_instance(TypeTag class_tag, Smi* class_id, Smi* instance_size) {
  Instance* result = unvoid_cast<Instance*>(_allocate_raw(Smi::value(instance_size)));
  if (result == null) return null;  // Allocation failure.
  // Initialize object.
  result->_set_header(class_id, class_tag);
  return result;
}

Array* ProgramHeap::allocate_array(int length, Object* filler) {
  ASSERT(length >= 0);
  ASSERT(length <= Array::max_length_in_program());
  HeapObject* result = _allocate_raw(Array::allocation_size(length));
  if (result == null) {
    return null;  // Allocation failure.
  }
  // Initialize object.
  result->_set_header(program_, program_->array_class_id());
  Array::cast(result)->_initialize_no_write_barrier(length, filler);
  return Array::cast(result);
}

Array* ProgramHeap::allocate_array(int length) {
  ASSERT(length >= 0);
  ASSERT(length <= Array::max_length_in_program());
  HeapObject* result = _allocate_raw(Array::allocation_size(length));
  if (result == null) {
    return null;  // Allocation failure.
  }
  // Initialize object.
  result->_set_header(program_, program_->array_class_id());
  Array::cast(result)->_initialize(length);
  return Array::cast(result);
}

ByteArray* ProgramHeap::allocate_internal_byte_array(int length) {
  ASSERT(length >= 0);
  // Byte array should fit within one heap block.
  ASSERT(length <= ByteArray::max_internal_size_in_program());
  ByteArray* result = unvoid_cast<ByteArray*>(_allocate_raw(ByteArray::internal_allocation_size(length)));
  if (result == null) return null;  // Allocation failure.
  // Initialize object.
  result->_set_header(program_, program_->byte_array_class_id());
  result->_initialize(length);
  return result;
}

Double* ProgramHeap::allocate_double(double value) {
  HeapObject* result = _allocate_raw(Double::allocation_size());
  if (result == null) return null;  // Allocation failure.
  // Initialize object.
  result->_set_header(program_, program_->double_class_id());
  Double::cast(result)->_initialize(value);
  return Double::cast(result);
}

LargeInteger* ProgramHeap::allocate_large_integer(int64 value) {
  HeapObject* result = _allocate_raw(LargeInteger::allocation_size());
  if (result == null) return null;  // Allocation failure.
  // Initialize object.
  result->_set_header(program_, program_->large_integer_class_id());
  LargeInteger::cast(result)->_initialize(value);
  return LargeInteger::cast(result);
}

int ProgramHeap::payload_size() {
  return blocks_.payload_size();
}

String* ProgramHeap::allocate_internal_string(int length) {
  ASSERT(length >= 0);
  ASSERT(length <= String::max_internal_size_in_program());
  HeapObject* result = _allocate_raw(String::internal_allocation_size(length));
  if (result == null) return null;
  // Initialize object.
  Smi* string_id = program()->string_class_id();
  result->_set_header(string_id, program()->class_tag_for(string_id));
  String::cast(result)->_set_length(length);
  String::cast(result)->_raw_set_hash_code(String::NO_HASH_CODE);
  String::MutableBytes bytes(String::cast(result));
  bytes._set_end();
  ASSERT(bytes.length() == length);
  return String::cast(result);
}

void ProgramHeap::migrate_to(Program* program) {
  set_writable(false);
  program->take_blocks(&blocks_);
}

HeapObject* ProgramHeap::_allocate_raw(int byte_size) {
  ASSERT(byte_size > 0);
  ASSERT(byte_size <= ProgramBlock::max_payload_size());
  HeapObject* result = blocks_.last()->allocate_raw(byte_size);
  if (result == null) {
    AllocationResult expand_result = _expand();
    set_last_allocation_result(expand_result);
    if (expand_result != ALLOCATION_SUCCESS) return null;
    result = blocks_.last()->allocate_raw(byte_size);
  }
  if (result == null) return null;
  total_bytes_allocated_ += byte_size;
  return result;
}

ProgramHeap::AllocationResult ProgramHeap::_expand() {
  ProgramBlock* block = ProgramBlock::allocate_program_block();
  blocks_.append(block);
  return ALLOCATION_SUCCESS;
}

String* ProgramHeap::allocate_string(const char* str) {
  return allocate_string(str, strlen(str));
}

String* ProgramHeap::allocate_string(const char* str, int length) {
  bool can_fit_in_heap_block = length <= String::max_internal_size_in_program();
  String* result;
  if (can_fit_in_heap_block) {
    result = allocate_internal_string(length);
    // We are in the program heap. We should never run out of memory.
    ASSERT(result != null);
    // Initialize object.
    String::MutableBytes bytes(result);
    bytes._initialize(str);
  } else {
    result = allocate_external_string(length, const_cast<uint8*>(unsigned_cast(str)));
  }
  result->hash_code();  // Ensure hash_code is computed at creation.
  return result;
}

ByteArray* ProgramHeap::allocate_byte_array(const uint8* data, int length) {
  if (length > ByteArray::max_internal_size_in_program()) {
    auto result = allocate_external_byte_array(length, const_cast<uint8*>(data));
    // We are on the program heap which should never run out of memory.
    ASSERT(result != null);
    return result;
  }
  auto byte_array = allocate_internal_byte_array(length);
  // We are on the program heap which should never run out of memory.
  ASSERT(byte_array != null);
  ByteArray::Bytes bytes(byte_array);
  if (length != 0) memcpy(bytes.address(), data, length);
  return byte_array;
}

ByteArray* ProgramHeap::allocate_external_byte_array(int length, uint8* memory) {
  ByteArray* result = unvoid_cast<ByteArray*>(_allocate_raw(ByteArray::external_allocation_size()));
  if (result == null) return null;  // Allocation failure.
  // Initialize object.
  result->_set_header(program_, program_->byte_array_class_id());
  result->_initialize_external_memory(length, memory, false);
  return result;
}

String* ProgramHeap::allocate_external_string(int length, uint8* memory) {
  String* result = unvoid_cast<String*>(_allocate_raw(String::external_allocation_size()));
  if (result == null) return null;  // Allocation failure.
  // Initialize object.
  result->_set_header(program(), program()->string_class_id());
  result->_set_external_length(length);
  result->_raw_set_hash_code(String::NO_HASH_CODE);
  result->_set_external_address(memory);
  ASSERT(!result->content_on_heap());
  if (memory[length] != '\0') {
    // TODO(florian): we should not have '\0' at the end of strings anymore.
    String::MutableBytes bytes(String::cast(result));
    bytes._set_end();
  }
  return result;
}

// We initialize lazily - this is because the number of objects can grow during
// iteration.
ProgramHeap::Iterator::Iterator(ProgramBlockList& list, Program* program)
  : list_(list)
  , iterator_(list.end())  // Set to null.
  , block_(null)
  , current_(null)
  , program_(program) {}

bool ProgramHeap::Iterator::eos() {
  return list_.is_empty()
      || (block_ == null
          ? list_.first()->is_empty()
          :  (current_ >= block_->top() && block_ == list_.last()));
}

void ProgramHeap::Iterator::ensure_started() {
  ASSERT(!eos());
  if (block_ == null) {
     iterator_ = list_.begin();
     block_ = *iterator_;
     current_ = block_->base();
  }
}

HeapObject* ProgramHeap::Iterator::current() {
  ensure_started();
  if (current_ >= block_->top() && block_ != list_.last()) {
    block_ = *++iterator_;
    current_ = block_->base();
  }
  ASSERT(!block_->is_empty());
  return HeapObject::cast(current_);
}

void ProgramHeap::Iterator::advance() {
  ensure_started();

  ASSERT(is_smi(HeapObject::cast(current_)->header()));  // Header is not a forwarding pointer.
  current_ = Utils::address_at(current_, HeapObject::cast(current_)->size(program_));
  if (current_ >= block_->top() && block_ != list_.last()) {
    block_ = *++iterator_;
    current_ = block_->base();
    ASSERT(!block_->is_empty());
  }
}

}
