#pragma once
#include <cstddef>
#include <cassert>
#include <algorithm>
#include <memory>

template <typename T, size_t SMALL_SIZE>
struct socow_vector {
  using iterator = T*;
  using const_iterator = T const*;

  socow_vector() : size_(0) {}

  socow_vector(socow_vector const& other) : size_(other.size_) {
    if (other.is_small()) {
      safe_init_array(other.small_data, small_data, size());
    } else {
      large_data = make_new_ref(other.large_data);
    }
  }

  socow_vector& operator=(socow_vector const& other) {
    if (this != &other) {
      socow_vector copy(other);
      swap(copy);
    }
    return *this;
  }

  ~socow_vector() {
    if (is_small()) {
      destruct_data();
    } else {
      release_owned_large_data();
    }
  }

  T& operator[](size_t i) {
    return *(begin() + i);
  }

  T const& operator[](size_t i) const {
    return *(begin() + i);
  }

  size_t size() const {
    return size_ & ~STATE_BIT;
  }

  T* data() {
    if (is_small()) {
      return small_data;
    } else {
      if (!is_unique()) {
        large_data_header* new_data =
            alloc_large_data(large_data->data_,size(),large_data->capacity_);
        release_owned_large_data();
        large_data = new_data;
      }
      return large_data->data_;
    }
  }

  T const* data() const {
    if (is_small()) {
      return small_data;
    } else {
      return large_data->data_;
    }
  }

  T& front() {
    return *begin();
  }

  T const& front() const {
    return *begin();
  }

  T& back() {
    return *(begin() + size() - 1);
  }

  T const& back() const {
    return *(begin() + size() - 1);
  }

  void push_back(T const& value) {
    size_t cur_size = size();
    size_t cur_cap = capacity();

    if (cur_size == cur_cap) {
      large_data_header* new_data =
          alloc_large_data(std::as_const(*this).data(), cur_size, 2 * cur_cap + 1);
      try {
        new (new_data->data_ + cur_size) T(value);
      } catch (...) {
        release_large_data(new_data, cur_size);
        throw;
      }
      this->~socow_vector();
      large_data = new_data;
      set_large_state();
    } else {
      new (data() + cur_size) T(value);
    }
    size_++;
  }

  void pop_back() {
    data()[size() - 1].~T();
    --size_;
  }

  bool empty() const {
    return size() == 0;
  }

  size_t capacity() const {
    return is_small() ? SMALL_SIZE : large_data->capacity_;
  }

  void reserve(size_t new_capacity) {
    if ((!is_unique() && new_capacity > size()) || new_capacity > capacity()) {
      resize(new_capacity);
    }
  }

  void shrink_to_fit() {
    if (!is_small() && size() != large_data->capacity_) {
      resize(size());
    }
  }

  void clear() {
    if (is_unique()) {
      destruct_data();
      size_ &= STATE_BIT;
    } else {
      release_owned_large_data();
      large_data = alloc_large_data(nullptr, 0, capacity());
      size_ = STATE_BIT;
    }
  }

  void swap(socow_vector& other) {
    if (this == &other) {
      return;
    }
    if (is_small()) {
      if (other.is_small()) {
        size_t cnt = std::min(size(), other.size());
        for (size_t i = 0; i < cnt; i++) {
          std::swap(small_data[i], other.small_data[i]);
        }
        T* dst = small_data;
        T* src = other.small_data;
        if (size() > other.size()) {
          std::swap(src, dst);
        }
        for (size_t i = std::max(size(), other.size()); i > cnt; i--) {
          new (dst + i - 1) T(*(src + i - 1));
          (src + i - 1)->~T();
        }
      } else {
        socow_vector copy(other);
        other.copy_to_small_data(small_data, size());
        destruct_data();
        large_data = make_new_ref(copy.large_data);
      }
      std::swap(size_, other.size_);
    }
    else if (!other.is_small()){
      std::swap(large_data, other.large_data);
      std::swap(size_, other.size_);
    } else {
      other.swap(*this);
    }
  }

  iterator begin() {
    return data();
  }

  iterator end() {
    return data() + size();
  }

  const_iterator begin() const {
    return data();
  }

  const_iterator end() const {
    return data() + size();
  }

  iterator insert(const_iterator pos, T const& value) {
    size_t index = pos - std::as_const(*this).begin();
    push_back(value);
    pos = begin() + index;
    iterator wpos = end() - 1;
    for (; wpos != pos; wpos--) {
      std::swap(*wpos, *(wpos - 1));
    }
    return wpos;
  }

  iterator erase(const_iterator pos) {
    return erase(pos, pos + 1);
  }

  iterator erase(const_iterator first, const_iterator last) {
    ptrdiff_t ind = first - std::as_const(*this).begin();
    ptrdiff_t len = last - first;
    iterator pos = begin() + ind + len;
    iterator res = end();
    for (; pos != end(); pos++) {
      res = pos - len;
      std::swap(*pos, *(pos - len));
    }
    destruct_array(begin() + (size() - len), len);
    size_ -= len;
    return res;
  }

private:
  struct large_data_header {
    size_t capacity_;
    size_t refcnt;
    T data_[0];
  };

  static constexpr uint64_t STATE_BIT = 1ull << 63;

  // Highest bit of size_ encodes small/large state: 0 for small, 1 for large
  uint64_t size_;
  union {
    T small_data[SMALL_SIZE];
    large_data_header* large_data;
  };

  bool is_small() const {
    return (size_ & STATE_BIT) == 0ull;
  }

  void switch_size_state() {
    size_ ^= STATE_BIT;
  }

  void set_large_state() {
    size_ |= STATE_BIT;
  }

  void resize(size_t new_capacity) {
    if (new_capacity <= SMALL_SIZE) {
      assert(!is_small());
      copy_to_small_data(large_data->data_, size());
      switch_size_state();
      return;
    }
    large_data_header* ndata =
        alloc_large_data(data(), size(), new_capacity);
    this->~socow_vector();
    large_data = ndata;
    if (is_small()) {
      switch_size_state();
    }
  }

  bool is_unique() {
    return is_small() || large_data->refcnt == 1;
  }

  void release_owned_large_data() {
    release_large_data(large_data, size());
  }

  static void release_large_data(large_data_header* large_data, size_t size) {
    if (--large_data->refcnt == 0) {
      destruct_array(large_data->data_, size);
      operator delete(large_data);
    }
  }

  static void destruct_array(T* src, size_t cnt) {
    for (size_t i = cnt; i > 0; i--) {
      src[i - 1].~T();
    }
  }

  static void safe_init_array(T const* src, T* dst, size_t cnt) {
    size_t i = 0;
    try {
      for (; i < cnt; i++) {
        new (dst + i) T(src[i]);
      }
    } catch (...) {
      destruct_array(dst, i);
      throw;
    }
  }

  void destruct_data() {
    destruct_array(data(), size());
  }

  static large_data_header*
  alloc_large_data(T const* src, size_t size, size_t capacity) {
    assert(size <= capacity);
    void* buffer = operator new(sizeof(large_data_header) + capacity * sizeof(T));
    large_data_header* large_data = new (buffer) large_data_header{capacity, 1};
    try {
      safe_init_array(src, reinterpret_cast<T*>(large_data->data_), size);
    } catch (...) {
      operator delete(buffer);
      throw;
    }
    return large_data;
  }

  static large_data_header* make_new_ref(large_data_header* large_data) {
    large_data->refcnt++;
    return large_data;
  }

  void copy_to_small_data(T const* begin, size_t cnt) {
    assert(!is_small());
    large_data_header* old_data = make_new_ref(large_data);
    release_owned_large_data();
    try {
      safe_init_array(begin, small_data, cnt);
    } catch (...) {
      large_data = old_data;
      throw;
    }
    release_large_data(old_data, size());
  }
};
