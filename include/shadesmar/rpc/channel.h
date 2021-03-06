/* MIT License

Copyright (c) 2020 Dheeraj R Reddy

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
==============================================================================*/

#ifndef INCLUDE_SHADESMAR_RPC_CHANNEL_H_
#define INCLUDE_SHADESMAR_RPC_CHANNEL_H_

#include <cstdint>
#include <cstring>

#include <atomic>
#include <iostream>
#include <string>

#include <msgpack.hpp>

#include "shadesmar/concurrency/cond_var.h"
#include "shadesmar/concurrency/lock.h"
#include "shadesmar/concurrency/scope.h"
#include "shadesmar/memory/memory.h"

#define RPC_QUEUE_SIZE 32

namespace shm::rpc {

struct ChannelElem : public memory::Element {
  concurrent::PthreadWriteLock mutex;
  concurrent::CondVar cond;
  std::atomic<bool> ready{};

  ChannelElem() : Element() {
    mutex = concurrent::PthreadWriteLock();
    cond = concurrent::CondVar();
    ready.store(false);
  }

  ChannelElem(const ChannelElem &channel_elem) : Element(channel_elem) {
    mutex = channel_elem.mutex;
    cond = channel_elem.cond;
    ready.store(channel_elem.ready.load());
  }
};

class Channel : public memory::Memory<ChannelElem, RPC_QUEUE_SIZE> {
  using ScopeGuardT = concurrent::ScopeGuard<concurrent::PthreadWriteLock,
                                             concurrent::EXCLUSIVE>;

 public:
  Channel(const std::string &name, bool caller)
      : Memory<ChannelElem, RPC_QUEUE_SIZE>(name), caller_(caller), idx_(0) {}

  bool write(void *data, size_t size);
  bool read(msgpack::object_handle *oh);

  bool _write_caller(void *data, size_t size);
  bool _write_server(void *data, size_t size);
  void _write(ChannelElem *elem, void *data, size_t size);
  inline __attribute__((always_inline)) uint32_t fetch_add_counter() {
    return this->shared_queue_->counter.fetch_add(1);
  }
  inline __attribute__((always_inline)) uint32_t counter() {
    return this->shared_queue_->counter.load();
  }

  int32_t idx_;

 private:
  bool caller_;
};

bool Channel::write(void *data, size_t size) {
  if (size > this->raw_buf_->get_free_memory()) {
    std::cerr << "Increase max_buffer_size" << std::endl;
    return false;
  }

  if (caller_) {
    return _write_caller(data, size);
  } else {
    return _write_server(data, size);
  }
}

bool Channel::_write_caller(void *data, size_t size) {
  idx_ = this->fetch_add_counter();
  uint32_t pos = idx_ & (RPC_QUEUE_SIZE - 1);
  ChannelElem &elem = this->shared_queue_->elements[pos];

  bool exp = true;
  if (!elem.empty.compare_exchange_strong(exp, false)) {
    return false;
  }
  ScopeGuardT _(&elem.mutex);

  _write(&elem, data, size);
  elem.ready = false;

  return true;
}

bool Channel::_write_server(void *data, size_t size) {
  uint32_t pos = idx_ & (RPC_QUEUE_SIZE - 1);
  ChannelElem &elem = this->shared_queue_->elements[pos];

  ScopeGuardT _(&elem.mutex);
  this->raw_buf_->deallocate(
      this->raw_buf_->get_address_from_handle(elem.addr_hdl));

  _write(&elem, data, size);
  elem.ready = true;
  idx_++;
  return true;
}

void Channel::_write(ChannelElem *elem, void *data, size_t size) {
  void *new_addr = this->raw_buf_->allocate(size);
  std::memcpy(new_addr, data, size);
  elem->addr_hdl = this->raw_buf_->get_handle_from_address(new_addr);
  elem->size = size;
}

bool Channel::read(msgpack::object_handle *oh) {
  uint32_t pos = idx_ & (RPC_QUEUE_SIZE - 1);
  ChannelElem &elem = this->shared_queue_->elements[pos];

  if (caller_) {
    bool exp = true;
    if (!elem.ready.compare_exchange_strong(exp, false)) {
      return false;
    }
  }

  ScopeGuardT _(&elem.mutex);

  const char *dst = reinterpret_cast<const char *>(
      this->raw_buf_->get_address_from_handle(elem.addr_hdl));

  *oh = msgpack::unpack(dst, elem.size);

  if (caller_) {
    elem.empty = true;
  }

  return true;
}

}  // namespace shm::rpc

#endif  // INCLUDE_SHADESMAR_RPC_CHANNEL_H_
